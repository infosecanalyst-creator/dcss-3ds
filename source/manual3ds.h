// manual3ds.h -- the in-game instruction manual shared by the 3DS ports
// (Crawl, Micropolis, X-COM, ScummVM). Header-only, no libctru dependency:
// the frontend hands it landscape RGB888 canvases and a small input record.
//
//   Top screen:    the section's text, scrolled (Circle Pad / D-pad), with a
//                  header (section title, n/N) and a footer with the keys.
//   Bottom screen: one big button per section (tap), and Close.
//   L / R:         previous / next section.  B or Start: close.
//   It opens again at the section and place it was left at.
//
// Text format (a small subset of Markdown), built in and optionally replaced
// by a file on the SD card:
//   "# Title"   starts a section        "## Heading"  a heading
//   "- item"    a bullet                "1. step"     a numbered step
//   "| a | b"   a two-column table row  blank line    ends a paragraph
#pragma once
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "font6x13.h"
#include "font6x13b.h"

// Standard controls for every port's manual (keep them identical everywhere):
//   scroll text ....... Circle Pad or C-Stick up/down (hold to keep scrolling)
//   choose section .... D-Pad moves around the section buttons on the bottom
//                       screen; L/R = previous/next; or tap a section
//   (the D-Pad never scrolls the text)
// Touch feedback (also standard): a tapped button shows pressed for ~120 ms,
// and take_click() reports each tap / section change so the game can play
// its click sound.
//   close ............. B or START (or the Close button)
// The game must not see these inputs while the manual is open.
// A libctru port can build the Input with input_from_hid() below: define
// MANUAL3DS_HID and include <3ds.h> before this header.

