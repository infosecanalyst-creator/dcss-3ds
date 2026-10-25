/**
 * @file
 * @brief Nintendo 3DS console backend for Dungeon Crawl Stone Soup.
 *
 * Implements the ~30-function console interface in libconsole.h (the same
 * contract libw32c.cc and libunix.cc fulfil) on top of libctru.
 *
 * Crawl believes it is drawing into an 80x24 terminal. This file keeps that
 * grid and, at blit time, routes each cell to a screen by the region it
 * belongs to (crawl_view.viewp/hudp/msgp/mlistp):
 *
 *   Top screen (400x240)
 *     - dungeon viewport, 10x10 px cells (the 5x10 font doubled in width)
 *       drawn in depth layers for the 3D slider: floor behind the plane,
 *       walls/items on it, monsters and @ in front
 *     - message pane, 80 cols x 7 rows at 5x10 (exactly 400 px wide)
 *   Bottom screen (320x240)
 *     - HUD + monster list at 5x10 in the left 210 px
 *     - command buttons on the right; a full touch keyboard overlay (toggle
 *       with the KB button or SELECT)
 *
 * Any crawl screen that is not the dungeon (menus, inventory, the level
 * map, ...) blanks the viewport: those full-screen interfaces are detected
 * by the viewport region being overwritten with non-map text, and the whole
 * 80x24 grid is then shown on the top screen at 5x10 instead ("CRT mode").
 * Nothing in crawl proper needs to change for any of this.
**/

#include "AppHdr.h"

#if defined(TARGET_OS_3DS) && !defined(USE_TILE_LOCAL)

// crawl (mon-flags.h) and libctru (types.h) both define BIT()
#undef BIT
#include <3ds.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>

#include "cio.h"
#include "defines.h"
#include "libutil.h"
#include "options.h"
#include "state.h"
#include "unicode.h"
#include "version.h"
#include "viewgeom.h"
#include "view.h"

#include "font5x10.h"

// newlib has no process ownership; SQLite (contrib) references these in a
// code path that is never taken without a real filesystem owner model.
extern "C" {
uid_t geteuid(void) { return 0; }
int fchown(int, uid_t, gid_t) { return 0; }
}

// ---------------------------------------------------------------------------
// Text grid
// ---------------------------------------------------------------------------

static const int GRID_W = 80;   // 80 * 5  = 400 px  (top screen width)
static const int GRID_H = 24;   // 24 * 10 = 240 px  (top screen height)

struct cell_t
{
    char32_t glyph;
    uint8_t  fg;
    uint8_t  bg;
    uint8_t  dirty;
};

static cell_t  grid[GRID_H][GRID_W];
static bool    grid_all_dirty = true;
static int     cx = 0, cy = 0;            // cursor, 0-based
static bool    cursor_is_enabled = false;
static bool    inited = false;

static COLOURS FG_COL = LIGHTGREY;
static COLOURS BG_COL = BLACK;
static uint8_t current_fg = LIGHTGREY;
static uint8_t current_bg = BLACK;

// Classic 16-colour palette (BGR order is handled at blit time).
static const uint8_t palette[16][3] =
{
    {  0,   0,   0}, // BLACK
    {  0,   0, 170}, // BLUE
    {  0, 170,   0}, // GREEN
    {  0, 170, 170}, // CYAN
    {170,   0,   0}, // RED
    {170,   0, 170}, // MAGENTA
    {170,  85,   0}, // BROWN
    {170, 170, 170}, // LIGHTGREY
    { 85,  85,  85}, // DARKGREY
    { 85,  85, 255}, // LIGHTBLUE
    { 85, 255,  85}, // LIGHTGREEN
    { 85, 255, 255}, // LIGHTCYAN
    {255,  85,  85}, // LIGHTRED
    {255,  85, 255}, // LIGHTMAGENTA
    {255, 255,  85}, // YELLOW
    {255, 255, 255}, // WHITE
};

// ---------------------------------------------------------------------------
// Font
// ---------------------------------------------------------------------------

