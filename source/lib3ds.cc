/**
 * @file
 * @brief Nintendo 3DS frontend for Dungeon Crawl Stone Soup.
 *
 * Implements the console interface in libconsole.h (the same contract
 * libunix.cc and libw32c.cc fulfil) on top of libctru, and draws a tiles
 * presentation from the tile data crawl computes when built with
 * USE_TILE_WEB (each viewport cell arrives in puttext() with a packed_cell).
 *
 *   Bottom (touch) screen -- the game:
 *     the dungeon as 16 px tiles (Crawl's own tile art), 20 x 15 cells
 *     centred on the player so the whole line of sight fits; translucent
 *     button columns at the two edges (outside line of sight). Tap a square
 *     to travel there / attack, tap yourself to pick up or use stairs.
 *   Top screen -- the dashboard:
 *     crawl's status panel (name, HP/MP, stats, status lights) in a 6x13
 *     font, the monsters in view with their tiles, and the message pane.
 *
 * Any crawl screen that is not the dungeon (menus, inventory, the level map,
 * character screens...) is drawn into the 80x24 text grid; those are shown
 * on the top screen in a 5x8 font ("text mode") while the bottom screen
 * offers the menu's entries as big touch rows plus Esc/Enter/page buttons.
 *
 * Crawl proper only needs USE_TILE_WEB turned on; nothing here talks to a
 * websocket -- the tile data is read straight from the view buffer.
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
#include <algorithm>
#include <deque>
#include <string>
#include <vector>
#include <sqlite3.h>

// libctru's default main-thread stack is only 32 KB; crawl builds a whole
// `player` (and other big objects) on the stack. Give it 4 MB.
extern "C" { u32 __stacksize__ = 4 * 1024 * 1024; }

#include "cio.h"
#include "command.h"
#include "defines.h"
#include "directn.h"
#include "env.h"
#include "libutil.h"
#include "message.h"
#include "package.h"
#include "mon-info.h"
#include "options.h"
#include "player.h"
#include "state.h"
#include "terrain.h"
#include "travel.h"
#include "unicode.h"
#include "format.h"
#include <climits>
#ifdef SOUND_PLAY_3DS
void lib3ds_play_sound(const char *file);   // defined below
#endif
// message.cc (patched): newest messages with colours and turn numbers
vector<pair<formatted_string, int>> lib3ds_recent_messages(int mcount);
#include "version.h"
#include "viewgeom.h"
#include "view.h"

#ifdef USE_TILE
#include "tilecell.h"
#include "tiledoll.h"
#include "tilemcache.h"
#include "tilepick.h"
#include "tiles.h"
#include "rltiles/tiledef-dngn.h"
#include "rltiles/tiledef-gui.h"
#include "rltiles/tiledef-icons.h"
#include "rltiles/tiledef-main.h"
#include "rltiles/tiledef-player.h"
#include "rltiles/status-icon-sizes.h"
#endif

#include <map>
#include <math.h>
#include "font5x8.h"
#include "font6x13.h"
#include "font6x13b.h"
#include "manual3ds.h"
#include "manual_text.h"

// ===========================================================================
// Text grid: crawl believes it draws into an 80x24 terminal.
// ===========================================================================

static const int GRID_W = 80;
static const int GRID_H = 24;

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

// Terminal palette, slightly softened for the 3DS LCDs.
static const uint8_t palette[16][3] =
{
    { 10,  10,  14}, // BLACK
    { 40,  80, 210}, // BLUE
    { 40, 170,  60}, // GREEN
    { 40, 170, 170}, // CYAN
    {190,  40,  40}, // RED
    {170,  60, 180}, // MAGENTA
    {170, 110,  30}, // BROWN
    {185, 185, 190}, // LIGHTGREY
    {100, 100, 110}, // DARKGREY
    {100, 140, 255}, // LIGHTBLUE
    { 90, 230,  90}, // LIGHTGREEN
    { 90, 230, 230}, // LIGHTCYAN
    {255,  95,  90}, // LIGHTRED
    {240, 110, 240}, // LIGHTMAGENTA
    {250, 230,  80}, // YELLOW
    {250, 250, 250}, // WHITE
};

// Region bookkeeping ---------------------------------------------------------

enum Region { RG_VIEW, RG_MSG, RG_HUD, RG_MLIST, RG_OTHER };

// "Text mode": crawl is showing a full-screen interface (menu, inventory,
// level map...). Detected when text lands in the viewport that did not come
// through puttext().
static bool crt_mode = false;
static bool in_puttext = false;
static bool keyboard_overlay = false;
static bool top_dirty = true;
static bool bottom_dirty = true;

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

// ===========================================================================
// Canvas: landscape RGB888 buffers, rotated into the 3DS framebuffers at blit
// ===========================================================================

struct Canvas
{
    int w, h;
    uint8_t *px;
    inline uint8_t *at(int x, int y) { return px + (y * w + x) * 3; }
};

static uint8_t top_px[400 * 240 * 3], bot_px[320 * 240 * 3];
static Canvas top_cv = { 400, 240, top_px };
static Canvas bot_cv = { 320, 240, bot_px };

static void cv_fill(Canvas &c, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > c.w) w = c.w - x0;
    if (y0 + h > c.h) h = c.h - y0;
    for (int y = y0; y < y0 + h; ++y)
    {
        uint8_t *p = c.at(x0, y);
        for (int x = 0; x < w; ++x, p += 3)
            p[0] = r, p[1] = g, p[2] = b;
    }
}

// Blend a colour over a rectangle (a: 0..255).
static void cv_shade(Canvas &c, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b, int a)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > c.w) w = c.w - x0;
    if (y0 + h > c.h) h = c.h - y0;
    const int ia = 255 - a;
    for (int y = y0; y < y0 + h; ++y)
    {
        uint8_t *p = c.at(x0, y);
        for (int x = 0; x < w; ++x, p += 3)
        {
            p[0] = (p[0] * ia + r * a) / 255;
            p[1] = (p[1] * ia + g * a) / 255;
            p[2] = (p[2] * ia + b * a) / 255;
        }
    }
}

static void cv_frame(Canvas &c, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    cv_fill(c, x, y, w, 1, r, g, b);
    cv_fill(c, x, y + h - 1, w, 1, r, g, b);
    cv_fill(c, x, y, 1, h, r, g, b);
    cv_fill(c, x + w - 1, y, 1, h, r, g, b);
}

// Rotate a canvas into a portrait BGR8 framebuffer (fb[(x*240 + 239-y)*3]).
// Brightness: a gamma curve applied as the image goes to the screen, so
// dark tiles and text lift without washing out the whites. 0 = as drawn.
static int g_bright = 2;
static uint8_t g_lut[256];
static const float kBrightGamma[5] = { 1.0f, 0.85f, 0.72f, 0.62f, 0.54f };
static void build_lut()
{
    for (int i = 0; i < 256; ++i)
        g_lut[i] = (uint8_t)(255.0f * powf(i / 255.0f, kBrightGamma[g_bright]) + 0.5f);
}
static void load_brightness()
{
    if (FILE *f = fopen("sdmc:/3ds/crawl/brightness.txt", "r"))
    {
        int v;
        if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= 4)
            g_bright = v;
        fclose(f);
    }
    build_lut();
}
static void cycle_brightness()
{
    g_bright = (g_bright + 1) % 5;
    build_lut();
    if (FILE *f = fopen("sdmc:/3ds/crawl/brightness.txt", "w"))
    {
        fprintf(f, "%d\n", g_bright);
        fclose(f);
    }
}

static void cv_blit(const Canvas &c, gfxScreen_t screen, gfx3dSide_t side)
{
    u16 fbw, fbh;
    u8 *fb = gfxGetFramebuffer(screen, side, &fbw, &fbh);
    const int H = fbw; // 240
    for (int x = 0; x < c.w; ++x)
    {
        u8 *col = fb + x * H * 3;
        for (int y = 0; y < c.h; ++y)
        {
            const uint8_t *s = c.px + (y * c.w + x) * 3;
            u8 *d = col + (H - 1 - y) * 3;
            d[0] = g_lut[s[2]]; d[1] = g_lut[s[1]]; d[2] = g_lut[s[0]];
        }
    }
}

// Fonts ----------------------------------------------------------------------

struct Font
{
    int w, h, n;
    const uint16_t *cp;
    const uint8_t *bits;   // n * h rows, MSB = leftmost of w pixels
};

static const Font F_SMALL = { FONT5X8_W, FONT5X8_H, FONT5X8_N, font5x8_cp, &font5x8_bits[0][0] };
static const Font F_TEXT  = { FONT6X13_W, FONT6X13_H, FONT6X13_N, font6x13_cp, &font6x13_bits[0][0] };
static const Font F_BOLD  = { FONT6X13B_W, FONT6X13B_H, FONT6X13B_N, font6x13b_cp, &font6x13b_bits[0][0] };

static int font_find(const Font &f, char32_t c)
{
    int lo = 0, hi = f.n - 1;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        if (f.cp[mid] == c)
            return mid;
        if (f.cp[mid] < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

// Glyph with fallbacks: the bold font lacks most symbols, crawl uses a few
// characters none of the fonts have.
static const uint8_t *glyph_rows(const Font &f, char32_t c, const Font **used)
{
    int i = font_find(f, c);
    if (i >= 0) { *used = &f; return f.bits + i * f.h; }
    if (&f == &F_BOLD && (i = font_find(F_TEXT, c)) >= 0)
    {
        *used = &F_TEXT;
        return F_TEXT.bits + i * F_TEXT.h;
    }
    switch (c)
    {
    case 0x00B7: case 0x2219: c = '.'; break;   // middle dot floors
    case 0x2248: c = '~'; break;
    default: c = '?'; break;
    }
    i = font_find(f, c);
    *used = &f;
    return f.bits + (i < 0 ? 0 : i) * f.h;
}

static void draw_char(Canvas &cv, const Font &f, int px, int py, char32_t c,
                      const uint8_t *fg, const uint8_t *bg)
{
    const Font *u;
    const uint8_t *rows = glyph_rows(f, c, &u);
    for (int y = 0; y < f.h; ++y)
    {
        const int yy = py + y;
        if (yy < 0 || yy >= cv.h)
            continue;
        const uint8_t row = y < u->h ? rows[y] : 0;
        for (int x = 0; x < f.w; ++x)
        {
            const int xx = px + x;
            if (xx < 0 || xx >= cv.w)
                continue;
            const bool on = row & (1 << (u->w - 1 - x));
            const uint8_t *col = on ? fg : bg;
            if (!col)
                continue;
            uint8_t *p = cv.at(xx, yy);
            p[0] = col[0]; p[1] = col[1]; p[2] = col[2];
        }
    }
}

// UTF-8 string; returns the x after the text. maxw clips (pixels, 0 = none).
static int draw_text(Canvas &cv, const Font &f, int x, int y, const char *s,
                     const uint8_t *fg, const uint8_t *bg = nullptr, int maxw = 0)
{
    const int x0 = x;
    char32_t c;
    while (int taken = utf8towc(&c, s))
    {
        s += taken;
        if (maxw && x - x0 + f.w > maxw)
            break;
        draw_char(cv, f, x, y, c, fg, bg);
        x += f.w;
    }
    return x;
}

static int text_width(const Font &f, const char *s)
{
    int n = 0;
    char32_t c;
    while (int taken = utf8towc(&c, s))
        s += taken, ++n;
    return n * f.w;
}

static const uint8_t C_WHITE[3] = {245, 245, 245};
static const uint8_t C_TEXT[3]  = {215, 215, 222};
static const uint8_t C_DIM[3]   = {140, 140, 152};
static const uint8_t C_GOLD[3]  = {250, 220, 120};

// ===========================================================================
// Tile atlas (romfs:/tiles16.bin, built by tools/gen_atlas.py)
// ===========================================================================

static uint32_t atlas_n = 0, atlas_px_size = 16;
static uint16_t *atlas_wh = nullptr;      // w,h pairs
static uint32_t *atlas_off = nullptr;     // pixel offsets
static uint8_t  *atlas_px = nullptr;      // premultiplied RGBA8
static bool atlas_tried = false;

static bool load_atlas()
{
    if (atlas_tried)
        return atlas_px != nullptr;
    atlas_tried = true;
    FILE *f = fopen("romfs:/tiles16.bin", "rb");
    if (!f)
        return false;
    char magic[4];
    uint32_t hdr[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "T3DS", 4)
        || fread(hdr, 4, 3, f) != 3 || hdr[0] != 1)
    {
        fclose(f);
        return false;
    }
    atlas_n = hdr[1];
    atlas_px_size = hdr[2];
    atlas_wh = new uint16_t[atlas_n * 2];
    atlas_off = new uint32_t[atlas_n];
    uint32_t total = 0;
    for (uint32_t i = 0; i < atlas_n; ++i)
    {
        uint16_t wh[2];
        uint32_t off;
        if (fread(wh, 2, 2, f) != 2 || fread(&off, 4, 1, f) != 1)
            break;
        atlas_wh[i * 2] = wh[0];
        atlas_wh[i * 2 + 1] = wh[1];
        atlas_off[i] = off;
        total = off + wh[0] * wh[1];
    }
    atlas_px = new uint8_t[(size_t)total * 4];
    size_t got = fread(atlas_px, 4, total, f);
    fclose(f);
#ifdef USE_TILE
    // the atlas must match this build's tile numbering exactly
    if (atlas_n != (uint32_t)TILEI_ICONS_MAX)
        got = 0;
#endif
    if (got != total)
    {
        delete[] atlas_px;
        atlas_px = nullptr;
        return false;
    }
    return true;
}

// Draw tile `idx` with its top-left at (x,y). ofs and ymax are in crawl's
// 32 px tile units. alpha 0..255; `submerged` fades the part below the
// waterline. Clipped to the clip rectangle.
static int clip_x0 = 0, clip_x1 = 4096, clip_y0 = 0, clip_y1 = 4096;

static void blit_tile(Canvas &cv, unsigned idx, int x, int y, int ofsx = 0, int ofsy = 0,
                      int alpha = 255, int ymax = -1, bool submerged = false)
{
    if (!atlas_px || idx >= atlas_n)
        return;
    const int S = atlas_px_size;
    const int w = atlas_wh[idx * 2], h = atlas_wh[idx * 2 + 1];
    const uint8_t *src = atlas_px + (size_t)atlas_off[idx] * 4;
    x += ofsx * S / 32;
    y += ofsy * S / 32;
    const int rows = ymax >= 0 ? std::min(h, ymax * S / 32) : h;
    const int water = S * 20 / 32; // waterline for submerged parts
    for (int yy = 0; yy < rows; ++yy)
    {
        const int dy = y + yy;
        if (dy < clip_y0 || dy >= clip_y1 || dy < 0 || dy >= cv.h)
            continue;
        const int a_row = (submerged && yy >= water) ? alpha * 3 / 10 : alpha;
        for (int xx = 0; xx < w; ++xx)
        {
            const int dx = x + xx;
            if (dx < clip_x0 || dx >= clip_x1 || dx < 0 || dx >= cv.w)
                continue;
            const uint8_t *s = src + (yy * w + xx) * 4;
            if (!s[3])
                continue;
            uint8_t *d = cv.at(dx, dy);
            if (a_row == 255)
            {
                const int ia = 255 - s[3];
                d[0] = s[0] + d[0] * ia / 255;
                d[1] = s[1] + d[1] * ia / 255;
                d[2] = s[2] + d[2] * ia / 255;
            }
            else
            {
                const int sa = s[3] * a_row / 255, ia = 255 - sa;
                d[0] = (s[0] * a_row + d[0] * ia) / 255;
                d[1] = (s[1] * a_row + d[1] * ia) / 255;
                d[2] = (s[2] * a_row + d[2] * ia) / 255;
            }
        }
    }
}

// ===========================================================================
// Cell renderer: a port of DungeonCellBuffer (tiledgnbuf.cc) to the atlas.
// Ops are collected per layer, then drawn layer by layer like crawl does.
// ===========================================================================

#ifdef USE_TILE
enum { L_FLOOR, L_WALL, L_FEAT, L_FEAT_TRANS, L_DOLL, L_MAIN_TRANS, L_MAIN, L_ICONS, L_COUNT };

struct TileOp
{
    uint16_t idx;
    int16_t  x, y;
    int8_t   ox, oy;
    int16_t  ymax;
    uint8_t  alpha;
    bool     submerged;
};

static std::vector<TileOp> layer_ops[L_COUNT];

static void op(int layer, tileidx_t idx, int x, int y, int ox = 0, int oy = 0,
               int alpha = 255, int ymax = -1, bool submerged = false)
{
    if (!idx)
        return;
    layer_ops[layer].push_back({ (uint16_t)idx, (int16_t)x, (int16_t)y, (int8_t)ox, (int8_t)oy,
                                 (int16_t)ymax, (uint8_t)alpha, submerged });
}

static void add_dngn_tile(tileidx_t idx, int x, int y, bool in_water = false)
{
    if (idx < TILE_FLOOR_MAX)
        op(L_FLOOR, idx, x, y);
    else if (idx < TILE_WALL_MAX)
        op(L_WALL, idx, x, y);
    else if (in_water)
    {
        op(L_FLOOR, TILE_DNGN_SHALLOW_WATER, x, y);
        op(L_FEAT_TRANS, idx, x, y, 0, 0, 255, -1, true);
    }
    else
        op(L_FEAT, idx, x, y);
}

static void add_blood_overlay(int x, int y, const packed_cell &cell, bool is_wall = false)
{
    if (cell.is_liquefied && !is_wall)
    {
        const int offset = cell.flv.special % tile_dngn_count(TILE_LIQUEFACTION);
        op(L_FLOOR, TILE_LIQUEFACTION + offset, x, y);
    }
    else if (cell.is_bloody && Options.show_blood)
    {
        tileidx_t basetile;
        if (is_wall)
        {
            basetile = cell.old_blood ? TILE_WALL_OLD_BLOOD : TILE_WALL_BLOOD_S;
            basetile += tile_dngn_count(basetile) * cell.blood_rotation;
        }
        else
            basetile = TILE_BLOOD;
        const int offset = cell.flv.special % tile_dngn_count(basetile);
        op(L_FEAT, basetile + offset, x, y);
    }
}

static void pack_background(int x, int y, const packed_cell &cell)
{
    const tile_with_flags_t bg = cell.bg;
    const tileidx_t bg_idx = bg.tile();

    if (bg_idx >= TILE_DNGN_FIRST_TRANSPARENT)
        add_dngn_tile(cell.flv.floor, x, y);
    if (bg_idx > TILE_WALL_MAX)
        add_blood_overlay(x, y, cell);
    add_dngn_tile(bg_idx, x, y, cell.mangrove_water);

    if (bg_idx > TILE_DNGN_UNSEEN)
    {
        if (bg_idx <= TILE_WALL_MAX)
            add_blood_overlay(x, y, cell, bg_idx >= TILE_FLOOR_MAX);

        for (int i = 0; i < cell.num_dngn_overlay; ++i)
        {
            const auto tile = cell.dngn_overlay[i];
            if (tile < TILE_DNGN_MAX)
                add_dngn_tile(tile, x, y);
        }

        if (!bg.has_flag(TILE_FLAG_UNSEEN))
        {
            if (cell.is_sanctuary)  op(L_FEAT, TILE_SANCTUARY, x, y);
            if (cell.is_blasphemy)  op(L_FEAT, TILE_BLASPHEMY, x, y);
            if (cell.is_silenced)   op(L_FEAT, TILE_SILENCED, x, y);
            if (cell.halo == HALO_RANGE)
                op(L_FEAT, TILE_HALO_RANGE, x, y);
            if (cell.halo >= HALO_UMBRA_FIRST && cell.halo <= HALO_UMBRA_LAST)
                op(L_FEAT, TILE_UMBRA + cell.halo - HALO_UMBRA_FIRST, x, y);
            if (cell.orb_glow)
                op(L_FEAT, TILE_ORB_GLOW + cell.orb_glow - 1, x, y);
            if (cell.quad_glow)
                op(L_FEAT, TILE_QUAD_GLOW, x, y);
            if (cell.disjunct)
                op(L_FEAT, TILE_DISJUNCT + cell.disjunct - 1, x, y);
            if (cell.awakened_forest)
                op(L_ICONS, TILEI_BERSERK, x, y);
            if (cell.has_bfb_corpse)
                op(L_FEAT, TILE_BLOOD_FOR_BLOOD, x, y);

            if (cell.fg != 0)
            {
                const tile_flag_t att = cell.fg.flags(TILE_FLAG_ATT_MASK);
                if (att == TILE_FLAG_PET)
                    op(L_FEAT, TILE_HALO_FRIENDLY, x, y);
                else if (att == TILE_FLAG_GD_NEUTRAL)
                    op(L_FEAT, TILE_HALO_GD_NEUTRAL, x, y);
                else if (att == TILE_FLAG_NEUTRAL)
                    op(L_FEAT, TILE_HALO_NEUTRAL, x, y);

                const tile_flag_t threat = cell.fg.flags(TILE_FLAG_THREAT_MASK);
                const bool ghost = cell.fg.has_flag(TILE_FLAG_GHOST);
                if (threat == TILE_FLAG_TRIVIAL)
                    op(L_FEAT, ghost ? TILE_THREAT_GHOST_TRIVIAL : TILE_THREAT_TRIVIAL, x, y);
                else if (threat == TILE_FLAG_EASY)
                    op(L_FEAT, ghost ? TILE_THREAT_GHOST_EASY : TILE_THREAT_EASY, x, y);
                else if (threat == TILE_FLAG_TOUGH)
                    op(L_FEAT, ghost ? TILE_THREAT_GHOST_TOUGH : TILE_THREAT_TOUGH, x, y);
                else if (threat == TILE_FLAG_NASTY)
                    op(L_FEAT, ghost ? TILE_THREAT_GHOST_NASTY : TILE_THREAT_NASTY, x, y);
                else if (threat == TILE_FLAG_UNUSUAL)
                    op(L_FEAT, TILE_THREAT_UNUSUAL, x, y);

                if (cell.is_highlighted_summoner)
                    op(L_FEAT, TILE_HALO_SUMMONER, x, y);
            }

            if (bg.has_flag(TILE_FLAG_EXCL_CTR))
                op(L_FEAT, TILE_TRAVEL_EXCLUSION_CENTRE_BG, x, y);
            else if (bg.has_flag(TILE_FLAG_TRAV_EXCL))
                op(L_FEAT, TILE_TRAVEL_EXCLUSION_BG, x, y);
        }

        if (bg.has_flag(TILE_FLAG_REMEMBERED_INVIS))
            op(L_FEAT, TILE_REMEMBERED_INVIS, x, y);
    }
}

static void pack_foreground(int x, int y, const packed_cell &cell)
{
    const tile_with_flags_t fg = cell.fg;
    const tile_with_flags_t bg = cell.bg;
    const tileidx_t fg_idx = fg.tile();
    const bool in_water = is_in_water(cell);

    if (fg_idx && get_tile_texture(fg_idx) == TEX_DEFAULT)
    {
        const tileidx_t base_idx = tileidx_known_base_item(fg_idx);
        if (in_water || cell.cloud)
        {
            op(L_MAIN_TRANS, base_idx, x, y, 0, 0, 255, -1, true);
            op(L_MAIN_TRANS, fg_idx, x, y, 0, 0, 255, -1, true);
        }
        else
        {
            op(L_MAIN, base_idx, x, y);
            op(L_MAIN, fg_idx, x, y);
        }
    }

    if (fg_idx >= TILE_PARCHMENT_LOW && fg_idx <= TILE_PARCHMENT_HIGH)
    {
        if (const item_def *item = cell.map_knowledge.item())
        {
            const spell_type spell = static_cast<spell_type>(item->plus);
            op(L_MAIN, tileidx_parchment_overlay(spell, 0), x, y);
            op(L_MAIN, tileidx_parchment_overlay(spell, 1), x, y);
        }
    }

    if (fg.has_flag(TILE_FLAG_NET)) op(L_ICONS, TILEI_TRAP_NET, x, y);
    if (fg.has_flag(TILE_FLAG_WEB)) op(L_ICONS, TILEI_TRAP_WEB, x, y);

    if (fg.has_flag(TILE_FLAG_S_UNDER_ARTEFACT))
        op(L_ICONS, TILEI_ITEM_STACK_3, x, y);
    else if (fg.has_flag(TILE_FLAG_S_UNDER_GOOD))
        op(L_ICONS, TILEI_ITEM_STACK_2, x, y);
    else if (fg.has_flag(TILE_FLAG_S_UNDER))
        op(L_ICONS, TILEI_ITEM_STACK_1, x, y);

    if (fg.has_flag(TILE_FLAG_ATT_MASK))
    {
        const tile_flag_t att = fg.flags(TILE_FLAG_ATT_MASK);
        if (att == TILE_FLAG_PET)
            op(L_ICONS, TILEI_FRIENDLY, x, y);
        else if (att == TILE_FLAG_GD_NEUTRAL)
            op(L_ICONS, TILEI_GOOD_NEUTRAL, x, y);
        else if (att == TILE_FLAG_NEUTRAL)
            op(L_ICONS, TILEI_NEUTRAL, x, y);
    }

    int shift = 0;
    if (fg.has_flag(TILE_FLAG_BEH_MASK))
    {
        const tile_flag_t beh = fg.flags(TILE_FLAG_BEH_MASK);
        if (beh == TILE_FLAG_PARALYSED)     { op(L_ICONS, TILEI_PARALYSED, x, y);  shift += 12; }
        else if (beh == TILE_FLAG_STAB)     { op(L_ICONS, TILEI_STAB_BRAND, x, y); shift += 12; }
        else if (beh == TILE_FLAG_MAY_STAB) { op(L_ICONS, TILEI_UNAWARE, x, y);    shift += 7; }
        else if (beh == TILE_FLAG_FLEEING)  { op(L_ICONS, TILEI_FLEEING, x, y);    shift += 3; }
    }

    if (fg.has_flag(TILE_FLAG_POISON_MASK))
    {
        const tile_flag_t p = fg.flags(TILE_FLAG_POISON_MASK);
        if (p == TILE_FLAG_POISON)           { op(L_ICONS, TILEI_POISON, x, y, -shift);      shift += 6; }
        else if (p == TILE_FLAG_MORE_POISON) { op(L_ICONS, TILEI_MORE_POISON, x, y, -shift); shift += 6; }
        else if (p == TILE_FLAG_MAX_POISON)  { op(L_ICONS, TILEI_MAX_POISON, x, y, -shift);  shift += 7; }
    }

    if (fg.has_flag(TILE_FLAG_REMEMBERED_INVIS))
        op(L_ICONS, TILEI_UNSEEN_INVIS_REMEMBERED, x, y);

    for (auto icon : cell.icons)
    {
        int size = status_icon_size(icon);
        if (size == 0)
        {
            op(L_ICONS, icon, x, y);
            continue;
        }
        op(L_ICONS, icon, x, y, -shift);
        shift += size < 0 ? 7 : size;
    }

    if (bg.has_flag(TILE_FLAG_UNSEEN) && (bg != TILE_FLAG_UNSEEN || fg != 0))
        op(L_ICONS, TILEI_MESH, x, y);
    if (bg.has_flag(TILE_FLAG_OOR) && (bg != TILE_FLAG_OOR || fg != 0))
        op(L_ICONS, TILEI_OOR_MESH, x, y);
    if (bg.has_flag(TILE_FLAG_MM_UNSEEN) && (bg != TILE_FLAG_MM_UNSEEN || fg != 0))
        op(L_ICONS, TILEI_MAGIC_MAP_MESH, x, y);

    if (bg.has_flag(TILE_FLAG_RAMPAGE))
        op(L_ICONS, you.duration[DUR_TAILWIND] ? TILEI_RAMPAGE_INSTANT : TILEI_RAMPAGE, x, y);
    if (bg.has_flag(TILE_FLAG_NEW_STAIR) && shift == 0)
        op(L_ICONS, TILEI_NEW_STAIR, x, y);
    if (bg.has_flag(TILE_FLAG_NEW_TRANSPORTER) && shift == 0)
        op(L_ICONS, TILEI_NEW_TRANSPORTER, x, y);

    if (bg.has_flag(TILE_FLAG_EXCL_CTR) && bg.has_flag(TILE_FLAG_UNSEEN))
        op(L_ICONS, TILEI_TRAVEL_EXCLUSION_CENTRE_FG, x, y);
    else if (bg.has_flag(TILE_FLAG_TRAV_EXCL) && bg.has_flag(TILE_FLAG_UNSEEN))
        op(L_ICONS, TILEI_TRAVEL_EXCLUSION_FG, x, y);

    if (bg.has_flag(TILE_FLAG_TUT_CURSOR))
        op(L_ICONS, TILEI_TUTORIAL_CURSOR, x, y);
    else if (bg.has_flag(TILE_FLAG_CURSOR))
    {
        const tile_flag_t cf = bg.flags(TILE_FLAG_CURSOR);
        op(L_ICONS, cf == TILE_FLAG_CURSOR3 ? TILEI_CURSOR3
                    : cf == TILE_FLAG_CURSOR1 ? TILEI_CURSOR : TILEI_CURSOR2, x, y);
    }

    if (cell.travel_trail & 0xF)
        op(L_ICONS, TILEI_TRAVEL_PATH_FROM + (cell.travel_trail & 0xF) - 1, x, y);
    if (cell.travel_trail & 0xF0)
        op(L_ICONS, TILEI_TRAVEL_PATH_TO + ((cell.travel_trail & 0xF0) >> 4) - 1, x, y);

    if (fg.has_flag(TILE_FLAG_MDAM_MASK))
    {
        const tile_flag_t m = fg.flags(TILE_FLAG_MDAM_MASK);
        if (m == TILE_FLAG_MDAM_LIGHT)      op(L_ICONS, TILEI_MDAM_LIGHTLY_DAMAGED, x, y);
        else if (m == TILE_FLAG_MDAM_MOD)   op(L_ICONS, TILEI_MDAM_MODERATELY_DAMAGED, x, y);
        else if (m == TILE_FLAG_MDAM_HEAVY) op(L_ICONS, TILEI_MDAM_HEAVILY_DAMAGED, x, y);
        else if (m == TILE_FLAG_MDAM_SEV)   op(L_ICONS, TILEI_MDAM_SEVERELY_DAMAGED, x, y);
        else if (m == TILE_FLAG_MDAM_ADEAD) op(L_ICONS, TILEI_MDAM_ALMOST_DEAD, x, y);
    }

    if (fg.has_flag(TILE_FLAG_DEMON))
    {
        const tile_flag_t d = fg.flags(TILE_FLAG_DEMON);
        if (d == TILE_FLAG_DEMON_1)      op(L_ICONS, TILEI_DEMON_NUM1, x, y);
        else if (d == TILE_FLAG_DEMON_2) op(L_ICONS, TILEI_DEMON_NUM2, x, y);
        else if (d == TILE_FLAG_DEMON_3) op(L_ICONS, TILEI_DEMON_NUM3, x, y);
        else if (d == TILE_FLAG_DEMON_4) op(L_ICONS, TILEI_DEMON_NUM4, x, y);
        else if (d == TILE_FLAG_DEMON_5) op(L_ICONS, TILEI_DEMON_NUM5, x, y);
    }
}

static void pack_doll(const dolls_data &doll, int x, int y, bool submerged, bool ghost)
{
    vector<tile_def> parts;
    pack_tilep_set(parts, doll);
    for (const tile_def &t : parts)
        op(L_DOLL, t.tile, x, y, 0, 0, ghost ? 150 : 255, t.ymax, submerged);
}

static void pack_cell(int x, int y, const packed_cell &cell)
{
    pack_background(x, y, cell);

    const tileidx_t fg_idx = cell.fg.tile();
    const bool in_water = is_in_water(cell);
    tileidx_t cloud_idx = cell.cloud;
    if (cloud_idx == TILE_CLOUD_INK && player_in_branch(BRANCH_SHOALS))
        cloud_idx = 0;

    if (fg_idx >= TILEP_MCACHE_START)
    {
        if (mcache_entry *entry = mcache.get(fg_idx))
        {
            const bool trans = entry->transparent();
            if (const dolls_data *doll = entry->doll())
                pack_doll(*doll, x, y, in_water, trans);
            tile_draw_info dinfo[mcache_entry::MAX_INFO_COUNT];
            const int n = entry->info(&dinfo[0]);
            for (int i = 0; i < n; ++i)
                op(L_DOLL, dinfo[i].idx, x, y, dinfo[i].ofs_x, dinfo[i].ofs_y,
                   trans ? 150 : 255, -1, in_water);
        }
        else
            op(L_DOLL, TILEP_MONS_UNKNOWN, x, y);
    }
    else if (fg_idx == TILEP_PLAYER)
    {
        dolls_data result = player_doll;
        fill_doll_equipment(result);
        pack_doll(result, x, y, in_water, false);
    }
    else if (fg_idx && get_tile_texture(fg_idx) == TEX_PLAYER)
        op(L_DOLL, fg_idx, x, y, 0, 0, 255, -1, in_water);

    pack_foreground(x, y, cell);

    if (cloud_idx)
    {
        if (fg_idx)
            op(L_MAIN_TRANS, cloud_idx, x, y, 0, 0, 140);
        else
            op(L_MAIN, cloud_idx, x, y);
    }

    for (int i = 0; i < cell.num_dngn_overlay; ++i)
    {
        const auto tile = cell.dngn_overlay[i];
        if (TILE_DNGN_MAX <= tile && tile < TILE_MAIN_MAX)
        {
            op(L_MAIN, tileidx_known_base_item(tile), x, y);
            op(L_MAIN, tile, x, y);
        }
    }
}

static void flush_ops(Canvas &cv)
{
    for (int l = 0; l < L_COUNT; ++l)
    {
        for (const TileOp &o : layer_ops[l])
            blit_tile(cv, o.idx, o.x, o.y, o.ox, o.oy, o.alpha, o.ymax, o.submerged);
        layer_ops[l].clear();
    }
}
#endif // USE_TILE

// ===========================================================================
// View cache: the last dungeon view crawl handed to puttext()
// ===========================================================================

#ifdef USE_TILE
// Heap-allocated on purpose: packed_cells own map_cell items/monster_info,
// and destroying them as a static after crawl's own globals are gone (at
// process exit) crashed on HOME. They are cleared in console_shutdown().
static std::vector<packed_cell> &vcells = *new std::vector<packed_cell>;
#endif
static coord_def vsize;           // size of the cached view
static coord_def vorigin;         // grid coordinate of cached cell (0,0)
static coord_def vcentre;         // crawl_view.vgrdc at the time
static bool have_view = false;

// Bottom-screen map geometry
static const int TS = 16;               // tile pixels
static const int MAP_COLS = 20, MAP_ROWS = 15;
static const int MAP_CC = 10, MAP_CR = 7; // centre cell (the player)
static const int BTN_W = 32;            // edge button columns

// Map zoom (L = out, R = in): tiles are drawn at 16 px and scaled up.
static const int kZoomTS[3] = { 16, 24, 32 };
static int g_zoom = 0;
static int zoom_ts()   { return kZoomTS[g_zoom]; }
static int zoom_cols() { return (320 + zoom_ts() - 1) / zoom_ts() + 1; }
static int zoom_rows() { return (240 + zoom_ts() - 1) / zoom_ts() + 1; }
// screen position of the top-left of cell (0,0): the player's square stays
// where it is at 1x (centre 168,120)
static int zoom_ox()   { return 168 - ((zoom_cols() / 2) * zoom_ts() + zoom_ts() / 2); }
static int zoom_oy()   { return 120 - ((zoom_rows() / 2) * zoom_ts() + zoom_ts() / 2); }
static coord_def zoom_cell_grid(int c, int r)
{
    return vcentre + coord_def(c - zoom_cols() / 2, r - zoom_rows() / 2);
}

static coord_def map_cell_grid(int c, int r)
{
    return vcentre + coord_def(c - MAP_CC, r - MAP_CR);
}

// ===========================================================================
// Bottom screen: buttons
// ===========================================================================

struct button
{
    int x, y, w, h;
    const char *label;
    int key;                 // key to send (0 = special)
    int special;             // see enum below
    unsigned tile;           // icon (gui/main tile), 0 = text only
};

enum { BS_NONE = 0, BS_KEYBOARD, BS_HIDE, BS_YES, BS_NO };

static std::vector<button> buttons;     // rebuilt per frame
static int pressed_button = -1;
static bool buttons_hidden = false;

// The two edge columns in play mode.
static void build_play_buttons()
{
    buttons.clear();
    struct def { const char *label; int key; unsigned tile; int special; };
#ifdef USE_TILE
    static const def left[] = {
        { "explr",   'o',    TILEG_CMD_EXPLORE, 0 },
        { "fight",   CK_TAB, TILEG_CMD_AUTOFIGHT, 0 },
        { "rest",    '5',    TILEG_CMD_REST, 0 },
        { "wait",    's',    TILEG_CMD_WAIT, 0 },
        { "pick up", 'g',    0, 0 },
        { "up",      '<',    TILEG_CMD_MAP_FIND_UPSTAIR, 0 },
        { "down",    '>',    TILEG_CMD_MAP_FIND_DOWNSTAIR, 0 },
    };
    static const def right[] = {
        { "items",   'i',    TILEG_CMD_DISPLAY_INVENTORY, 0 },
        { "cast",    'z',    TILEG_CMD_CAST_SPELL, 0 },
        { "abil",    'a',    TILEG_CMD_USE_ABILITY, 0 },
        { "map",     'X',    TILEG_CMD_DISPLAY_MAP, 0 },
        { "char",    '%',    TILEG_CMD_DISPLAY_CHARACTER_STATUS, 0 },
        { "msgs",    CONTROL('P'), TILEG_CMD_REPLAY_MESSAGES, 0 },
        { "keys",    0,      TILEG_CMD_KEYBOARD, BS_KEYBOARD },
    };
#else
    static const def left[] = {
        { "explr", 'o', 0, 0 }, { "fight", CK_TAB, 0, 0 }, { "rest", '5', 0, 0 },
        { "wait", 's', 0, 0 }, { "pick", 'g', 0, 0 }, { "up <", '<', 0, 0 }, { "dn >", '>', 0, 0 },
    };
    static const def right[] = {
        { "items", 'i', 0, 0 }, { "cast", 'z', 0, 0 }, { "abil", 'a', 0, 0 },
        { "map", 'X', 0, 0 }, { "char", '%', 0, 0 }, { "msgs", CONTROL('P'), 0, 0 },
        { "keys", 0, 0, BS_KEYBOARD },
    };
#endif
    const int n = 7, bh = 240 / n;
    for (int i = 0; i < n; ++i)
    {
        buttons.push_back({ 0, i * bh, BTN_W, bh, left[i].label, left[i].key, left[i].special, left[i].tile });
        buttons.push_back({ 320 - BTN_W, i * bh, BTN_W, bh, right[i].label, right[i].key, right[i].special, right[i].tile });
    }
}

static void draw_button(Canvas &cv, const button &b, bool pressed, bool translucent)
{
    if (translucent)
        cv_shade(cv, b.x, b.y, b.w, b.h, 12, 12, 20, pressed ? 120 : 170);
    else
        cv_fill(cv, b.x, b.y, b.w, b.h, pressed ? 90 : 40, pressed ? 90 : 42, pressed ? 130 : 56);
    if (pressed)
    {
        cv_frame(cv, b.x, b.y, b.w, b.h, 250, 230, 120);
        // pushed in: dark inner edge at top/left, contents move down 1 px
        cv_fill(cv, b.x + 1, b.y + 1, b.w - 2, 2, 20, 20, 30);
        cv_fill(cv, b.x + 1, b.y + 1, 2, b.h - 2, 20, 20, 30);
    }
    else
        cv_frame(cv, b.x, b.y, b.w, b.h, 60, 62, 80);
    const int push = pressed ? 1 : 0;
    if (b.tile)
    {
        blit_tile(cv, b.tile, b.x + (b.w - 16) / 2 + push, b.y + 3 + push);
        const int lw = text_width(F_SMALL, b.label);
        draw_text(cv, F_SMALL, b.x + (b.w - lw) / 2 + push, b.y + b.h - 10 + push, b.label, C_TEXT);
    }
    else
    {
        // text button: label (possibly two words) centred
        const Font &f = b.w >= 40 ? F_TEXT : F_SMALL;
        const char *sp = strchr(b.label, ' ');
        if (sp && f.w * (int)strlen(b.label) > b.w - 2)
        {
            std::string a(b.label, sp - b.label), c(sp + 1);
            draw_text(cv, f, b.x + (b.w - text_width(f, a.c_str())) / 2 + push, b.y + b.h / 2 - f.h + push, a.c_str(), C_WHITE);
            draw_text(cv, f, b.x + (b.w - text_width(f, c.c_str())) / 2 + push, b.y + b.h / 2 + push, c.c_str(), C_WHITE);
        }
        else
            draw_text(cv, f, b.x + (b.w - text_width(f, b.label)) / 2 + push, b.y + (b.h - f.h) / 2 + push, b.label, C_WHITE);
    }
}

// ===========================================================================
// Prompt / menu detection from the text grid
// ===========================================================================

static std::string grid_row_text(int y, int x0 = 0, int x1 = GRID_W)
{
    std::string s;
    if (y < 0 || y >= GRID_H)
        return s;
    for (int x = std::max(0, x0); x < std::min(GRID_W, x1); ++x)
    {
        char buf[8];
        const int n = wctoutf8(buf, grid[y][x].glyph ? grid[y][x].glyph : ' ');
        s.append(buf, n);
    }
    while (!s.empty() && s.back() == ' ')
        s.pop_back();
    return s;
}

// Is crawl waiting on a yes/no question in the message pane?
static bool yesno_prompt_visible()
{
    if (mouse_control::current_mode() == MOUSE_MODE_YESNO)
        return true;
    const crawl_view_geometry &v = crawl_view;
    int last = -1;
    for (int r = 0; r < v.msgsz.y; ++r)
        if (!grid_row_text(v.msgp.y - 1 + r).empty())
            last = r;
    if (last < 0)
        return false;
    const std::string s = grid_row_text(v.msgp.y - 1 + last);
    return s.find("(y/n)") != std::string::npos || s.find("[Y/n]") != std::string::npos
           || s.find("[y/N]") != std::string::npos || s.find("(Y/n)") != std::string::npos
           || s.find("(y/N)") != std::string::npos || s.find("[y/n]") != std::string::npos;
}

static bool more_prompt_visible()
{
    if (mouse_control::current_mode() == MOUSE_MODE_MORE)
        return true;
    const crawl_view_geometry &v = crawl_view;
    for (int r = 0; r < v.msgsz.y; ++r)
        if (grid_row_text(v.msgp.y - 1 + r).find("--more--") != std::string::npos)
            return true;
    return false;
}

// Menu entries in text mode: "x - text", "x + text", "x # text", "x) text",
// "x. text" or "x: text", possibly several per row (multi-column menus).
struct menu_entry { int key; int row; int col; int tcol; int tend; };

static bool menu_key_char(char32_t k)
{
    return k < 128 && k > ' ' && (isalnum(k) || strchr("$*_!?%^&#@<>{}[]|~=,/'\\\"+-;:", (int)k));
}

static std::vector<menu_entry> find_menu_entries()
{
    std::vector<menu_entry> out;
    for (int y = 0; y < GRID_H; ++y)
    {
        std::vector<menu_entry> row;
        for (int x = 0; x < GRID_W - 3; ++x)
        {
            // an entry starts after a gap: line start or two spaces
            if (x > 0 && grid[y][x - 1].glyph != ' ')
                continue;
            if (x > 1 && grid[y][x - 2].glyph != ' ' && x != 1)
                continue;
            const char32_t k = grid[y][x].glyph;
            if (!menu_key_char(k))
                continue;
            const char32_t a = grid[y][x + 1].glyph, b = grid[y][x + 2].glyph, c = grid[y][x + 3].glyph;
            int t = -1;
            if (a == ' ' && (b == '-' || b == '+' || b == '#' || b == '=') && c == ' ')
                t = x + 4;
            else if ((a == ')' || a == '.' || a == ':') && b == ' ')
                t = x + 2;
            if (t < 0)
                continue;
            while (t < GRID_W && grid[y][t].glyph == ' ')
                ++t;
            if (t >= GRID_W || grid[y][t].glyph == ' ')
                continue;
            row.push_back({ (int)k, y, x, t, GRID_W });
            x = t;
        }
        for (size_t i = 0; i < row.size(); ++i)
        {
            if (i + 1 < row.size())
                row[i].tend = row[i + 1].col;
            out.push_back(row[i]);
        }
    }
    // a lone match is usually prose, not a menu
    if (out.size() < 2)
        out.clear();
    // column-major, like the menu reads on the top screen; a footer block
    // (after a gap of blank rows: "+ - Recommended...", "? - Help") goes last
    int footer = GRID_H;
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i].row - out[i - 1].row > 2)
        {
            footer = out[i].row;
            break;
        }
    std::stable_sort(out.begin(), out.end(), [footer](const menu_entry &a, const menu_entry &b) {
        const bool fa = a.row >= footer, fb = b.row >= footer;
        if (fa != fb)
            return fb;
        if (fa)
            return a.row != b.row ? a.row < b.row : a.col < b.col;
        return a.col / 8 != b.col / 8 ? a.col < b.col : a.row < b.row;
    });
    return out;
}

// ===========================================================================
// Drawing: top screen
// ===========================================================================

static const uint8_t *pal(int c) { return palette[c & 15]; }

// Text mode: the whole 80x24 grid in the 5x8 font (5x10 cells).
static void draw_text_mode_top()
{
    Canvas &cv = top_cv;
    cv_fill(cv, 0, 0, 400, 240, 8, 8, 12);
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
        {
            const cell_t &c = grid[y][x];
            const uint8_t *fg = pal(c.fg), *bg = pal(c.bg);
            if (cursor_is_enabled && x == cx && y == cy)
                std::swap(fg, bg);
            if ((c.bg & 15) != BLACK || (cursor_is_enabled && x == cx && y == cy))
                cv_fill(cv, x * 5, y * 10, 5, 10, bg[0], bg[1], bg[2]);
            if (c.glyph != ' ')
                draw_char(cv, F_SMALL, x * 5, y * 10 + 1, c.glyph, fg, nullptr);
        }
}

// Copy a grid region with a given font at (px,py).
static void draw_grid_region(Canvas &cv, const Font &f, int px, int py, int rowh,
                             coord_def p, coord_def sz, int maxcols, int maxrows)
{
    for (int r = 0; r < sz.y && r < maxrows; ++r)
    {
        const int gy = p.y - 1 + r;
        if (gy < 0 || gy >= GRID_H)
            continue;
        for (int c = 0; c < sz.x && c < maxcols; ++c)
        {
            const int gx = p.x - 1 + c;
            if (gx < 0 || gx >= GRID_W)
                continue;
            const cell_t &cell = grid[gy][gx];
            const uint8_t *fg = pal(cell.fg);
            const uint8_t *bg = (cell.bg & 15) == BLACK ? nullptr : pal(cell.bg);
            if (cell.glyph != ' ' || bg)
                draw_char(cv, f, px + c * f.w, py + r * rowh, cell.glyph, fg, bg);
        }
    }
}

static void draw_play_top()
{
    Canvas &cv = top_cv;
    cv_fill(cv, 0, 0, 400, 240, 10, 10, 16);
    const crawl_view_geometry &v = crawl_view;

    // --- status panel (crawl's HUD text) at 6x13, left
    const int hud_cols = std::min((int)v.hudsz.x, 42);
    int hud_rows = 0;                      // only the rows crawl actually uses
    for (int r = 0; r < std::min((int)v.hudsz.y, 12); ++r)
    {
        const int gy = v.hudp.y - 1 + r;
        if (gy < 0 || gy >= GRID_H)
            break;
        for (int c = 0; c < hud_cols; ++c)
        {
            const int gx = v.hudp.x - 1 + c;
            if (gx >= 0 && gx < GRID_W && grid[gy][gx].glyph != ' ')
            {
                hud_rows = r + 1;
                break;
            }
        }
    }
    hud_rows = std::max(hud_rows, 8);
    const int msg_top = hud_rows * 13 + 6;  // messages fill the rest of the screen
    cv_fill(cv, 0, 0, 256, hud_rows * 13 + 4, 16, 16, 26);
    draw_grid_region(cv, F_TEXT, 3, 2, 13, v.hudp, v.hudsz, hud_cols, hud_rows);
    const int map_oy = msg_top - GYM - 6;   // minimap sits just above the messages

    // --- monsters in view, right column, with their tiles
    const int mx0 = 262, my0 = 2;
    draw_text(cv, F_BOLD, mx0, my0, "In view", C_DIM);
    int row = 0;
#ifdef USE_TILE
    if (map_bounds(you.pos()))
    {
        vector<monster_info> mons;
        get_nearby_monster_info(mons);
        for (size_t i = 0; i < mons.size() && my0 + 16 + row * 18 + 16 < map_oy - 2; )
        {
            size_t j = i + 1;
            while (j < mons.size() && !monster_info::less_than(mons[i], mons[j])
                   && !monster_info::less_than(mons[j], mons[i]))
                ++j;
            const int count = (int)(j - i);
            const int y = my0 + 16 + row * 18;
            const tile_with_flags_t t = tileidx_monster(mons[i]);
            tileidx_t ti = t.tile();
            if (ti >= TILEP_MCACHE_START)
                ti = TILEP_MONS_UNKNOWN;
            cv_fill(cv, mx0, y, 16, 16, 30, 30, 40);
            blit_tile(cv, ti, mx0, y);
            string desc;
            int col = LIGHTGREY;
            mons[i].to_string(count, desc, col, false, nullptr, false);
            draw_text(cv, F_TEXT, mx0 + 19, y + 2, desc.c_str(), pal(col), nullptr, 400 - mx0 - 20);
            ++row;
            i = j;
        }
    }
#endif
    if (!row)
        draw_text(cv, F_TEXT, mx0, my0 + 18, "nothing", C_DIM);

    // --- level minimap (1 px per square), bottom of the right column
    {
        const int ox = 400 - GXM - 4, oy = map_oy;
        cv_fill(cv, ox - 2, oy - 2, GXM + 4, GYM + 4, 6, 6, 10);
        for (int y = 0; y < GYM; ++y)
            for (int x = 0; x < GXM; ++x)
            {
                const coord_def gc(x, y);
                const map_cell &mc = env.map_knowledge(gc);
                if (!mc.known())
                    continue;
                const dungeon_feature_type f = mc.feat();
                uint8_t r = 70, g = 70, b = 78;                       // floor
                if (feat_is_wall(f) || feat_is_solid(f)) r = 130, g = 120, b = 105;
                else if (feat_is_water(f))               r = 40, g = 80, b = 190;
                else if (feat_is_door(f))                r = 170, g = 110, b = 40;
                if (feat_is_stair(f))                    r = 250, g = 220, b = 60;
                if (mc.monsterinfo() && you.see_cell(gc)) r = 240, g = 60, b = 60;
                if (gc == you.pos())                     r = g = b = 255;
                uint8_t *p = cv.at(ox + x, oy + y);
                p[0] = r; p[1] = g; p[2] = b;
            }
        // the bottom map's window
        if (g_zoom == 0)
            cv_frame(cv, ox + vcentre.x - MAP_CC, oy + vcentre.y - MAP_CR, MAP_COLS, MAP_ROWS, 120, 120, 160);
        else
        {
            // the part of the zoomed map actually on screen
            const int zts = zoom_ts();
            const int c0 = std::max(0, (0 - zoom_ox()) / zts), r0 = std::max(0, (0 - zoom_oy()) / zts);
            const int vis_c = 320 / zts, vis_r = 240 / zts;
            const coord_def g0 = zoom_cell_grid(c0, r0);
            cv_frame(cv, ox + g0.x, oy + g0.y, vis_c, vis_r, 120, 120, 160);
        }
    }

    // --- messages, in the 6x13 font (66 columns), newest at the bottom.
    // Normally they come whole from crawl's message log and are word-wrapped
    // once here (re-wrapping crawl's 80-column lines left ragged fragments).
    // While the player is typing into the message line, the pane itself is
    // shown so the typed text and cursor appear. Dark colours are lifted so
    // every message reads clearly on the dark panel.
    cv_fill(cv, 0, msg_top - 2, 400, 1, 60, 60, 80);
    cv_fill(cv, 0, msg_top - 1, 400, 240 - msg_top + 1, 6, 6, 10);
    {
        const int COLS = 400 / F_TEXT.w - 1, LH = 13, INDENT = 2;
        const int max_lines = (240 - msg_top - 2) / LH;
        struct Ch { char32_t c; int col; bool cursor; };
        std::vector<std::vector<Ch>> lines;

        auto wrap = [&](const std::vector<Ch> &text) {
            size_t s = 0;
            bool first = true;
            while (s < text.size())
            {
                const int width = first ? COLS : COLS - INDENT;
                size_t n = std::min(text.size() - s, (size_t)width);
                if (s + n < text.size())          // break at the last space
                    for (size_t k = n; k > (size_t)width / 2; --k)
                        if (text[s + k].c == ' ')
                        {
                            n = k;
                            break;
                        }
                std::vector<Ch> line;
                if (!first)
                    line.assign(INDENT, Ch{ ' ', LIGHTGREY, false });
                line.insert(line.end(), text.begin() + s, text.begin() + s + n);
                lines.push_back(line);
                s += n;
                while (s < text.size() && text[s].c == ' ' && !text[s].cursor)
                    ++s;
                first = false;
            }
        };

        const bool typing = cursor_is_enabled && cy >= v.msgp.y - 1
                            && cy < v.msgp.y - 1 + v.msgsz.y;
        if (!typing)
        {
            int last_turn = INT_MIN;
            for (const auto &m : lib3ds_recent_messages(max_lines + 4))
            {
                std::vector<Ch> text;
                int col = LIGHTGREY;
                for (const auto &op : m.first.ops)
                {
                    if (op.type == FSOP_COLOUR)
                        col = op.colour;
                    else if (op.type == FSOP_TEXT)
                        for (const char *p = op.text.c_str(); *p; )
                        {
                            char32_t c;
                            const int len = utf8towc(&c, p);
                            if (len <= 0)
                                break;
                            p += len;
                            text.push_back({ c, col, false });
                        }
                }
                // a dim mark starts each new turn, like crawl's '_'
                const bool new_turn = m.second != last_turn;
                last_turn = m.second;
                const size_t before = lines.size();
                wrap(text);
                if (new_turn && lines.size() > before)
                    lines[before].insert(lines[before].begin(), Ch{ 0x2022, DARKGREY, false });
                else if (lines.size() > before)
                    lines[before].insert(lines[before].begin(), Ch{ ' ', LIGHTGREY, false });
            }
            if (more_prompt_visible())
            {
                std::vector<Ch> more;
                for (const char *p = "--more--"; *p; ++p)
                    more.push_back({ (char32_t)*p, YELLOW, false });
                lines.push_back(more);
            }
        }
        else
        {
            for (int r = 0; r < v.msgsz.y; ++r)
            {
                const int gy = v.msgp.y - 1 + r;
                if (gy < 0 || gy >= GRID_H)
                    continue;
                const int x0 = std::max(0, v.msgp.x - 1);
                int len = std::min((int)v.msgsz.x, GRID_W - x0);
                while (len > 0 && grid[gy][x0 + len - 1].glyph == ' ')
                    --len;
                if (cy == gy && cx - x0 + 1 > len)
                    len = cx - x0 + 1;
                if (len <= 0)
                    continue;
                std::vector<Ch> text;
                for (int c = 0; c < len; ++c)
                    text.push_back({ grid[gy][x0 + c].glyph, grid[gy][x0 + c].fg,
                                     cy == gy && cx == x0 + c });
                wrap(text);
            }
        }

        const int first = std::max(0, (int)lines.size() - max_lines);
        for (int i = first; i < (int)lines.size(); ++i)
        {
            const int y = msg_top + 1 + (i - first) * LH;
            for (size_t c = 0; c < lines[i].size() && (int)c <= COLS; ++c)
            {
                const Ch &ch = lines[i][c];
                const int x = 4 + (int)c * F_TEXT.w;
                if (ch.cursor)
                    cv_fill(cv, x, y + 11, F_TEXT.w, 2, 230, 230, 230);
                if (ch.c == ' ')
                    continue;
                const uint8_t *src = pal(ch.col);
                uint8_t fg[3] = { src[0], src[1], src[2] };
                const int lum = (fg[0] * 3 + fg[1] * 6 + fg[2]) / 10;
                if (lum < 150)                 // lift blues, purples, dark grey
                    for (int k = 0; k < 3; ++k)
                        fg[k] = (uint8_t)(fg[k] + (255 - fg[k]) * (150 - lum) / 220);
                draw_char(cv, F_TEXT, x, y, ch.c, fg, nullptr);
            }
        }
    }
}

// ===========================================================================
// Drawing: bottom screen
// ===========================================================================

static void draw_play_bottom()
{
    Canvas &cv = bot_cv;
    cv_fill(cv, 0, 0, 320, 240, 0, 0, 0);

#ifdef USE_TILE
    if (have_view && atlas_px)
    {
        if (g_zoom == 0)
        {
            clip_x0 = 0; clip_x1 = 320; clip_y0 = 0; clip_y1 = 240;
            for (int r = 0; r < MAP_ROWS; ++r)
                for (int c = 0; c < MAP_COLS; ++c)
                {
                    const coord_def gc = map_cell_grid(c, r);
                    const coord_def vc = gc - vorigin;
                    if (vc.x < 0 || vc.y < 0 || vc.x >= vsize.x || vc.y >= vsize.y)
                        continue;
                    pack_cell(c * TS, r * TS, vcells[vc.y * vsize.x + vc.x]);
                }
            flush_ops(cv);
        }
        else
        {
            // draw the visible cells at 16 px off screen, then scale up
            static uint8_t zpx[24 * TS * 17 * TS * 3];
            const int cols = zoom_cols(), rows = zoom_rows();
            Canvas zc = { cols * TS, rows * TS, zpx };
            memset(zpx, 0, (size_t)zc.w * zc.h * 3);
            clip_x0 = 0; clip_x1 = zc.w; clip_y0 = 0; clip_y1 = zc.h;
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < cols; ++c)
                {
                    const coord_def vc = zoom_cell_grid(c, r) - vorigin;
                    if (vc.x < 0 || vc.y < 0 || vc.x >= vsize.x || vc.y >= vsize.y)
                        continue;
                    pack_cell(c * TS, r * TS, vcells[vc.y * vsize.x + vc.x]);
                }
            flush_ops(zc);
            clip_x0 = 0; clip_x1 = 320; clip_y0 = 0; clip_y1 = 240;   // button icons
            const int zts = zoom_ts(), ox = zoom_ox(), oy = zoom_oy();
            for (int y = 0; y < 240; ++y)
            {
                const int sy = (y - oy) * TS / zts;
                if (y - oy < 0 || sy >= zc.h)
                    continue;
                uint8_t *d = cv.at(0, y);
                for (int x = 0; x < 320; ++x, d += 3)
                {
                    const int sx = (x - ox) * TS / zts;
                    if (x - ox < 0 || sx >= zc.w)
                        continue;
                    const uint8_t *s = zc.at(sx, sy);
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2];
                }
            }
        }
    }
    else
#endif
    {
        // no tiles: the viewport glyphs, 16 px cells
        const crawl_view_geometry &v = crawl_view;
        for (int r = 0; r < MAP_ROWS; ++r)
            for (int c = 0; c < MAP_COLS; ++c)
            {
                const coord_def vp = v.grid2view(map_cell_grid(c, r));
                const int gx = v.viewp.x - 1 + vp.x - 1, gy = v.viewp.y - 1 + vp.y - 1;
                if (vp.x < 1 || vp.y < 1 || vp.x > v.viewsz.x || vp.y > v.viewsz.y
                    || gx < 0 || gx >= GRID_W || gy < 0 || gy >= GRID_H)
                    continue;
                const cell_t &cell = grid[gy][gx];
                draw_char(cv, F_BOLD, c * TS + 5, r * TS + 1, cell.glyph, pal(cell.fg), nullptr);
            }
    }

    // edge button columns (outside the 15x15 line of sight)
    build_play_buttons();
    if (!buttons_hidden)
        for (size_t i = 0; i < buttons.size(); ++i)
            draw_button(cv, buttons[i], (int)i == pressed_button, true);

    // yes / no question: two big buttons over the map
    if (yesno_prompt_visible())
    {
        cv_shade(cv, 40, 176, 240, 56, 0, 0, 0, 170);
        const button yes = { 56, 182, 96, 44, "Yes", 'Y', BS_YES, 0 };
        const button no  = { 168, 182, 96, 44, "No", 'N', BS_NO, 0 };
        draw_button(cv, yes, false, false);
        draw_button(cv, no, false, false);
        buttons.push_back(yes);
        buttons.push_back(no);
    }
    else if (more_prompt_visible())
    {
        const char *m = "--more--  tap to continue";
        const int w = text_width(F_TEXT, m) + 12;
        cv_shade(cv, (320 - w) / 2, 222, w, 16, 0, 0, 0, 190);
        draw_text(cv, F_TEXT, (320 - w) / 2 + 6, 224, m, C_GOLD);
    }
}

// Bottom screen in text mode: the menu's entries as touch rows.
static std::vector<menu_entry> cur_entries;
static int entries_scroll = 0;

// Actions offered by the item / feature description crawl is showing
// ("Read", "Drop"...), set from describe.cc: big buttons above the bar, so
// no key has to be typed (the text says "Tap an action below").
static std::vector<std::pair<std::string, int>> g_actions;

void lib3ds_set_actions(const std::vector<std::pair<std::string, int>> &actions)
{
    g_actions = actions;
    bottom_dirty = true;
}

static void build_text_mode_buttons()
{
    buttons.clear();
    const int by = 240 - 30, bw = 64;
    const struct { const char *l; int k; int s; } bar[] = {
        { "Esc", ESCAPE, 0 }, { "Enter", CK_ENTER, 0 }, { "Pg Up", '<', 0 },
        { "Pg Dn", '>', 0 }, { "Keys", 0, BS_KEYBOARD },
    };
    for (int i = 0; i < 5; ++i)
        buttons.push_back({ i * bw, by, bw, 30, bar[i].l, bar[i].k, bar[i].s, 0 });
}

static void draw_text_mode_bottom()
{
    Canvas &cv = bot_cv;
    cv_fill(cv, 0, 0, 320, 240, 14, 14, 22);
    cur_entries = find_menu_entries();
    build_text_mode_buttons();
    const int n = (int)cur_entries.size();
    const int cols = n <= 10 ? 1 : n <= 20 ? 2 : 3;
    const int rows = (n + cols - 1) / cols;
    // action buttons (item descriptions) sit right above the bar
    const int na = (int)g_actions.size(), acols = 3, arows = (na + acols - 1) / acols;
    const int abh = 30, agap = 3, alimit = 240 - 30 - (na ? arows * (abh + agap) + 2 : 0);
    for (int i = 0; i < na; ++i)
    {
        const int aw = (320 - 4 - agap * (acols - 1)) / acols;
        const int ax = 2 + (i % acols) * (aw + agap), ay = alimit + 2 + (i / acols) * (abh + agap);
        buttons.push_back({ ax, ay, aw, abh, g_actions[i].first.c_str(), g_actions[i].second, 0, 0 });
    }
    const int nbar = 5 + na;
    const int rowh = std::max(14, std::min(24, (alimit - 2) / std::max(1, rows)));
    const int colw = 320 / cols;
    if (cur_entries.empty() && na)
        draw_text(cv, F_TEXT, 8, 8, "Tap an action:", C_DIM);
    else if (cur_entries.empty())
    {
        draw_text(cv, F_TEXT, 8, 8, "Use the buttons below, or Keys", C_DIM);
        draw_text(cv, F_TEXT, 8, 24, "for the touch keyboard.", C_DIM);
        draw_text(cv, F_TEXT, 8, 48, "A = Enter   B = Esc   D-pad = move", C_DIM);
        draw_text(cv, F_TEXT, 8, 64, "L / R = page up / down", C_DIM);
    }
    for (int i = 0; i < n && i < rows * cols; ++i)
    {
        const menu_entry &e = cur_entries[i];
        const int cc = i / rows, rr = i % rows;
        const int x = cc * colw + 2, y = 2 + rr * rowh;
        if (y + rowh > alimit)
            continue;
        // dim entries crawl draws in dark grey (unavailable choices)
        const int tfg = grid[e.row][e.tcol].fg;
        const bool dim = tfg == DARKGREY;
        cv_fill(cv, x, y, colw - 4, rowh - 2, dim ? 22 : 30, dim ? 22 : 32, dim ? 30 : 46);
        char k[2] = { (char)e.key, 0 };
        cv_fill(cv, x + 2, y + (rowh - 2 - 14) / 2, 12, 14, 70, 70, 110);
        draw_text(cv, F_BOLD, x + 5, y + (rowh - 2 - 13) / 2, k, C_GOLD);
        int px = x + 17;
        const int ty = y + (rowh - 2 - 13) / 2;
        for (int gx = e.tcol; gx < e.tend && px + F_TEXT.w <= x + colw - 4; ++gx)
        {
            const cell_t &c = grid[e.row][gx];
            if (c.glyph != ' ')
                draw_char(cv, F_TEXT, px, ty, c.glyph, pal(c.fg == BLACK ? LIGHTGREY : c.fg), nullptr);
            px += F_TEXT.w;
        }
        buttons.push_back({ x, y, colw - 4, rowh - 2, "", e.key, 0, 0 });
    }
    for (int i = 0; i < nbar; ++i)
        draw_button(cv, buttons[i], i == pressed_button, false);
}

// ===========================================================================
// Touch keyboard overlay
// ===========================================================================

struct softkey
{
    int x, y, w, h;
    const char *label;
    int key;
    int shifted;
    int special;
};

enum { SKEY_NONE = 0, SKEY_SHIFT = 1, SKEY_CTRL = 2, SKEY_LIGHT = 3, SKEY_CMD = 4 };

static const int KEY_W = 26, KEY_H = 34;
static const int KB_X0 = 4, KB_Y0 = 22;

static const char *row1 = "1234567890-=";
static const char *row1s = "!@#$%^&*()_+";
static const char *row2 = "qwertyuiop[]";
static const char *row2s = "QWERTYUIOP{}";
static const char *row3 = "asdfghjkl;'\\";
static const char *row3s = "ASDFGHJKL:\"|";
static const char *row4 = "zxcvbnm,./`";
static const char *row4s = "ZXCVBNM<>?~";

static std::vector<softkey> softkeys;
static bool sticky_shift = false;
static bool sticky_ctrl  = false;
static int  pressed_key_index = -1;

// Touch feedback (standard for every port): each tap on an on-screen key or
// button clicks and shows the control pressed for at least ~120 ms, so even
// a quick tap is seen.
static u64  press_until = 0;
static bool kb_close_pending = false;   // a quick command was tapped
static bool kb_auto = false;
static bool kb_dismissed = false;   // closed by hand during this prompt
static bool release_pending = false;
static void ui_tap_feedback()
{
#ifdef SOUND_PLAY_3DS
    if (Options.sounds_on)
        lib3ds_play_sound("romfs:/dat/sounds/x-click.pcm");
#endif
    press_until = osGetTime() + 120;
    bottom_dirty = true;
}
static void ui_feedback_tick()
{
    if (kb_close_pending && osGetTime() >= press_until)
    {
        kb_close_pending = false;
        keyboard_overlay = false;
        kb_dismissed = false;   // a prompt that follows (Search, Travel) may open it again
        kb_auto = false;
        pressed_key_index = -1;
        bottom_dirty = true;
    }
    if (release_pending && osGetTime() >= press_until)
    {
        release_pending = false;
        pressed_key_index = -1;
        pressed_button = -1;
        bottom_dirty = true;
    }
}

static void build_keyboard()
{
    softkeys.clear();
    static char labels[64][2];
    int nlabel = 0;
    auto add_row = [&](int row, const char *keys, const char *skeys, int x0)
    {
        for (int i = 0; keys[i]; ++i)
        {
            softkey k;
            k.x = x0 + i * KEY_W; k.y = KB_Y0 + row * KEY_H;
            k.w = KEY_W; k.h = KEY_H;
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

    const int by = KB_Y0 + 4 * KEY_H;
    const softkey bottom[] =
    {
        { KB_X0,       by, 44, KEY_H, "Shft", 0,        0, SKEY_SHIFT },
        { KB_X0 + 44,  by, 40, KEY_H, "Ctrl", 0,        0, SKEY_CTRL  },
        { KB_X0 + 84,  by, 36, KEY_H, "Esc",  ESCAPE,   0, SKEY_NONE  },
        { KB_X0 + 120, by, 36, KEY_H, "Tab",  CK_TAB,   0, SKEY_NONE  },
        { KB_X0 + 156, by, 64, KEY_H, "Space",' ',      0, SKEY_NONE  },
        { KB_X0 + 220, by, 40, KEY_H, "Bksp", CK_BKSP,  0, SKEY_NONE  },
        { KB_X0 + 260, by, 52, KEY_H, "Enter",CK_ENTER, 0, SKEY_NONE  },
    };
    for (const softkey &k : bottom)
        softkeys.push_back(k);
    // arrow keys in the top row (menus without hotkeys, history, cursor)
    const softkey arrows[] =
    {
        { 196, 0, 30, 21, "\u2190", CK_LEFT,  0, SKEY_NONE },
        { 226, 0, 30, 21, "\u2191", CK_UP,    0, SKEY_NONE },
        { 256, 0, 30, 21, "\u2193", CK_DOWN,  0, SKEY_NONE },
        { 286, 0, 30, 21, "\u2192", CK_RIGHT, 0, SKEY_NONE },
    };
    for (const softkey &k : arrows)
        softkeys.push_back(k);
    // screen brightness (label drawn with the current level)
    softkeys.push_back({ 120, 0, 72, 21, "Bright", 0, 0, SKEY_LIGHT });
    // quick commands under the keys: the commands without a play-screen
    // button, one tap each (the keyboard closes after the tap)
    static const struct { const char *label; int key; } quick[12] =
    {
        { "Fire", 'f' }, { "Learn", 'M' }, { "Look", 'x' }, { "Close", 'C' },
        { "Skills", 'm' }, { "Travel", 'G' },
        { "Search", CONTROL('F') }, { "Religion", '^' }, { "Overview", CONTROL('O') },
        { "Mutation", 'A' }, { "Swap", '\'' }, { "Orders", 't' },
    };
    const int qy = KB_Y0 + 5 * KEY_H + 2, qw = (320 - 2 * KB_X0) / 6, qh = (240 - qy) / 2;
    for (int i = 0; i < 12; ++i)
        softkeys.push_back({ KB_X0 + (i % 6) * qw, qy + (i / 6) * qh, qw, qh,
                             quick[i].label, quick[i].key, 0, SKEY_CMD });
}

static void draw_keyboard()
{
    Canvas &cv = bot_cv;
    cv_fill(cv, 0, 0, 320, 240, 18, 18, 26);
    char status[64];
    snprintf(status, sizeof(status), "SELECT: close");
    draw_text(cv, F_TEXT, 6, 4, status, C_DIM);

    for (size_t i = 0; i < softkeys.size(); ++i)
    {
        const softkey &k = softkeys[i];
        uint8_t kb[3] = { 46, 48, 62 };
        if ((int)i == pressed_key_index)
            kb[0] = 110, kb[1] = 110, kb[2] = 150;
        else if ((k.special == SKEY_SHIFT && sticky_shift) || (k.special == SKEY_CTRL && sticky_ctrl))
            kb[0] = 70, kb[1] = 120, kb[2] = 70;
        else if (k.special == SKEY_CMD)
            kb[0] = 40, kb[1] = 56, kb[2] = 96;     // quick commands: blue
        cv_fill(cv, k.x + 1, k.y + 1, k.w - 2, k.h - 2, kb[0], kb[1], kb[2]);
        const bool down_k = (int)i == pressed_key_index;
        if (down_k)
        {
            // pushed in: dark inner edge at top/left, label moves down 1 px
            cv_fill(cv, k.x + 1, k.y + 1, k.w - 2, 2, 30, 30, 44);
            cv_fill(cv, k.x + 1, k.y + 1, 2, k.h - 2, 30, 30, 44);
        }
        else
            cv_fill(cv, k.x + 1, k.y + k.h - 3, k.w - 2, 2, 28, 29, 38);   // key edge

        const char *label = k.label;
        char one[2] = { 0, 0 };
        if (k.special == SKEY_NONE && k.shifted && sticky_shift)
        {
            one[0] = (char)k.shifted;
            label = one;
        }
        char bright[16];
        if (k.special == SKEY_LIGHT)
        {
            snprintf(bright, sizeof(bright), "Bright %d", g_bright);
            label = bright;
        }
        const Font &f = strlen(label) == 1 ? F_BOLD : F_TEXT;
        draw_text(cv, f, k.x + (k.w - text_width(f, label)) / 2, k.y + (k.h - f.h) / 2 + (down_k ? 1 : 0), label, C_WHITE);
    }
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

// ===========================================================================
// Screen update
// ===========================================================================

static void draw_bottom()
{
    if (keyboard_overlay)
        draw_keyboard();
    else if (crt_mode || !g_actions.empty())
        draw_text_mode_bottom();
    else
        draw_play_bottom();
    cv_blit(bot_cv, GFX_BOTTOM, GFX_LEFT);
    gfxFlushBuffers();
    // double-buffered: we drew into the hidden buffer, show it at the next
    // VBlank (drawing into the visible one flickered). Presenting also makes
    // the GPU pick up the BGR8 format the startup console had changed.
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
    bottom_dirty = false;
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

static void draw_top()
{
    if (crt_mode)
        draw_text_mode_top();
    else
        draw_play_top();
    // the dashboard is flat: same image for both eyes
    cv_blit(top_cv, GFX_TOP, GFX_LEFT);
    cv_blit(top_cv, GFX_TOP, GFX_RIGHT);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, true);
    top_dirty = false;
}

// ===========================================================================
// Input
// ===========================================================================

static std::deque<int> keyq;
static std::deque<coord_def> tapq;       // map taps (grid coords)

// Physical buttons: play mode / text mode.
struct keymap_3ds
{
    u32 button;
    int play;
    int text;
};

static const keymap_3ds button_map[] =
{
    { KEY_DUP,    CK_UP,    CK_UP    },
    { KEY_DDOWN,  CK_DOWN,  CK_DOWN  },
    { KEY_DLEFT,  CK_LEFT,  CK_LEFT  },
    { KEY_DRIGHT, CK_RIGHT, CK_RIGHT },
    { KEY_A,      CK_TAB,   CK_ENTER },   // autofight / confirm
    { KEY_B,      ESCAPE,   ESCAPE   },
    { KEY_X,      'o',      0        },   // explore / (nothing: '?' would land in text fields)
    { KEY_Y,      'g',      ' '      },   // pick up / next page
    { KEY_L,      0,        '<'      },   // map zoom out / page up
    { KEY_R,      0,        '>'      },   // map zoom in / page down
    { KEY_ZL,     'i',      ESCAPE   },   // inventory (press again to close)
    { KEY_ZR,     'z',      ESCAPE   },   // cast (press again to close)
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

static u64 cpad_next_ms = 0;
static int cpad_last_key = 0;

static int button_at(int px, int py)
{
    for (int i = (int)buttons.size() - 1; i >= 0; --i)
    {
        const button &b = buttons[i];
        if (px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h)
            return i;
    }
    return -1;
}

static void press_button(const button &b)
{
    if (b.special == BS_KEYBOARD)
    {
        keyboard_overlay = true;
        return;
    }
    if (b.key)
        keyq.push_back(b.key);
}

// Text entry prompts (name, travel destination, search...) bring up the
// keyboard by themselves, and put it away again when the prompt ends.
static void auto_keyboard()
{
    // crawl shows the cursor while it reads a line of text (name, travel
    // destination, search, inscription...); menus and the map don't
    bool prompt = mouse_control::current_mode() == MOUSE_MODE_PROMPT;
    if (!prompt && cursor_is_enabled && region_of(cx, cy) != RG_VIEW)
        prompt = !crt_mode || find_menu_entries().empty();
    // full-screen text with nothing to tap (name entry, the main menu, help
    // pages): the keyboard (with arrows, Enter, Esc) is the useful thing
    if (!prompt && crt_mode && find_menu_entries().empty())
        prompt = true;
    if (!prompt)
        kb_dismissed = false;
    if (prompt && !keyboard_overlay && !kb_dismissed)
    {
        keyboard_overlay = kb_auto = true;
        bottom_dirty = true;
    }
    else if (!prompt && kb_auto)
    {
        keyboard_overlay = kb_auto = false;
        bottom_dirty = true;
    }
}

// The instruction manual (Start): text on the top screen, a button per
// section on the bottom; crawl waits underneath.
static manual3ds::Manual g_manual;

static void draw_manual()
{
    manual3ds::Canvas t = { top_cv.w, top_cv.h, top_cv.px };
    manual3ds::Canvas b = { bot_cv.w, bot_cv.h, bot_cv.px };
    g_manual.draw_top(t);
    g_manual.draw_bottom(b);
    cv_blit(top_cv, GFX_TOP, GFX_LEFT);
    cv_blit(top_cv, GFX_TOP, GFX_RIGHT);
    cv_blit(bot_cv, GFX_BOTTOM, GFX_LEFT);
    gfxFlushBuffers();
    gfxScreenSwapBuffers(GFX_TOP, true);
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

static bool manual_input(u32 down, u32 rep)
{
    manual3ds::Input mi;
    static const struct { u32 b; uint32_t k; bool rep; } map[] = {
        { KEY_DUP, manual3ds::K_UP, true }, { KEY_DDOWN, manual3ds::K_DOWN, true },
        { KEY_CSTICK_UP, manual3ds::K_SCROLL_UP, true }, { KEY_CSTICK_DOWN, manual3ds::K_SCROLL_DOWN, true },
        { KEY_DLEFT, manual3ds::K_LEFT, true }, { KEY_DRIGHT, manual3ds::K_RIGHT, true },
        { KEY_L, manual3ds::K_L, false }, { KEY_R, manual3ds::K_R, false },
        { KEY_A, manual3ds::K_A, false }, { KEY_B, manual3ds::K_B, false },
        { KEY_START, manual3ds::K_START, false },
    };
    for (auto &m : map)
        if ((m.rep ? rep : down) & m.b)
            mi.down |= m.k;
    if (down & KEY_TOUCH)
    {
        touchPosition tp;
        hidTouchRead(&tp);
        mi.touch_down = true;
        mi.tx = tp.px;
        mi.ty = tp.py;
    }
    circlePosition cp;
    hidCircleRead(&cp);
    mi.cpad_y = cp.dy;
    const bool was_dirty = g_manual.dirty();
    const bool open = g_manual.update(mi);
#ifdef SOUND_PLAY_3DS
    if (g_manual.take_click() && Options.sounds_on)
        lib3ds_play_sound("romfs:/dat/sounds/x-click.pcm");
#endif
    if (!open)
    {
        top_dirty = bottom_dirty = grid_all_dirty = true;   // back to the game
        return false;
    }
    if (mi.down || mi.touch_down || was_dirty || g_manual.dirty())
        draw_manual();
    return true;
}

static void poll_input()
{
    auto_keyboard();
    hidScanInput();
    const u32 down = hidKeysDown();
    const u32 rep  = hidKeysDownRepeat();

    if (g_manual.active())
    {
        manual_input(down, rep);
        return;
    }
    if (down & KEY_START)
    {
        g_manual.open();
        draw_manual();
        return;
    }

    if (down & KEY_SELECT)
    {
        keyboard_overlay = !keyboard_overlay;
        kb_dismissed = !keyboard_overlay;
        kb_auto = false;
        bottom_dirty = true;
    }

    const bool yesno = !crt_mode && yesno_prompt_visible();
    const bool more = !crt_mode && more_prompt_visible();
    // L / R: zoom the dungeon map out / in (remembered between sessions)
    if (!crt_mode && !keyboard_overlay && (down & (KEY_L | KEY_R)))
    {
        const int z = std::max(0, std::min(2, g_zoom + ((down & KEY_R) ? 1 : 0) - ((down & KEY_L) ? 1 : 0)));
        if (z != g_zoom)
        {
            g_zoom = z;
            bottom_dirty = top_dirty = true;
            if (FILE *f = fopen("sdmc:/3ds/crawl/zoom.txt", "w"))
            {
                fprintf(f, "%d\n", g_zoom);
                fclose(f);
            }
        }
    }
    for (const keymap_3ds &m : button_map)
    {
        const bool is_dir = m.button & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT);
        if ((is_dir ? rep : down) & m.button)
        {
            int k = crt_mode || keyboard_overlay ? m.text : m.play;
            if (!k)
                continue;
            if (yesno && m.button == KEY_A) k = 'Y';   // uppercase: accepted by every yes/no prompt
            if (yesno && m.button == KEY_B) k = 'N';
            if (more && m.button == KEY_A) k = CK_ENTER;
            keyq.push_back(k);
        }
    }

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

    if (down & KEY_TOUCH)
    {
        touchPosition tp;
        hidTouchRead(&tp);
        bottom_dirty = true;
        if (keyboard_overlay)
        {
            if (tp.py < KB_Y0 && tp.px < 120)
            {
                keyboard_overlay = kb_auto = false;
                kb_dismissed = true;
                return;
            }
            const int i = softkey_at(tp.px, tp.py);
            pressed_key_index = i;
            if (i >= 0)
            {
                ui_tap_feedback();
                const softkey &k = softkeys[i];
                if (k.special == SKEY_LIGHT)
                {
                    cycle_brightness();
                    top_dirty = true;
                }
                else if (k.special == SKEY_SHIFT)
                    sticky_shift = !sticky_shift;
                else if (k.special == SKEY_CTRL)
                    sticky_ctrl = !sticky_ctrl;
                else if (k.special == SKEY_CMD)
                {
                    // the command, then close the keyboard once the press has shown
                    sticky_shift = sticky_ctrl = false;
                    keyq.push_back(k.key);
                    kb_close_pending = true;
                }
                else
                {
                    int key = k.key;
                    if (k.shifted && sticky_shift)
                        key = k.shifted, sticky_shift = false;
                    if (sticky_ctrl && key > 0 && key < 128 && isalpha(key))
                        key = CONTROL(toupper(key)), sticky_ctrl = false;
                    keyq.push_back(key);
                }
            }
            return;
        }

        const int b = button_at(tp.px, tp.py);
        if (b >= 0 && (crt_mode || !buttons_hidden || buttons[b].special == BS_YES
                       || buttons[b].special == BS_NO))
        {
            pressed_button = b;
            ui_tap_feedback();
            press_button(buttons[b]);
            return;
        }
        if (crt_mode)
            return;
        if (more)
        {
            keyq.push_back(CK_ENTER);
            return;
        }
        // a map square
        if (g_zoom == 0)
        {
            const int c = tp.px / TS, r = tp.py / TS;
            if (c >= 0 && c < MAP_COLS && r >= 0 && r < MAP_ROWS)
                tapq.push_back(map_cell_grid(c, r));
        }
        else
        {
            const int dx = tp.px - zoom_ox(), dy = tp.py - zoom_oy();
            if (dx >= 0 && dy >= 0)
                tapq.push_back(zoom_cell_grid(dx / zoom_ts(), dy / zoom_ts()));
        }
    }
    if ((hidKeysUp() & KEY_TOUCH) && (pressed_key_index >= 0 || pressed_button >= 0))
        release_pending = true;          // cleared by ui_feedback_tick()
    ui_feedback_tick();
}

// A tap on the map, mirroring webtiles' cell click: travel / move / attack
// in command mode, select in targeting mode. Tapping yourself picks up or
// takes the stairs you stand on.
static int handle_tap(const coord_def &gc)
{
    const mouse_mode mode = mouse_control::current_mode();
    if (mode == MOUSE_MODE_COMMAND)
    {
        if (gc == you.pos())
        {
            const command_type dir = feat_stair_direction(env.grid(gc));
            if (dir == CMD_GO_UPSTAIRS)
                return '<';
            if (dir == CMD_GO_DOWNSTAIRS)
                return '>';
            if (env.igrid(gc) != NON_ITEM)
                return 'g';
            return 's';
        }
        if (!in_bounds(gc))
            return CK_MOUSE_CMD;
        const command_type c = click_travel(gc, false, false);
        if (c != CMD_NO_CMD)
        {
            clear_messages();
            process_command(c);
        }
        return CK_MOUSE_CMD;
    }
    if (mode == MOUSE_MODE_TARGET || mode == MOUSE_MODE_TARGET_DIR
        || mode == MOUSE_MODE_TARGET_PATH)
    {
        if (targeting_mouse_select(gc))
            return CK_MOUSE_CMD;
        return CK_REDRAW;
    }
    return CK_MOUSE_CLICK;
}

// ===========================================================================
// Early init (called from main() before anything touches the filesystem)
// ===========================================================================

static bool gfx_inited = false;
static bool headless = false;

bool in_headless_mode() { return headless; }

#ifndef __3DS_HARNESS__
// SQLite's unix VFS references these; the 3DS has no users or ownership.
extern "C" {
uid_t geteuid(void) { return 0; }
int fchown(int, uid_t, gid_t) { return 0; }
}

// newlib declares sigprocmask but the 3DS has no signals; crawl's webtiles
// framework (compiled in for the tile data) brackets socket writes with it.
#include <signal.h>
extern "C" int sigprocmask(int, const sigset_t *, sigset_t *old)
{
    if (old)
        memset(old, 0, sizeof(*old));
    return 0;
}
#endif

// ===========================================================================
// Sound: crawl's sound rules (dat/defaults/sounds3ds.txt, the CC0
// crawl-sounds pack) call lib3ds_play_sound(path). Clips are raw 16-bit mono
// PCM at 22050 Hz (converted from the pack's OGGs at build time), loaded on
// first use and kept; four ndsp channels in rotation so effects can overlap.
// ===========================================================================
#ifdef SOUND_PLAY_3DS
namespace {
struct snd_clip { int16_t *pcm = nullptr; u32 samples = 0; };
std::map<std::string, snd_clip> *snd_cache = nullptr;
bool snd_ok = false;
#ifndef __3DS_HARNESS__
ndspWaveBuf snd_buf[4];
#endif
int snd_next = 0;
}

static void sound_init()
{
#ifndef __3DS_HARNESS__
    const Result rc = ndspInit();       // needs sdmc:/3ds/dspfirm.cdc
    snd_ok = R_SUCCEEDED(rc);
    if (FILE *lf = fopen("sdmc:/3ds/crawl/sound.log", "w"))
    {
        fprintf(lf, "ndspInit: %08lX (%s)\n", (unsigned long)rc, snd_ok ? "ok" : "FAILED");
        fclose(lf);
    }
    if (!snd_ok)
        return;
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    for (int ch = 0; ch < 4; ++ch)
    {
        ndspChnReset(ch);
        ndspChnSetInterp(ch, NDSP_INTERP_LINEAR);
        ndspChnSetRate(ch, 22050.0f);
        ndspChnSetFormat(ch, NDSP_FORMAT_MONO_PCM16);
        float mix[12] = { 0.9f, 0.9f };
        ndspChnSetMix(ch, mix);
    }
    memset(snd_buf, 0, sizeof(snd_buf));
#else
    snd_ok = true;
#endif
    snd_cache = new std::map<std::string, snd_clip>;
}

static void sound_shutdown()
{
#ifndef __3DS_HARNESS__
    if (snd_ok)
    {
        for (int ch = 0; ch < 4; ++ch)
            ndspChnWaveBufClear(ch);
        ndspExit();
    }
#endif
    snd_ok = false;     // clips stay allocated: the process is ending
}

void lib3ds_play_sound(const char *file)
{
    static int logged = 0;               // first plays go to sound.log
    if (logged < 40 && !strstr(file ? file : "", "x-step") && !strstr(file ? file : "", "x-click"))
    {
        if (FILE *lf = fopen("sdmc:/3ds/crawl/sound.log", "a"))
        {
            fprintf(lf, "play %s%s\n", file ? file : "(null)", snd_ok ? "" : " (sound off)");
            fclose(lf);
        }
        ++logged;
    }
    if (!snd_ok || !snd_cache || !file || !*file)
        return;
    std::string path(file);
    auto it = snd_cache->find(path);
    if (it == snd_cache->end())
    {
        snd_clip clip;
        if (FILE *f = fopen(path.c_str(), "rb"))
        {
            fseek(f, 0, SEEK_END);
            const long bytes = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (bytes > 0 && bytes < 4 * 1024 * 1024)
            {
#ifndef __3DS_HARNESS__
                clip.pcm = (int16_t *)linearAlloc(bytes);
#else
                clip.pcm = (int16_t *)malloc(bytes);
#endif
                if (clip.pcm && fread(clip.pcm, 1, bytes, f) == (size_t)bytes)
                    clip.samples = bytes / 2;
            }
            fclose(f);
#ifndef __3DS_HARNESS__
            if (clip.samples)
                DSP_FlushDataCache(clip.pcm, clip.samples * 2);
#endif
        }
        if (FILE *lf = fopen("sdmc:/3ds/crawl/sound.log", "a"))
        {
            fprintf(lf, "load %s: %u samples\n", path.c_str(), (unsigned)clip.samples);
            fclose(lf);
        }
        it = snd_cache->emplace(path, clip).first;   // misses are cached too
    }
    if (!it->second.samples)
        return;
#ifndef __3DS_HARNESS__
    const int ch = snd_next;
    snd_next = (snd_next + 1) % 4;
    ndspChnWaveBufClear(ch);
    ndspWaveBuf &b = snd_buf[ch];
    memset(&b, 0, sizeof(b));
    b.data_vaddr = it->second.pcm;
    b.nsamples = it->second.samples;
    ndspChnWaveBufAdd(ch, &b);
#else
    fprintf(stderr, "harness: sound %s (%u samples)\n", path.c_str(), (unsigned)it->second.samples);
#endif
}
#endif // SOUND_PLAY_3DS


#ifndef __3DS_HARNESS__
// Stable timestamp for every romfs file (see files.cc patch): the mtime of
// the .3dsx we were launched from, so caches rebuild once per install.
extern "C" { extern int __system_argc; extern char **__system_argv; }
time_t lib3ds_romfs_mtime()
{
    static time_t t = 0;
    if (t)
        return t;
    // Preferred: a fingerprint of the game data, written into romfs at build
    // time (Makefile.3ds-rules), so a new build with the same data doesn't
    // rebuild the databases and level cache.
    if (FILE *f = fopen("romfs:/stamp.txt", "r"))
    {
        unsigned long v = 0;
        if (fscanf(f, "%lu", &v) == 1 && v)
            t = (time_t)(1000000000UL + v % 1000000000UL);
        fclose(f);
        if (t)
            return t;
    }
    struct stat st;
    const char *self = (__system_argc > 0 && __system_argv && __system_argv[0])
                       ? __system_argv[0] : nullptr;
    if (self && !stat(self, &st) && st.st_mtime > 0)
        t = st.st_mtime;
    else if (!stat("sdmc:/3ds/crawl/crawl.3dsx", &st) && st.st_mtime > 0)
        t = st.st_mtime;
    else
        t = 1700000000;   // no executable to look at: one fixed value
    return t;
}
#else
// Desktop harness: same data fingerprint, so caches it builds are valid on
// the 3DS (tools: build caches here, copy saves/db + saves/des to the card).
time_t lib3ds_romfs_mtime()
{
    static time_t t = 0;
    if (!t)
    {
        unsigned long v = 0;
        if (FILE *f = fopen("romfs:/stamp.txt", "r"))
        {
            if (fscanf(f, "%lu", &v) != 1)
                v = 0;
            fclose(f);
        }
        t = v ? (time_t)(1000000000UL + v % 1000000000UL) : 1700000000;
    }
    return t;
}
#endif

void enter_headless_mode() { headless = true; }

// Boot trace: one line per startup step in sdmc:/3ds/crawl/boot.log, to see how far a launch gets
// (an emulator that exits in two seconds leaves no other trace).
void lib3ds_boot_mark(const char *what)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/crawl", 0777);
    if (FILE *f = fopen("sdmc:/3ds/crawl/boot.log", "a"))
    {
        fprintf(f, "%s\n", what);
        fclose(f);
    }
}
#define boot_mark lib3ds_boot_mark
static struct BootMarkStatic { BootMarkStatic() { boot_mark("static init (before main)"); } } g_boot_mark_static;

void lib3ds_early_init()
{
    boot_mark("early_init: start");
    gfxInitDefault();
    gfx_inited = true;
    boot_mark("early_init: gfxInitDefault ok");
    // Debug console on the bottom screen so startup errors and the first-run
    // database / level-cache progress are visible; console_startup() takes
    // the screens over later.
    consoleInit(GFX_BOTTOM, nullptr);
    printf("Dungeon Crawl Stone Soup for 3DS\n\n");
    printf("The first start builds the game databases\n");
    printf("and level cache: this takes a few minutes.\n\n");

    Result rc = romfsInit();
    if (R_FAILED(rc))
        printf("romfsInit failed: %08X\n", (unsigned)rc);

    boot_mark(R_FAILED(rc) ? "early_init: romfsInit FAILED" : "early_init: romfsInit ok");
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/crawl", 0777);
    mkdir("sdmc:/3ds/crawl/saves", 0777);
    mkdir("sdmc:/3ds/crawl/morgue", 0777);

    // SQLite's default unix VFS locks files with fcntl(), which newlib on the
    // 3DS doesn't implement. Only Crawl touches its databases.
    if (sqlite3_vfs *nolock = sqlite3_vfs_find("unix-none"))
        sqlite3_vfs_register(nolock, 1);

    osSetSpeedupEnable(true);
    boot_mark("early_init: done");
}

// Called from end() on a fatal exit: show the reason and wait.
void lib3ds_fatal_wait(const char *msg)
{
    if (!gfx_inited)
    {
        gfxInitDefault();
        gfx_inited = true;
    }
    gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
    consoleInit(GFX_BOTTOM, nullptr);
    printf("\x1b[2J\x1b[1;1HDCSS 3DS stopped with an error:\n\n%s\n\nPress START to exit.\n", msg ? msg : "");
    while (aptMainLoop())
    {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

// ===========================================================================
// libconsole.h implementation
// ===========================================================================


#ifndef __3DS_HARNESS__
// Before the HOME Menu takes over it captures our framebuffers. Hand it the
// plainest setup (2D, both BGR8, a fresh frame presented) and restore
// stereo when we come back.
static aptHookCookie home_cookie;
// HOME diagnostics: what the HOME Menu was handed, in sdmc:/3ds/crawl/home.log
static void home_log(const char *what)
{
    FILE *f = fopen("sdmc:/3ds/crawl/home.log", "a");
    if (!f)
        return;
    GSPGPU_CaptureInfo ci;
    memset(&ci, 0, sizeof(ci));
    GSPGPU_ImportDisplayCaptureInfo(&ci);
    fprintf(f, "%llu %s: app free %lu KB, linear free %lu KB, sys free %lu KB, base free %lu KB\n",
            (unsigned long long)osGetTime(), what,
            (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024),
            (unsigned long)(linearSpaceFree() / 1024),
            (unsigned long)(osGetMemRegionFree(MEMREGION_SYSTEM) / 1024),
            (unsigned long)(osGetMemRegionFree(MEMREGION_BASE) / 1024));
    fprintf(f, "%llu %s: top fmt=%02lx fb0=%p fb1=%p | bot fmt=%02lx fb0=%p fb1=%p\n",
            (unsigned long long)osGetTime(), what,
            (unsigned long)ci.screencapture[0].format, ci.screencapture[0].framebuf0_vaddr,
            ci.screencapture[0].framebuf1_vaddr,
            (unsigned long)ci.screencapture[1].format, ci.screencapture[1].framebuf0_vaddr,
            ci.screencapture[1].framebuf1_vaddr);
    fclose(f);
}
static void home_hook(APT_HookType type, void *)
{
    static const char *names[] = { "suspend", "restore", "sleep", "wakeup", "exit" };
    home_log(type >= 0 && type < 5 ? names[type] : "?");
    if (!inited)
        return;
    if (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONEXIT)
    {
        gfxSet3D(false);
        gfxSetDoubleBuffering(GFX_TOP, true);
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        for (int i = 0; i < 2; ++i)
        {
            cv_blit(top_cv, GFX_TOP, GFX_LEFT);
            cv_blit(bot_cv, GFX_BOTTOM, GFX_LEFT);
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
        }
        if (type == APTHOOK_ONSUSPEND && you.save)
        {
            you.save->lib3ds_close_fd();   // see package.cc: HOME Menu + open save
            home_log("save file closed");
        }
        home_log("suspend-ready");
    }
    else if (type == APTHOOK_ONRESTORE || type == APTHOOK_ONWAKEUP)
    {
        if (you.save)
            you.save->lib3ds_reopen_fd();
        gfxSet3D(true);
        top_dirty = bottom_dirty = grid_all_dirty = true;
    }
}
#endif

void console_startup()
{
    if (inited)
        return;
    boot_mark("console_startup: start");
    if (!gfx_inited)
    {
        gfxInitDefault();
        gfx_inited = true;
    }
    // From here on we own both screens. Anything crawl still prints to
    // stdout/stderr goes to a log file: the libctru console would draw 16-bit
    // text into our 24-bit framebuffer (that was the garbled bottom screen).
    fflush(stdout);
    fflush(stderr);
#ifndef __3DS_HARNESS__
    freopen("sdmc:/3ds/crawl/stdout.txt", "w", stdout);
    freopen("sdmc:/3ds/crawl/stdout.txt", "a", stderr);
#endif

    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);    // draw hidden, then flip: no flicker
    gfxSetDoubleBuffering(GFX_BOTTOM, true);
    gfxSet3D(true);
    osSetSpeedupEnable(true);           // New 3DS: 804 MHz + L2
    crt_mode = true;
    hidSetRepeatParameters(18, 5);

    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
            grid[y][x] = { ' ', LIGHTGREY, BLACK, 1 };
    grid_all_dirty = true;

    load_atlas();
    load_brightness();
    if (FILE *f = fopen("sdmc:/3ds/crawl/zoom.txt", "r"))
    {
        int z;
        if (fscanf(f, "%d", &z) == 1 && z >= 0 && z <= 2)
            g_zoom = z;
        fclose(f);
    }
#ifndef __3DS_HARNESS__
    aptHook(&home_cookie, home_hook, nullptr);
    home_log("startup");
#endif
#ifdef SOUND_PLAY_3DS
    sound_init();
#endif
    g_manual.load(kManualText, "sdmc:/3ds/crawl/manual.txt", "Crawl for 3DS: manual");
    boot_mark("console_startup: sound+manual ok");
    build_keyboard();
    bottom_dirty = top_dirty = true;

    textcolour(LIGHTGREY);
    cursor_is_enabled = false;
    inited = true;

    crawl_state.terminal_resize_handler = nullptr;
    crawl_state.terminal_resize_check   = nullptr;

    draw_bottom();
    draw_top();
}

void console_shutdown()
{
    if (!inited)
        return;
    inited = false;
#ifdef USE_TILE
    vcells.clear();
    vcells.shrink_to_fit();
    have_view = false;
#endif
#ifndef __3DS_HARNESS__
    aptUnhook(&home_cookie);
#endif
#ifdef SOUND_PLAY_3DS
    sound_shutdown();
#endif
    // no romfsExit(): files may still be open and are closed after this
    gfxExit();
}

bool is_cursor_enabled()
{
    return cursor_is_enabled;
}

void set_cursor_enabled(bool enabled)
{
    if (cursor_is_enabled != enabled)
    {
        cursor_is_enabled = enabled;
        if (cx >= 0 && cx < GRID_W && cy >= 0 && cy < GRID_H)
            grid[cy][cx].dirty = 1;
    }
}

void set_mouse_enabled(bool /*enabled*/)
{
}