namespace manual3ds {

struct Canvas
{
    int w, h;
    uint8_t *px;               // RGB888, row-major
};

enum Key
{
    K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_L = 16, K_R = 32,
    K_A = 64, K_B = 128, K_START = 256, K_X = 512, K_Y = 1024,
    K_SCROLL_UP = 2048, K_SCROLL_DOWN = 4096,   // C-Stick (digital, with repeat)
};

struct Input
{
    uint32_t down = 0;        // pressed this frame (with repeat for directions)
    bool touch_down = false;  // stylus went down this frame
    int tx = 0, ty = 0;
    int cpad_y = 0;           // -156..156, up positive
};

// ---------------------------------------------------------------------------
namespace detail {

struct Font { int w, h, n; const uint16_t *cp; const uint8_t *bits; };
static const Font REG  = { FONT6X13_W, FONT6X13_H, FONT6X13_N, font6x13_cp, &font6x13_bits[0][0] };
static const Font BOLD = { FONT6X13B_W, FONT6X13B_H, FONT6X13B_N, font6x13b_cp, &font6x13b_bits[0][0] };

inline int find(const Font &f, uint32_t c)
{
    int lo = 0, hi = f.n - 1;
    while (lo <= hi)
    {
        const int m = (lo + hi) / 2;
        if (f.cp[m] == c) return m;
        if (f.cp[m] < c) lo = m + 1; else hi = m - 1;
    }
    return -1;
}

inline uint32_t next_cp(const char *&s)
{
    const unsigned char *p = (const unsigned char *)s;
    uint32_t c = *p++;
    if (c >= 0xC0 && c < 0xE0 && *p) c = ((c & 0x1F) << 6) | (*p++ & 0x3F);
    else if (c >= 0xE0 && c < 0xF0 && p[0] && p[1]) { c = ((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    s = (const char *)p;
    return c;
}

inline int ulen(const std::string &s)
{
    int n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++n;
    return n;
}

inline void fill(Canvas &c, int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b)
{
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > c.w) w = c.w - x0;
    if (y0 + h > c.h) h = c.h - y0;
    for (int y = y0; y < y0 + h; ++y)
    {
        uint8_t *p = c.px + (y * c.w + x0) * 3;
        for (int x = 0; x < w; ++x, p += 3) { p[0] = r; p[1] = g; p[2] = b; }
    }
}

inline int text(Canvas &cv, int x, int y, const std::string &str, const uint8_t *col,
                bool bold = false, int maxx = 100000)
{
    const char *s = str.c_str();
    while (*s)
    {
        uint32_t c = next_cp(s);
        const Font *f = bold ? &BOLD : &REG;
        int i = find(*f, c);
        if (i < 0 && bold) { f = &REG; i = find(*f, c); }
        if (i < 0) { f = &REG; i = find(*f, '?'); }
        if (x + f->w > maxx) break;
        const uint8_t *rows = f->bits + i * f->h;
        for (int yy = 0; yy < f->h; ++yy)
        {
            const int py = y + yy;
            if (py < 0 || py >= cv.h) continue;
            for (int xx = 0; xx < f->w; ++xx)
            {
                const int px = x + xx;
                if (px < 0 || px >= cv.w) continue;
                if (rows[yy] & (1 << (f->w - 1 - xx)))
                {
                    uint8_t *p = cv.px + (py * cv.w + px) * 3;
                    p[0] = col[0]; p[1] = col[1]; p[2] = col[2];
                }
            }
        }
        x += 6;
    }
    return x;
}

// A laid-out line of the top-screen page.
struct Line
{
    enum Kind { BLANK, PARA, HEAD, BULLET, STEP, TABLE } kind;
    std::string a, b;          // text; b = second table column
    bool first;                // first visual line of a bullet / step
    std::string marker;        // "1." for steps
};

} // namespace detail

// ---------------------------------------------------------------------------
class Manual
{
public:
    // Load the built-in text, replaced by `override_path` if that file exists.
    void load(const char *built_in, const char *override_path = nullptr, const char *title = "Manual")
    {
        m_title = title;
        std::string text = built_in ? built_in : "";
        if (override_path)
            if (FILE *f = fopen(override_path, "rb"))
            {
                std::string t;
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    t.append(buf, n);
                fclose(f);
                if (!t.empty())
                    text = t;
            }
        parse(text);
    }

    bool active() const { return m_active; }
    void open() { m_active = true; m_dirty = true; m_bar_focus = 0; }
    void close() { m_active = false; }
    bool dirty() const { return m_dirty; }

    // Feed one frame of input. Returns true while the manual stays open.
    // true once per tap or section change: play the UI click
    bool take_click() { const bool c = m_click; m_click = false; return c; }
    // Optional host toggle button on the bottom screen (e.g. "Left hand: off").
    // Empty label = no button. take_toggle() reports one tap.
    void set_toggle(const std::string &label) { m_toggle = label; m_dirty = true; }
    // Footer hint on the top screen (empty = the default). Lets a port show left-handed labels.
    void set_footer(const std::string &f) { if (f != m_footer) { m_footer = f; m_dirty = true; } }
    bool take_toggle() { const bool t = m_toggle_hit; m_toggle_hit = false; return t; }

    bool update(const Input &in)
    {
        if (m_press_frames > 0 && --m_press_frames == 0)
            m_dirty = true;                  // pressed look ends
        if (!m_active)
            return false;
        const int nsec = (int)m_sections.size();
        const int sec_before = m_sec;
        if (in.down & (K_B | K_START))
        {
            m_active = false;
            return false;
        }
        // C-Stick scrolls the text
        if (in.down & K_SCROLL_UP)   scroll(-1);
        if (in.down & K_SCROLL_DOWN) scroll(+1);
        // D-Pad moves around the section buttons (same grid as build_buttons); down from the
        // bottom row reaches the button row (toggle / Close), A presses the one highlighted
        if (nsec && (in.down & (K_UP | K_DOWN | K_LEFT | K_RIGHT)))
        {
            const int cols = nsec > 6 ? 2 : 1, rows = (nsec + cols - 1) / cols;
            const int c = m_sec / rows, r = m_sec % rows;
            const int nbar = m_toggle.empty() ? 1 : 2;   // buttons in the bottom row
            if (m_bar_focus)
            {
                if (in.down & K_UP)                          { m_bar_focus = 0; m_dirty = true; }
                if ((in.down & K_LEFT) && m_bar_focus > 1)   { --m_bar_focus; m_dirty = true; }
                if ((in.down & K_RIGHT) && m_bar_focus < nbar) { ++m_bar_focus; m_dirty = true; }
            }
            else
            {
                int s = m_sec;
                if ((in.down & K_UP) && r > 0)                      s = m_sec - 1;
                if (in.down & K_DOWN)
                {
                    if (r < rows - 1 && m_sec + 1 < nsec) s = m_sec + 1;
                    else { m_bar_focus = (nbar == 2 && c > 0) ? 2 : 1; m_dirty = true; }
                }
                if ((in.down & K_LEFT) && c > 0)                    s = m_sec - rows;
                if ((in.down & K_RIGHT) && c < cols - 1)            s = std::min(m_sec + rows, nsec - 1);
                if (s != m_sec)
                {
                    m_sec = s;
                    m_scroll = 0;
                    m_dirty = true;
                }
            }
        }
        if ((in.down & K_A) && m_bar_focus)
        {
            const int id = (m_toggle.empty() || m_bar_focus == 2) ? -2 : -3;
            m_press_btn = id; m_press_frames = 7; m_click = true; m_dirty = true;
            if (id == -3) m_toggle_hit = true;
            else { m_bar_focus = 0; m_active = false; return false; }
        }
        if (in.down & (K_L | K_R) && nsec)
        {
            m_sec = (m_sec + (in.down & K_L ? nsec - 1 : 1)) % nsec;
            m_scroll = 0;
            m_dirty = true;
        }
        // Circle Pad: smooth-ish scrolling
        if (in.cpad_y > 40 || in.cpad_y < -40)
        {
            m_cpad_acc += in.cpad_y > 0 ? -(in.cpad_y - 30) : -(in.cpad_y + 30);
            while (m_cpad_acc >= 700) { scroll(+1); m_cpad_acc -= 700; }
            while (m_cpad_acc <= -700) { scroll(-1); m_cpad_acc += 700; }
        }
        else
            m_cpad_acc = 0;
        if (in.touch_down)
        {
            const int b = button_at(in.tx, in.ty);
            if (b != -1)
            {
                m_press_btn = b;
                m_press_frames = 7;          // ~120 ms at 60 fps
                m_click = true;
                m_dirty = true;
            }
            if (b == -3)
                m_toggle_hit = true;
            if (b == -2)
            {
                m_active = false;
                return false;
            }
            if (b >= 0 && b != m_sec)
            {
                m_sec = b;
                m_scroll = 0;
                m_dirty = true;
            }
        }
        if (m_sec != sec_before)
            m_click = true;                  // D-Pad / L / R moved to another section
        return m_active;
    }

    void draw_top(Canvas &cv)
    {
        using namespace detail;
        static const uint8_t C_HEAD[3] = { 250, 220, 120 }, C_TEXT[3] = { 222, 222, 228 },
                             C_DIM[3] = { 140, 140, 155 }, C_SUB[3] = { 140, 200, 255 },
                             C_MARK[3] = { 250, 200, 90 };
        m_view_h = cv.h;
        fill(cv, 0, 0, cv.w, cv.h, 14, 14, 22);
        fill(cv, 0, 0, cv.w, HEAD, 34, 34, 56);
        if (m_sections.empty())
        {
            text(cv, 8, 6, "No manual text.", C_TEXT);
            m_dirty = false;
            return;
        }
        layout(cv.w);
        const Section &s = m_sections[m_sec];
        text(cv, 8, 5, s.title, C_HEAD, true, cv.w - 60);
        char pg[16];
        snprintf(pg, sizeof(pg), "%d/%d", m_sec + 1, (int)m_sections.size());
        text(cv, cv.w - 8 - 6 * (int)strlen(pg), 5, pg, C_DIM);

        const int rows = body_rows(), y0 = HEAD + 4;
        for (int i = 0; i < rows && m_scroll + i < (int)m_lines.size(); ++i)
        {
            const Line &l = m_lines[m_scroll + i];
            const int y = y0 + i * LINE_H;
            switch (l.kind)
            {
            case Line::HEAD:   text(cv, MARGIN, y, l.a, C_SUB, true); break;
            case Line::PARA:   text(cv, MARGIN, y, l.a, C_TEXT); break;
            case Line::BULLET:
                if (l.first) text(cv, MARGIN + 2, y, "•", C_MARK);
                text(cv, MARGIN + 14, y, l.a, C_TEXT);
                break;
            case Line::STEP:
                if (l.first) text(cv, MARGIN, y, l.marker, C_MARK, true);
                text(cv, MARGIN + 20, y, l.a, C_TEXT);
                break;
            case Line::TABLE:
                text(cv, MARGIN, y, l.a, C_MARK, true, MARGIN + TABLE_COL - 6);
                text(cv, MARGIN + TABLE_COL, y, l.b, C_TEXT);
                break;
            default: break;
            }
        }
        // scroll bar
        const int total = (int)m_lines.size();
        if (total > rows)
        {
            const int bh = cv.h - HEAD - FOOT - 8, bx = cv.w - 5;
            fill(cv, bx, HEAD + 4, 3, bh, 40, 40, 60);
            const int th = std::max(10, bh * rows / total);
            const int ty = HEAD + 4 + (bh - th) * m_scroll / std::max(1, total - rows);
            fill(cv, bx, ty, 3, th, 150, 150, 200);
        }
        fill(cv, 0, cv.h - FOOT, cv.w, FOOT, 24, 24, 38);
        text(cv, 8, cv.h - FOOT + 2, m_footer.empty() ? "Stick: scroll   D-Pad: section   B: close" : m_footer.c_str(), C_DIM);
        m_dirty = false;
    }

    void draw_bottom(Canvas &cv)
    {
        using namespace detail;
        static const uint8_t C_TXT[3] = { 235, 235, 240 }, C_SEL[3] = { 255, 240, 160 };
        fill(cv, 0, 0, cv.w, cv.h, 18, 18, 28);
        build_buttons(cv.w, cv.h);
        if (m_show_title) text(cv, 8, 4, m_title, C_SEL, true);
        for (size_t i = 0; i < m_buttons.size(); ++i)
        {
            const Btn &b = m_buttons[i];
            const bool close = b.section == -2, tog = b.section == -3;
            const bool barSel = m_bar_focus && ((tog && m_bar_focus == 1) || (close && (m_toggle.empty() || m_bar_focus == 2)));
            const bool sel = m_bar_focus ? barSel : b.section == m_sec;
            const bool down = m_press_frames > 0 && b.section == m_press_btn;
            const bool cur = b.section == m_sec;   // the section being read keeps its colour
            fill(cv, b.x, b.y, b.w, b.h, close ? 120 : cur ? 70 : 40, close ? 40 : cur ? 70 : 42,
                 close ? 40 : cur ? 120 : 62);
            if (down)
            {
                // pushed in: brighter face, dark inner edge at top/left
                fill(cv, b.x, b.y, b.w, b.h, close ? 150 : 95, close ? 60 : 95, close ? 60 : 150);
                fill(cv, b.x, b.y, b.w, 2, 20, 20, 30);
                fill(cv, b.x, b.y, 2, b.h, 20, 20, 30);
            }
            if (sel)
            {
                fill(cv, b.x, b.y, b.w, 1, 250, 230, 120);
                fill(cv, b.x, b.y + b.h - 1, b.w, 1, 250, 230, 120);
            }
            const std::string &t = close ? std::string("Close") : tog ? m_toggle : m_sections[b.section].title;
            const int tw = std::min(ulen(t) * 6, b.w - 8);
            const int push = down ? 1 : 0;
            text(cv, b.x + (b.w - tw) / 2 + push, b.y + (b.h - 13) / 2 + push, t, sel ? C_SEL : C_TXT, sel || close, b.x + b.w - 4 + push);
        }
    }

private:
    struct Section { std::string title; std::vector<std::string> raw; };
    struct Btn { int x, y, w, h, section; };   // section -2 = Close

    static const int HEAD = 22, FOOT = 16, LINE_H = 14, MARGIN = 8, TABLE_COL = 132;

    std::string m_title;
    std::vector<Section> m_sections;
    std::vector<detail::Line> m_lines;
    std::vector<Btn> m_buttons;
    int m_sec = 0, m_scroll = 0, m_layout_sec = -1, m_layout_w = 0, m_cpad_acc = 0;
    bool m_active = false, m_dirty = true, m_click = false, m_toggle_hit = false;
    std::string m_toggle, m_footer;
    bool m_show_title = true;
    int m_bar_focus = 0;   // 0: sections; 1, 2: the bottom row (toggle, Close) has the D-Pad highlight
    int m_press_btn = -3, m_press_frames = 0;   // touch feedback

    int m_view_h = 240;          // height of the last top canvas drawn
    int body_rows() const { return (m_view_h - HEAD - FOOT - 6) / LINE_H; }
    int page() const { return body_rows() - 1; }

    void scroll(int d)
    {
        const int maxs = std::max(0, (int)m_lines.size() - body_rows());
        const int ns = std::max(0, std::min(maxs, m_scroll + d));
        if (ns != m_scroll) { m_scroll = ns; m_dirty = true; }
    }

    void parse(const std::string &text)
    {
        m_sections.clear();
        size_t p = 0;
        while (p <= text.size())
        {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            std::string line = text.substr(p, e - p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("# ", 0) == 0)
                m_sections.push_back({ line.substr(2), {} });
            else if (!m_sections.empty())
                m_sections.back().raw.push_back(line);
            p = e + 1;
        }
        m_sec = 0; m_scroll = 0; m_layout_sec = -1;
    }

    // Word-wrap `s` to `cols` characters.
    static std::vector<std::string> wrap(const std::string &s, int cols)
    {
        std::vector<std::string> out;
        std::string cur;
        size_t i = 0;
        while (i < s.size())
        {
            size_t j = s.find(' ', i);
            if (j == std::string::npos) j = s.size();
            std::string w = s.substr(i, j - i);
            if (!cur.empty() && detail::ulen(cur) + 1 + detail::ulen(w) > cols)
            {
                out.push_back(cur);
                cur.clear();
            }
            if (!cur.empty()) cur += ' ';
            cur += w;
            i = j + 1;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    void layout(int width)
    {
        if (m_layout_sec == m_sec && m_layout_w == width)
            return;
        using detail::Line;
        m_lines.clear();
        const int cols = (width - 2 * MARGIN - 6) / 6;
        std::string para;
        auto flush = [&]() {
            if (para.empty()) return;
            for (auto &l : wrap(para, cols)) m_lines.push_back({ Line::PARA, l, "", false, "" });
            para.clear();
        };
        for (const std::string &raw : m_sections[m_sec].raw)
        {
            if (raw.empty())
            {
                flush();
                if (!m_lines.empty() && m_lines.back().kind != Line::BLANK)
                    m_lines.push_back({ Line::BLANK, "", "", false, "" });
                continue;
            }
            if (raw.rfind("## ", 0) == 0)
            {
                flush();
                if (!m_lines.empty() && m_lines.back().kind != Line::BLANK)
                    m_lines.push_back({ Line::BLANK, "", "", false, "" });
                m_lines.push_back({ Line::HEAD, raw.substr(3), "", false, "" });
                continue;
            }
            if (raw.rfind("- ", 0) == 0)
            {
                flush();
                bool first = true;
                for (auto &l : wrap(raw.substr(2), cols - 3))
                    m_lines.push_back({ Line::BULLET, l, "", first, "" }), first = false;
                continue;
            }
            const size_t dot = raw.find(". ");
            if (dot != std::string::npos && dot > 0 && dot < 3 && isdigit((unsigned char)raw[0]))
            {
                flush();
                bool first = true;
                for (auto &l : wrap(raw.substr(dot + 2), cols - 4))
                    m_lines.push_back({ Line::STEP, l, "", first, raw.substr(0, dot + 1) }), first = false;
                continue;
            }
            if (raw.rfind("|", 0) == 0)
            {
                flush();
                std::string a, b;
                const size_t bar2 = raw.find('|', 1);
                a = raw.substr(1, bar2 == std::string::npos ? std::string::npos : bar2 - 1);
                if (bar2 != std::string::npos) b = raw.substr(bar2 + 1);
                if (!b.empty() && b.back() == '|') b.pop_back();
                auto trim = [](std::string &s) {
                    while (!s.empty() && s[0] == ' ') s.erase(0, 1);
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                };
                trim(a); trim(b);
                const int bcols = (width - 2 * MARGIN - TABLE_COL - 6) / 6;
                bool first = true;
                for (auto &l : wrap(b.empty() ? std::string(" ") : b, bcols))
                    m_lines.push_back({ Line::TABLE, first ? a : "", l, first, "" }), first = false;
                continue;
            }
            if (!para.empty()) para += ' ';
            para += raw;
        }
        flush();
        while (!m_lines.empty() && m_lines.back().kind == Line::BLANK)
            m_lines.pop_back();
        m_layout_sec = m_sec;
        m_layout_w = width;
        scroll(0);
    }

    // UI rule 11: targets at least 24 px tall, at least 4 px apart, at least 6 px from the screen
    // edges. When the section grid needs the room, the bottom-screen title is left off (the top
    // screen already names the section).
    void build_buttons(int w, int h)
    {
        m_buttons.clear();
        const int n = (int)m_sections.size();
        const int cols = n > 6 ? 2 : 1;
        const int rows = (n + cols - 1) / cols;
        const int edge = 6, gap = 4, minh = 24, maxh = 34, barh = 26;
        const int bar_y = h - edge - 4 - barh;          // Close / toggle row
        const int bottom = bar_y - gap - 2;             // the grid ends above it
        int top = 22;
        m_show_title = true;
        if (rows * minh + (rows - 1) * gap > bottom - top) { top = edge; m_show_title = false; }
        const int bh = std::max(minh, std::min(maxh, (bottom - top + gap) / std::max(1, rows) - gap));
        const int bw = (w - 2 * edge - (cols - 1) * gap) / cols;
        for (int i = 0; i < n; ++i)
        {
            const int c = i / rows, r = i % rows;
            m_buttons.push_back({ edge + c * (bw + gap), top + r * (bh + gap), bw, bh, i });
        }
        if (m_toggle.empty())
            m_buttons.push_back({ w / 2 - 60, bar_y, 120, barh, -2 });
        else
        {
            const int cw = 90;
            m_buttons.push_back({ edge, bar_y, w - 2 * edge - gap - cw, barh, -3 });   // toggle, left
            m_buttons.push_back({ w - edge - cw, bar_y, cw, barh, -2 });               // Close, right
        }
    }

    int button_at(int x, int y) const
    {
        for (const Btn &b : m_buttons)
            if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h)
                return b.section;
        return -1;
    }
};

#ifdef MANUAL3DS_HID
// Input from the 3DS HID service, mapped to the standard controls above.
// Call after hidScanInput(). touch_dy shifts the touch y (for games whose
// bottom image is offset, e.g. a 200-line surface centred on 240).
inline Input input_from_hid(int touch_dy = 0)
{
    const u32 down = hidKeysDown(), rep = hidKeysDownRepeat();
    Input in;
    static const struct { u32 b; uint32_t k; bool rep; } map[] = {
        { KEY_DUP, K_UP, true }, { KEY_DDOWN, K_DOWN, true },
        { KEY_CSTICK_UP, K_SCROLL_UP, true }, { KEY_CSTICK_DOWN, K_SCROLL_DOWN, true },
        { KEY_DLEFT, K_LEFT, true }, { KEY_DRIGHT, K_RIGHT, true },
        { KEY_L, K_L, false }, { KEY_R, K_R, false },
        { KEY_A, K_A, false }, { KEY_B, K_B, false }, { KEY_START, K_START, false },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); ++i)
        if ((map[i].rep ? rep : down) & map[i].b)
            in.down |= map[i].k;
    if (down & KEY_TOUCH)
    {
        touchPosition tp;
        hidTouchRead(&tp);
        in.touch_down = true;
        in.tx = tp.px;
        in.ty = tp.py - touch_dy;
    }
    circlePosition cp;
    hidCircleRead(&cp);
    in.cpad_y = cp.dy;
    return in;
}
#endif

} // namespace manual3ds