static int font_index(char32_t c)
{
    // font_cp is sorted; binary search.
    int lo = 0, hi = FONT_NGLYPHS - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (font_cp[mid] == c)
            return mid;
        if (font_cp[mid] < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static const uint8_t *glyph_bits(char32_t c)
{
    int i = font_index(c);
    if (i < 0)
        i = font_index('?');
    return font_bits[i];
}

// ---------------------------------------------------------------------------
// Framebuffer helpers (BGR8, portrait-rotated: fb[(x*H + (H-1-y)) * 3])
// ---------------------------------------------------------------------------

static inline void put_px(u8 *fb, int fbh, int x, int y,
                          uint8_t r, uint8_t g, uint8_t b)
{
    u8 *p = fb + 3 * (x * fbh + (fbh - 1 - y));
    p[0] = b; p[1] = g; p[2] = r;
}

static void draw_glyph(u8 *fb, int fbh, int px, int py, char32_t c,
                       const uint8_t *fg, const uint8_t *bg, int scale = 1)
{
    const uint8_t *bits = glyph_bits(c);
    for (int y = 0; y < FONT_H; ++y)
    {
        const uint8_t row = bits[y];
        for (int x = 0; x < FONT_W; ++x)
        {
            const bool on = row & (1 << (FONT_W - 1 - x));
            const uint8_t *col = on ? fg : bg;
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx)
                    put_px(fb, fbh, px + x * scale + sx, py + y * scale + sy,
                           col[0], col[1], col[2]);
        }
    }
}

// 10x10 map cell: the glyph doubled horizontally.
static void draw_glyph_wide(u8 *fb, int fbh, int px, int py, char32_t c,
                            const uint8_t *fg, const uint8_t *bg)
{
    const uint8_t *bits = glyph_bits(c);
    for (int y = 0; y < FONT_H; ++y)
    {
        const uint8_t row = bits[y];
        for (int x = 0; x < FONT_W; ++x)
        {
            const bool on = row & (1 << (FONT_W - 1 - x));
            const uint8_t *col = on ? fg : bg;
            const int xx = px + x * 2, yy = py + y;
            if (xx < 0 || xx + 1 >= 400 || yy < 0 || yy >= 240)
                continue;
            put_px(fb, fbh, xx, yy, col[0], col[1], col[2]);
            put_px(fb, fbh, xx + 1, yy, col[0], col[1], col[2]);
        }
    }
}

static void fill_rect(u8 *fb, int fbh, int x0, int y0, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x)
            put_px(fb, fbh, x, y, r, g, b);
}

static void draw_string(u8 *fb, int fbh, int px, int py, const char *s,
                        const uint8_t *fg, const uint8_t *bg)
{
    char32_t c;
    while (int taken = utf8towc(&c, s))
    {
        s += taken;
        draw_glyph(fb, fbh, px, py, c, fg, bg);
        px += FONT_W;
    }
}

// ---------------------------------------------------------------------------
// Layout: which screen does a grid cell belong to?
// ---------------------------------------------------------------------------

enum Region { RG_VIEW, RG_MSG, RG_HUD, RG_MLIST, RG_OTHER };

// "CRT mode": crawl is showing a full-screen interface (menu, inventory,
// level map...). Detected when text lands in the viewport that did not
// come through puttext(). Shown as the plain 80x24 grid on the top screen.
static bool crt_mode = false;
static bool in_puttext = false;
static int  crt_quiet_frames = 0;
static bool keyboard_overlay = false;

static Region region_of(int x, int y)
{
    const crawl_view_geometry &v = crawl_view;
    const int gx = x + 1, gy = y + 1; // crawl regions are 1-based
    auto inside = [&](coord_def p, coord_def sz) {
        return gx >= p.x && gx < p.x + sz.x && gy >= p.y && gy < p.y + sz.y;
    };
    if (inside(v.viewp, v.viewsz)) return RG_VIEW;
    if (inside(v.msgp, v.msgsz)) return RG_MSG;
    if (inside(v.hudp, v.hudsz)) return RG_HUD;
    if (inside(v.mlistp, v.mlistsz)) return RG_MLIST;
    return RG_OTHER;
}

// Depth class of a map glyph for the stereoscopic layers.
enum { DL_FLOOR = 0, DL_PLANE = 1, DL_ACTOR = 2, DL_COUNT = 3 };
static const float kParallaxPx = 3.0f; // per level at full slider

static int depth_of_glyph(char32_t g)
{
    if (g == ' ' || g == '.' || g == ',' || g == 0x00B7 || g == 0x2219 || g == '~'
        || g == 0x2248 || g == '\'' || g == '`' || g == 0x2591)
        return DL_FLOOR;                               // floor, water, lava, shallow
    if (g == '@' || (g >= 'a' && g <= 'z') || (g >= 'A' && g <= 'Z') || g == '&'
        || g == ';' || (g >= '1' && g <= '9'))
        return DL_ACTOR;                               // monsters, player, uniques
    return DL_PLANE;                                   // walls, doors, items, stairs
}

// ---------------------------------------------------------------------------
// Top screen
// ---------------------------------------------------------------------------

static void draw_crt(u8 *fb, int H)
{
    fill_rect(fb, H, 0, 0, 400, 240, 0, 0, 0);
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
        {
            const cell_t &c = grid[y][x];
            const bool at_cursor = cursor_is_enabled && x == cx && y == cy;
            const uint8_t *fg = palette[c.fg & 15];
            const uint8_t *bg = palette[c.bg & 15];
            if (at_cursor)
                std::swap(fg, bg);
            draw_glyph(fb, H, x * FONT_W, y * FONT_H, c.glyph, fg, bg);
        }
}