COLOURS default_hover_colour()
{
    return BLUE;
}

// Menus park a "fake cursor" on the selected line; we have a real one.
void fakecursorxy(int x, int y)
{
    gotoxy_sys(x, y);
    set_cursor_region(GOTO_CRT);
}

void clear_to_end_of_line()
{
    for (int x = cx; x < GRID_W; ++x)
        grid[cy][x] = { ' ', current_fg, current_bg, 1 };
}

void clrscr_sys()
{
    for (int y = 0; y < GRID_H; ++y)
        for (int x = 0; x < GRID_W; ++x)
            grid[y][x] = { ' ', current_fg, current_bg, 1 };
    cx = cy = 0;
    grid_all_dirty = true;
}

static int view_text_run = 0;   // chars written in a row over the map, outside puttext()

void gotoxy_sys(int x, int y)
{
    view_text_run = 0;
    if (cursor_is_enabled && cx >= 0 && cx < GRID_W && cy >= 0 && cy < GRID_H)
        grid[cy][cx].dirty = 1;
    cx = x - 1;
    cy = y - 1;
    if (cx < 0) cx = 0;
    if (cx >= GRID_W) cx = GRID_W - 1;
    if (cy < 0) cy = 0;
    if (cy >= GRID_H) cy = GRID_H - 1;
    if (cursor_is_enabled)
        grid[cy][cx].dirty = 1;
}