// eye: 0 = mono, -1 left, +1 right
static void draw_play_top(u8 *fb, int H, int eye, float slider)
{
    const crawl_view_geometry &v = crawl_view;
    fill_rect(fb, H, 0, 0, 400, 240, 0, 0, 0);

    // --- viewport: 10x10 cells, centred in the 170 px band above the messages
    const int vw = v.viewsz.x, vh = v.viewsz.y;
    const int x0 = (400 - vw * 10) / 2;
    const int y0 = (170 - vh * 10) / 2;
    for (int layer = 0; layer < DL_COUNT; ++layer)
    {
        int dx = 0;
        if (eye != 0 && slider > 0.f)
        {
            const float par = (float)(layer - DL_PLANE) * kParallaxPx * slider;
            dx = -(int)(eye * par + (eye * par >= 0 ? 0.5f : -0.5f));
        }
        for (int r = 0; r < vh; ++r)
            for (int c = 0; c < vw; ++c)
            {
                const int gx = v.viewp.x - 1 + c, gy = v.viewp.y - 1 + r;
                if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H)
                    continue;
                const cell_t &cell = grid[gy][gx];
                const int d = depth_of_glyph(cell.glyph);
                if (d != layer)
                {
                    // floor shows through under elevated cells so parallax
                    // never opens a hole
                    if (layer == DL_FLOOR)
                    {
                        const uint8_t *bg = palette[cell.bg & 15];
                        fill_rect(fb, H, x0 + c * 10 + dx, y0 + r * 10, 10, 10, bg[0], bg[1], bg[2]);
                    }
                    continue;
                }
                const bool at_cursor = cursor_is_enabled && gx == cx && gy == cy;
                const uint8_t *fg = palette[cell.fg & 15];
                const uint8_t *bg = palette[cell.bg & 15];
                if (at_cursor)
                    std::swap(fg, bg);
                draw_glyph_wide(fb, H, x0 + c * 10 + dx, y0 + r * 10, cell.glyph, fg, bg);
            }
    }

    // --- messages: 80 cols at 5x10 across the bottom 70 px
    const int my0 = 240 - v.msgsz.y * FONT_H;
    for (int r = 0; r < v.msgsz.y; ++r)
        for (int c = 0; c < v.msgsz.x && c < GRID_W; ++c)
        {
            const int gx = v.msgp.x - 1 + c, gy = v.msgp.y - 1 + r;
            if (gy < 0 || gy >= GRID_H)
                continue;
            const cell_t &cell = grid[gy][gx];
            const bool at_cursor = cursor_is_enabled && gx == cx && gy == cy;
            const uint8_t *fg = palette[cell.fg & 15];
            const uint8_t *bg = palette[cell.bg & 15];
            if (at_cursor)
                std::swap(fg, bg);
            draw_glyph(fb, H, c * FONT_W, my0 + r * FONT_H, cell.glyph, fg, bg);
        }
    fill_rect(fb, H, 0, my0 - 1, 400, 1, 60, 60, 70);
}

static bool any_dirty()
{
    if (grid_all_dirty)
        return true;
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
            if (grid[y][x].dirty)
                return true;
    return false;
}

static void clear_dirty()
{
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
            grid[y][x].dirty = 0;
    grid_all_dirty = false;
}

static float slider_state()
{
    return osGet3DSliderState();
}

static void blit_top()
{
    if (!any_dirty())
        return;
    u16 fbw, fbh;
    u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
    const int H = fbw; // 240
    const float slider = crt_mode ? 0.f : slider_state();

    if (crt_mode)
    {
        draw_crt(fb, H);
        u8 *fbr = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &fbw, &fbh);
        draw_crt(fbr, H);
    }
    else
    {
        draw_play_top(fb, H, slider > 0.f ? -1 : 0, slider);
        u8 *fbr = gfxGetFramebuffer(GFX_TOP, GFX_RIGHT, &fbw, &fbh);
        draw_play_top(fbr, H, slider > 0.f ? 1 : 0, slider);
    }
    gfxFlushBuffers();
}

// ---------------------------------------------------------------------------
// Bottom screen: touch keyboard
// ---------------------------------------------------------------------------

struct softkey
{
    int x, y, w, h;      // pixel rect on the bottom screen
    const char *label;   // what to draw
    int key;             // crawl key to emit (0 = modifier/special)
    int shifted;         // key when Shift is sticky (0 = toupper/none)
    int special;         // 1=shift 2=ctrl 3=none
};

enum { SKEY_NONE = 0, SKEY_SHIFT = 1, SKEY_CTRL = 2 };

static const int KEY_W = 26, KEY_H = 34, KEY_GAP = 0;
static const int KB_X0 = 4, KB_Y0 = 22;

// Row layouts: (unshifted, shifted)
static const char *row1 = "1234567890-=";
static const char *row1s = "!@#$%^&*()_+";
static const char *row2 = "qwertyuiop[]";
static const char *row2s = "QWERTYUIOP{}";
static const char *row3 = "asdfghjkl;'\\";
static const char *row3s = "ASDFGHJKL:\"|";
static const char *row4 = "zxcvbnm,./`";
static const char *row4s = "ZXCVBNM<>?~";

static std::vector<softkey> softkeys;
static bool  sticky_shift = false;
static bool  sticky_ctrl  = false;
static int   pressed_key_index = -1;
static bool  bottom_dirty = true;

static void build_keyboard()
{
    softkeys.clear();
    auto add_row = [](int row, const char *keys, const char *skeys, int x0)
    {
        for (int i = 0; keys[i]; ++i)
        {
            softkey k;
            k.x = x0 + i * KEY_W; k.y = KB_Y0 + row * KEY_H;
            k.w = KEY_W; k.h = KEY_H;
            static char labels[64][2];
            static int nlabel = 0;
            char *lab = labels[nlabel++ % 64];
            lab[0] = keys[i]; lab[1] = 0;
            k.label = lab; k.key = keys[i]; k.shifted = skeys[i];
            k.special = SKEY_NONE;
            softkeys.push_back(k);
        }
    };
    add_row(0, row1, row1s, KB_X0);
    add_row(1, row2, row2s, KB_X0);
    add_row(2, row3, row3s, KB_X0);
    add_row(3, row4, row4s, KB_X0 + KEY_W / 2);

    // Bottom row of wide keys.
    const int by = KB_Y0 + 4 * KEY_H;
    const softkey bottom[] =
    {
        { KB_X0,            by, 44, KEY_H, "Shft", 0,         0, SKEY_SHIFT },
        { KB_X0 + 44,       by, 40, KEY_H, "Ctrl", 0,         0, SKEY_CTRL  },
        { KB_X0 + 84,       by, 36, KEY_H, "Esc",  ESCAPE,    0, SKEY_NONE  },
        { KB_X0 + 120,      by, 36, KEY_H, "Tab",  CK_TAB,    0, SKEY_NONE  },
        { KB_X0 + 156,      by, 64, KEY_H, "Space",' ',       0, SKEY_NONE  },
        { KB_X0 + 220,      by, 40, KEY_H, "Bksp", CK_BKSP,   0, SKEY_NONE  },
        { KB_X0 + 260,      by, 52, KEY_H, "Enter",CK_ENTER,  0, SKEY_NONE  },
    };
    for (const softkey &k : bottom)
        softkeys.push_back(k);
}

struct cmdbtn { int x, y, w, h; const char *label; int key; int special; };
enum { CB_NONE = 0, CB_KEYBOARD = 1 };
static const cmdbtn cmdbtns[] =
{
    { 214,   2, 50, 24, "explr", 'o',      CB_NONE },
    { 268,   2, 50, 24, "fight", CK_TAB,   CB_NONE },
    { 214,  30, 50, 24, "rest",  '5',      CB_NONE },
    { 268,  30, 50, 24, "s.rch", 's',      CB_NONE },
    { 214,  58, 50, 24, "pick",  'g',      CB_NONE },
    { 268,  58, 50, 24, "inv",   'i',      CB_NONE },
    { 214,  86, 50, 24, "map",   'X',      CB_NONE },
    { 268,  86, 50, 24, "trav",  'G',      CB_NONE },
    { 214, 114, 50, 24, "pray",  'p',      CB_NONE },
    { 268, 114, 50, 24, "cast",  'z',      CB_NONE },
    { 214, 142, 50, 24, "esc",   ESCAPE,   CB_NONE },
    { 268, 142, 50, 24, "enter", CK_ENTER, CB_NONE },
    { 214, 176, 104, 26, "keyboard", 0,    CB_KEYBOARD },
    { 214, 208, 50, 24, "up <",  '<',      CB_NONE },
    { 268, 208, 50, 24, "dn >",  '>',      CB_NONE },
};
static const int ncmdbtns = sizeof(cmdbtns) / sizeof(cmdbtns[0]);
static int pressed_cmd = -1;