static unsigned short _dos_reverse_highlight(unsigned short colour)
{
    return (colour & 0xF) << 4 | (colour >> 4 & 0xF);
}

static unsigned short _dos_do_highlight(unsigned short colour,
                                        unsigned short highlight)
{
    return (colour & 0xF0) | highlight;
}

static unsigned short _dos_highlight(unsigned short colour, unsigned highlight)
{
    if (highlight == CHATTR_REVERSE)
        return _dos_reverse_highlight(colour);
    if ((highlight & CHATTR_ATTRMASK) == CHATTR_HILITE)
        return _dos_do_highlight(colour, (highlight & CHATTR_COLMASK) >> 8);
    return colour;
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
       (col & COLFLAG_REVERSE)          ? CHATTR_REVERSE
                                        : CHATTR_NORMAL;
}

static void update_text_colours(int highlight)
{
    unsigned short fg = FG_COL, bg = BG_COL;
    if (highlight != CHATTR_NORMAL)
    {
        const unsigned short c = _dos_highlight(fg | bg << 4, highlight);
        fg = c & 0xF;
        bg = c >> 4 & 0xF;
    }
    current_fg = fg;
    current_bg = bg;
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
    // A run of text across the viewport that did not come from puttext()
    // means crawl is drawing a full-screen interface over the map. Single
    // squares redrawn in place (a monster dying, beams, missiles) are not.
    if (!in_puttext && region_of(cx, cy) == RG_VIEW)
        ++view_text_run;
    else
        view_text_run = 0;
    if (!in_puttext && !crt_mode && c != ' ' && view_text_run >= 6
        && region_of(cx, cy) == RG_VIEW)
    {
        crt_mode = true;
        grid_all_dirty = true;
        bottom_dirty = true;
        entries_scroll = 0;
    }
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
            if (you.save)
                you.save->lib3ds_reopen_fd();
            crawl_state.seen_hups = true;
            return ESCAPE;
        }
        poll_input();
        if (!tapq.empty())
        {
            const coord_def gc = tapq.front();
            tapq.pop_front();
            const int k = handle_tap(gc);
            update_screen();
            return k;
        }
        ui_feedback_tick();
        if (!g_manual.active())
        {
            if (top_dirty)
                draw_top();
            if (bottom_dirty)
                draw_bottom();
        }
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
    if (keyq.empty() && tapq.empty())
    {
        if (!aptMainLoop())
        {
            if (you.save)
                you.save->lib3ds_reopen_fd();
            crawl_state.seen_hups = true;
            return false;
        }
        poll_input();
        if (bottom_dirty && !g_manual.active())
            draw_bottom();
    }
    return !keyq.empty() || !tapq.empty();
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
    // viewport ends text mode. Anything else (the full-screen level map,
    // the arena...) is a full-screen interface and keeps/enters text mode.
    const coord_def size = vbuf.size();
    const bool is_viewport = x1 == crawl_view.viewp.x && y1 == crawl_view.viewp.y
                             && size == crawl_view.viewsz;
    in_puttext = is_viewport;
    if (crt_mode == is_viewport)
    {
        crt_mode = !is_viewport;
        grid_all_dirty = true;
        bottom_dirty = true;
        // back to the dungeon: put away a keyboard opened for a menu
        if (!crt_mode)
            keyboard_overlay = false;
    }
    const screen_cell_t *cell = vbuf;
    if (is_viewport)
    {
        vsize = size;
        vcentre = crawl_view.vgrdc;
        vorigin = crawl_view.view2grid(coord_def(1, 1));
#ifdef USE_TILE
        vcells.resize(size.x * size.y);
        for (int i = 0; i < size.x * size.y; ++i)
            vcells[i] = cell[i].tile;
#endif
        have_view = true;
        bottom_dirty = true;
#ifdef SOUND_PLAY_3DS
        // soft footsteps: crawl prints no message for an ordinary step, so
        // watch the player's square (one step at a time, same level)
        static coord_def last_pos;
        static int last_depth = -1, last_branch = -1, step_alt = 0;
        const coord_def p = you.pos();
        if (map_bounds(p) && p != last_pos && last_depth == you.depth
            && last_branch == (int)you.where_are_you && (p - last_pos).abs() <= 2)
        {
            lib3ds_play_sound(step_alt ? "romfs:/dat/sounds/x-step2.pcm"
                                       : "romfs:/dat/sounds/x-step1.pcm");
            step_alt ^= 1;
        }
        last_pos = p;
        last_depth = you.depth;
        last_branch = (int)you.where_are_you;
#endif
    }
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
    {
        top_dirty = true;
        bottom_dirty = true;
    }
    if (top_dirty)
        draw_top();
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