static void draw_keyboard(u8 *fb, int H)
{
    static const uint8_t bg[3]     = { 24,  24,  28};
    static const uint8_t keybg[3]  = { 52,  52,  60};
    static const uint8_t keyhi[3]  = {110, 110, 140};
    static const uint8_t stick[3]  = { 70, 110,  70};
    static const uint8_t fg[3]     = {220, 220, 220};
    static const uint8_t dim[3]    = {140, 140, 150};

    fill_rect(fb, H, 0, 0, 320, 240, bg[0], bg[1], bg[2]);
    char status[64];
    snprintf(status, sizeof(status), "%s%s  SELECT/KB: back to HUD",
             sticky_shift ? "[SHIFT] " : "", sticky_ctrl ? "[CTRL] " : "");
    draw_string(fb, H, 4, 6, status, dim, bg);

    for (size_t i = 0; i < softkeys.size(); ++i)
    {
        const softkey &k = softkeys[i];
        const uint8_t *kb = keybg;
        if ((int)i == pressed_key_index)
            kb = keyhi;
        else if ((k.special == SKEY_SHIFT && sticky_shift)
                 || (k.special == SKEY_CTRL && sticky_ctrl))
            kb = stick;
        fill_rect(fb, H, k.x + 1, k.y + 1, k.w - 2, k.h - 2, kb[0], kb[1], kb[2]);

        const char *label = k.label;
        char one[2] = {0, 0};
        if (k.special == SKEY_NONE && k.shifted && sticky_shift)
        {
            one[0] = (char)k.shifted;
            label = one;
        }
        const int lw = (int)strlen(label) * FONT_W;
        draw_string(fb, H, k.x + (k.w - lw) / 2, k.y + (k.h - FONT_H) / 2,
                    label, fg, kb);
    }
}

static void draw_hud_bottom(u8 *fb, int H)
{
    static const uint8_t bg[3]    = { 0, 0, 0 };
    static const uint8_t panel[3] = { 24, 24, 30 };
    static const uint8_t keybg[3] = { 52, 52, 60 };
    static const uint8_t keyhi[3] = { 110, 110, 140 };
    static const uint8_t fg[3]    = { 220, 220, 220 };

    fill_rect(fb, H, 0, 0, 320, 240, bg[0], bg[1], bg[2]);
    const crawl_view_geometry &v = crawl_view;

    // HUD then monster list, stacked in the left column at 5x10
    int row = 0;
    auto draw_region = [&](coord_def p, coord_def sz, int maxrows) {
        for (int r = 0; r < sz.y && row < maxrows; ++r, ++row)
            for (int c = 0; c < sz.x && c < 42; ++c)
            {
                const int gx = p.x - 1 + c, gy = p.y - 1 + r;
                if (gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H)
                    continue;
                const cell_t &cell = grid[gy][gx];
                draw_glyph(fb, H, c * FONT_W, row * FONT_H, cell.glyph,
                           palette[cell.fg & 15], palette[cell.bg & 15]);
            }
    };
    draw_region(v.hudp, v.hudsz, 24);
    draw_region(v.mlistp, v.mlistsz, 24);

    // command buttons
    fill_rect(fb, H, 210, 0, 110, 240, panel[0], panel[1], panel[2]);
    for (int i = 0; i < ncmdbtns; ++i)
    {
        const cmdbtn &b = cmdbtns[i];
        const uint8_t *kb = (i == pressed_cmd) ? keyhi : keybg;
        fill_rect(fb, H, b.x, b.y, b.w, b.h, kb[0], kb[1], kb[2]);
        const int lw = (int)strlen(b.label) * FONT_W;
        draw_string(fb, H, b.x + (b.w - lw) / 2, b.y + (b.h - FONT_H) / 2, b.label, fg, kb);
    }
}

static void draw_bottom()
{
    u16 fbw, fbh;
    u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbw, &fbh);
    const int H = fbw; // 240
    if (keyboard_overlay || crt_mode)
        draw_keyboard(fb, H);
    else
        draw_hud_bottom(fb, H);
    gfxFlushBuffers();
    bottom_dirty = false;
}

static int cmdbtn_at(int px, int py)
{
    for (int i = 0; i < ncmdbtns; ++i)
    {
        const cmdbtn &b = cmdbtns[i];
        if (px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h)
            return i;
    }
    return -1;
}

static int softkey_at(int px, int py)
{
    for (size_t i = 0; i < softkeys.size(); ++i)
    {
        const softkey &k = softkeys[i];
        if (px >= k.x && px < k.x + k.w && py >= k.y && py < k.y + k.h)
            return (int)i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static std::deque<int> keyq;

static int apply_modifiers(int key)
{
    if (key <= 0)
        return key; // CK_* codes: modifiers don't apply
    if (sticky_shift && key >= 'a' && key <= 'z')
        key = toupper(key);
    if (sticky_ctrl && isalpha(key))
        key = CONTROL(toupper(key));
    sticky_shift = sticky_ctrl = false;
    bottom_dirty = true;
    return key;
}

// Physical button map. Edit here to taste.
struct keymap_3ds
{
    u32 button;
    int key;          // plain
    int key_with_l;   // when L is held (0 = same)
};

static const keymap_3ds button_map[] =
{
    { KEY_DUP,    CK_UP,    CK_SHIFT_UP    },
    { KEY_DDOWN,  CK_DOWN,  CK_SHIFT_DOWN  },
    { KEY_DLEFT,  CK_LEFT,  CK_SHIFT_LEFT  },
    { KEY_DRIGHT, CK_RIGHT, CK_SHIFT_RIGHT },
    { KEY_A,      CK_ENTER, CK_ENTER       },
    { KEY_B,      ESCAPE,   ESCAPE         },
    { KEY_X,      'o',      'G'            },   // explore / travel
    { KEY_Y,      CK_TAB,   'p'            },   // autofight / pray
    { KEY_R,      '5',      's'            },   // rest / wait a turn
    { KEY_ZL,     'i',      'I'            },   // inventory / spell list
    { KEY_ZR,     'g',      ','            },   // pick up
    { KEY_START,  'S',      'S'            },   // save and quit
};

// Circle pad -> 8-way numpad with auto-repeat.
static int cpad_dir_key(const circlePosition &cp)
{
    const int dead = 40;
    const int dx = cp.dx, dy = cp.dy;
    if (dx * dx + dy * dy < dead * dead)
        return 0;
    const bool right = dx >  dead / 2 + abs(dy) / 3;
    const bool left  = dx < -dead / 2 - abs(dy) / 3;
    const bool up    = dy >  dead / 2 + abs(dx) / 3;
    const bool down  = dy < -dead / 2 - abs(dx) / 3;
    if (up && right)   return CK_NUMPAD_9;
    if (up && left)    return CK_NUMPAD_7;
    if (down && right) return CK_NUMPAD_3;
    if (down && left)  return CK_NUMPAD_1;
    if (up)            return CK_NUMPAD_8;
    if (down)          return CK_NUMPAD_2;
    if (left)          return CK_NUMPAD_4;
    if (right)         return CK_NUMPAD_6;
    return 0;
}

static u64  cpad_next_ms = 0;
static int  cpad_last_key = 0;

static void poll_input()
{
    hidScanInput();
    const u32 down = hidKeysDown();
    const u32 held = hidKeysHeld();
    const u32 rep  = hidKeysDownRepeat();
    const bool lmod = held & KEY_L;

    // SELECT toggles the touch keyboard overlay
    if (down & KEY_SELECT)
    {
        keyboard_overlay = !keyboard_overlay;
        bottom_dirty = true;
    }

    for (const keymap_3ds &m : button_map)
    {
        const bool is_dir = m.button & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
        if ((is_dir ? rep : down) & m.button)
        {
            int k = (lmod && m.key_with_l) ? m.key_with_l : m.key;
            keyq.push_back(apply_modifiers(k));
        }
    }

    // circle pad
    circlePosition cp;
    hidCircleRead(&cp);
    const int dk = cpad_dir_key(cp);
    const u64 now = osGetTime();
    if (dk == 0)
        cpad_last_key = 0;
    else if (dk != cpad_last_key || now >= cpad_next_ms)
    {
        keyq.push_back(dk);
        cpad_next_ms = now + (dk != cpad_last_key ? 260 : 110);
        cpad_last_key = dk;
    }

    // touch: command buttons (HUD view) or the keyboard overlay
    if (down & KEY_TOUCH)
    {
        touchPosition tp;
        hidTouchRead(&tp);
        bottom_dirty = true;
        if (!keyboard_overlay && !crt_mode)
        {
            const int b = cmdbtn_at(tp.px, tp.py);
            pressed_cmd = b;
            if (b >= 0)
            {
                if (cmdbtns[b].special == CB_KEYBOARD)
                    keyboard_overlay = true;
                else
                    keyq.push_back(apply_modifiers(cmdbtns[b].key));
            }
        }
        else
        {
        const int i = softkey_at(tp.px, tp.py);
        pressed_key_index = i;
        if (i >= 0)
        {
            const softkey &k = softkeys[i];
            if (k.special == SKEY_SHIFT)
                sticky_shift = !sticky_shift;
            else if (k.special == SKEY_CTRL)
                sticky_ctrl = !sticky_ctrl;
            else
            {
                int key = k.key;
                if (k.shifted && sticky_shift)
                {
                    key = k.shifted;
                    sticky_shift = false;
                }
                if (sticky_ctrl && isalpha(key))
                {
                    key = CONTROL(toupper(key));
                    sticky_ctrl = false;
                }
                keyq.push_back(key);
            }
        }
        }
    }
    if ((hidKeysUp() & KEY_TOUCH) && (pressed_key_index >= 0 || pressed_cmd >= 0))
    {
        pressed_key_index = -1;
        pressed_cmd = -1;
        bottom_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Early init (called from main() before anything touches the filesystem)
// ---------------------------------------------------------------------------

static bool gfx_inited = false;
static bool headless = false;

bool in_headless_mode() { return headless; }
void enter_headless_mode() { headless = true; }

void lib3ds_early_init()
{
    gfxInitDefault();
    gfx_inited = true;
    // Debug console on the bottom screen so startup errors (missing data,
    // bad rc file...) are visible; console_startup() takes it over later.
    consoleInit(GFX_BOTTOM, nullptr);
    printf("DCSS 3DS: starting\n");

    Result rc = romfsInit();
    if (R_FAILED(rc))
        printf("romfsInit failed: %08X\n", (unsigned)rc);

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/crawl", 0777);
    mkdir("sdmc:/3ds/crawl/saves", 0777);
    mkdir("sdmc:/3ds/crawl/morgue", 0777);

    // Give the rest of startup the New 3DS clock.
    osSetSpeedupEnable(true);
}

// ---------------------------------------------------------------------------
// libconsole.h implementation
// ---------------------------------------------------------------------------

void console_startup()
{
    if (inited)
        return;
    if (!gfx_inited)
    {
        gfxInitDefault();
        gfx_inited = true;
    }
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, false);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    gfxSet3D(true);
    osSetSpeedupEnable(true);           // New 3DS: 804 MHz + L2
    crt_mode = true;
    hidSetRepeatParameters(18, 5);      // frames: initial delay, repeat

    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
            grid[y][x] = { ' ', LIGHTGREY, BLACK, 1 };
    grid_all_dirty = true;

    build_keyboard();
    bottom_dirty = true;

    textcolour(LIGHTGREY);
    cursor_is_enabled = false;
    inited = true;

    crawl_state.terminal_resize_handler = nullptr;
    crawl_state.terminal_resize_check   = nullptr;

    draw_bottom();
    blit_top();
}

void console_shutdown()
{
    if (!inited)
        return;
    inited = false;
    romfsExit();
    gfxExit();
    gfx_inited = false;
}

bool is_cursor_enabled()
{
    return cursor_is_enabled;
}

void set_cursor_enabled(bool enabled)
{
    if (enabled == cursor_is_enabled)
        return;
    cursor_is_enabled = enabled;
    if (cy < GRID_H && cx < GRID_W)
        grid[cy][cx].dirty = 1;
}

void set_mouse_enabled(bool /*enabled*/)
{
}

void clear_to_end_of_line()
{
    for (int x = cx; x < GRID_W; ++x)
    {
        grid[cy][x] = { ' ', current_fg, current_bg, 1 };
    }
}

void clrscr_sys()
{
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
            grid[y][x] = { ' ', current_fg, current_bg, 1 };
    cx = cy = 0;
    grid_all_dirty = true;
}

void gotoxy_sys(int x, int y)
{
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    if (x > GRID_W) x = GRID_W;
    if (y > GRID_H) y = GRID_H;
    if (cursor_is_enabled)
        grid[cy][cx].dirty = 1;
    cx = x - 1;
    cy = y - 1;
    if (cursor_is_enabled)
        grid[cy][cx].dirty = 1;
}

// --- colour / highlight handling, same logic as the DOS/Win32 backends ---

static unsigned short _dos_reverse_highlight(unsigned short colour)
{
    if (colour == BLACK)
        colour = (DARKGREY << 4);
    else
        colour = (colour & 0xF) << 4;
    return colour;
}

static unsigned short _dos_do_highlight(unsigned short colour,
                                        unsigned short highlite)
{
    if (!highlite)
        return colour;
    if (colour == highlite)
        colour = 0;
    colour |= (highlite << 4);
    return colour;
}

static unsigned short _dos_highlight(unsigned short colour, unsigned highlight)
{
    if ((highlight & CHATTR_ATTRMASK) == CHATTR_NORMAL)
        return colour;
    colour &= 0xFF;
    if ((highlight & CHATTR_ATTRMASK) == CHATTR_HILITE)
        return _dos_do_highlight(colour, (highlight & CHATTR_COLMASK) >> 8);
    else
        return _dos_reverse_highlight(colour);
}

static inline unsigned get_highlight(int col)
{
    return ((col & COLFLAG_UNUSUAL_MASK) == COLFLAG_UNUSUAL_MASK) ?
                                              Options.unusual_highlight :
           (col & COLFLAG_FRIENDLY_MONSTER) ? Options.friend_highlight :
           (col & COLFLAG_NEUTRAL_MONSTER)  ? Options.neutral_highlight :
           (col & COLFLAG_ITEM_HEAP)        ? Options.heap_highlight :
           (col & COLFLAG_WILLSTAB)         ? Options.stab_highlight :
           (col & COLFLAG_MAYSTAB)          ? Options.may_stab_highlight :
           (col & COLFLAG_FEATURE_ITEM)     ? Options.feature_item_highlight :
           (col & COLFLAG_TRAP_ITEM)        ? Options.trap_item_highlight :
           (col & COLFLAG_REVERSE)          ? unsigned{CHATTR_REVERSE}
                                            : unsigned{CHATTR_NORMAL};
}

static void update_text_colours(int highlight)
{
    unsigned short highlighted_bg_fg = _dos_highlight(FG_COL, highlight);
    const bool highlight_overrides_bg = highlighted_bg_fg & 0xF0;
    const short fg = highlighted_bg_fg & 0x0F;
    const short bg = highlight_overrides_bg ? (highlighted_bg_fg & 0xF0) >> 4
                                            : BG_COL;
    current_fg = (uint8_t)Options.colour[fg];
    current_bg = (uint8_t)Options.colour[bg];
}

void textcolour(int c)
{
    FG_COL = static_cast<COLOURS>(c & 0xF);
    update_text_colours(get_highlight(c));
}

void textbackground(int c)
{
    BG_COL = static_cast<COLOURS>(c & 0xF);
    update_text_colours(get_highlight(c));
}

COLOURS default_hover_colour()
{
    return DARKGREY;
}

lib_display_info::lib_display_info()
    : type("3DS"),
      term("N/A"),
      fg_colors(16),
      bg_colors(16)
{
}

static void write_char(char32_t c)
{
    if (c == '\n')
    {
        cx = 0;
        if (cy < GRID_H - 1)
            ++cy;
        return;
    }
    if (c == '\r')
    {
        cx = 0;
        return;
    }
    if (c == '\t')
    {
        for (int i = 0; i < 8; ++i)
            write_char(' ');
        return;
    }
    grid[cy][cx] = { c, current_fg, current_bg, 1 };
    // Text landing in the viewport that did not come from puttext() means
    // crawl is drawing a full-screen interface over the map (menus,
    // inventory, level map, the start screen...). Switch to CRT mode until
    // puttext() paints the map again.
    if (!in_puttext && !crt_mode && c != ' ' && region_of(cx, cy) == RG_VIEW)
    {
        crt_mode = true;
        grid_all_dirty = true;
        bottom_dirty = true;
    }
    // like libw32c: the cursor parks on the last column, never past it
    if (++cx >= GRID_W)
        cx = GRID_W - 1;
}

static void cprintf_aux(const char *s)
{
    if (!inited)
    {
        printf("%s", s);
        return;
    }
    char32_t c;
    while (int taken = utf8towc(&c, s))
    {
        s += taken;
        write_char(c);
    }
}

void cprintf(const char *format, ...)
{
    va_list argp;
    char buffer[4096];
    va_start(argp, format);
    vsnprintf(buffer, sizeof(buffer), format, argp);
    cprintf_aux(buffer);
    va_end(argp);
}

int wherex()
{
    return cx + 1;
}

int wherey()
{
    return cy + 1;
}

void putwch(char32_t c)
{
    if (c == 0)
        c = ' ';
    write_char(c);
}

int getch_ck()
{
    update_screen();

    while (keyq.empty())
    {
        if (crawl_state.seen_hups)
            return ESCAPE;
        if (!aptMainLoop())
        {
            // HOME -> close: treat like a hangup so crawl saves and exits.
            crawl_state.seen_hups = true;
            return ESCAPE;
        }
        poll_input();
        if (bottom_dirty)
            draw_bottom();
        gspWaitForVBlank();
    }
    const int k = keyq.front();
    keyq.pop_front();
    return k;
}

bool kbhit()
{
    if (crawl_state.seen_hups)
        return false;
    if (keyq.empty())
    {
        if (!aptMainLoop())
        {
            crawl_state.seen_hups = true;
            return false;
        }
        poll_input();
        if (bottom_dirty)
            draw_bottom();
    }
    return !keyq.empty();
}

void delay(unsigned int ms)
{
    if (crawl_state.disables[DIS_DELAY])
        return;
    update_screen();
    svcSleepThread((s64)ms * 1000000LL);
}

void puttext(int x1, int y1, const crawl_view_buffer &vbuf)
{
    // The dungeon view arrives here wholesale; a buffer that matches the
    // viewport ends CRT mode. Anything else (the full-screen level map, the
    // arena...) is a full-screen interface and keeps/enters CRT mode.
    const coord_def size = vbuf.size();
    const bool is_viewport = x1 == crawl_view.viewp.x && y1 == crawl_view.viewp.y
                             && size == crawl_view.viewsz;
    in_puttext = is_viewport;
    if (crt_mode == is_viewport)
    {
        crt_mode = !is_viewport;
        grid_all_dirty = true;
        bottom_dirty = true;
    }
    const screen_cell_t *cell = vbuf;
    for (int y = 0; y < size.y; ++y)
    {
        cgotoxy(x1, y1 + y);
        for (int x = 0; x < size.x; ++x)
        {
            textcolour(cell->colour);
            putwch(cell->glyph);
            cell++;
        }
    }
    textcolour(WHITE);
    in_puttext = false;
}

void update_screen()
{
    if (!inited)
        return;
    if (any_dirty())
        bottom_dirty = true;
    blit_top();
    if (bottom_dirty)
        draw_bottom();
    clear_dirty();
}

int get_number_of_lines()
{
    return GRID_H;
}

int get_number_of_cols()
{
    return GRID_W;
}

int num_to_lines(int num)
{
    return num;
}

#endif // TARGET_OS_3DS && !USE_TILE_LOCAL
