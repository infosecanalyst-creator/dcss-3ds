// Headless stand-in for libctru's <3ds.h>: enough API for lib3ds.cc to link
// on a desktop, with scripted input and PPM frame dumps.
//
// Script (env DCSS3DS_SCRIPT): comma-separated tokens
//   A B X Y L R ZL ZR UP DOWN LEFT RIGHT START SELECT   button taps
//   TOUCH:x:y        stylus tap at bottom-screen pixel
//   KEY:c            raw key char via the touch keyboard path is not
//                    needed; use TOUCH instead
//   WAIT:n           idle n frames
//   SLIDER:f         set the 3D slider (0..1)
// Frames are written to $DCSS3DS_OUT every $DCSS3DS_EVERY vblanks.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result;
#define R_FAILED(r) ((r) < 0)
#define BIT(n) (1U << (n))

enum { KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
       KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
       KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
       KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20),
       KEY_CPAD_RIGHT = BIT(28), KEY_CPAD_LEFT = BIT(29), KEY_CPAD_UP = BIT(30), KEY_CPAD_DOWN = BIT(31),
       KEY_UP = KEY_DUP | KEY_CPAD_UP, KEY_DOWN = KEY_DDOWN | KEY_CPAD_DOWN,
       KEY_LEFT = KEY_DLEFT | KEY_CPAD_LEFT, KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT };

typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef enum { GSP_RGBA8_OES = 0, GSP_BGR8_OES = 1 } GSPGPU_FramebufferFormat;
typedef struct { u16 px, py; } touchPosition;
typedef struct { s16 dx, dy; } circlePosition;
typedef struct { int dummy; } PrintConsole;

namespace h3ds {
struct State {
    u8 top[2][400 * 240 * 3];
    u8 bottom[320 * 240 * 3];
    u32 down = 0, held = 0, up = 0, rep = 0;
    touchPosition touch = {0, 0};
    circlePosition cpad = {0, 0};
    float slider = 0.f;
    u64 ms = 0;
    int vblanks = 0, frames = 0, every = 1, maxVblanks = 2000;
    char *script = nullptr, *tok = nullptr;
    int wait = 0;
    const char *out = ".";
    bool inited = false;
};
inline State &st() { static State s; return s; }

inline u32 button(const char *t) {
    static const struct { const char *n; u32 b; } m[] = {
        {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y}, {"L", KEY_L}, {"R", KEY_R},
        {"ZL", KEY_ZL}, {"ZR", KEY_ZR}, {"UP", KEY_DUP}, {"DOWN", KEY_DDOWN}, {"LEFT", KEY_DLEFT},
        {"RIGHT", KEY_DRIGHT}, {"START", KEY_START}, {"SELECT", KEY_SELECT},
    };
    for (auto &e : m) if (!strcmp(e.n, t)) return e.b;
    return 0;
}

inline void initScript() {
    State &s = st();
    if (s.inited) return;
    s.inited = true;
    const char *scr = getenv("DCSS3DS_SCRIPT");
    s.script = strdup(scr ? scr : "");
    s.tok = strtok(s.script, ",");
    const char *o = getenv("DCSS3DS_OUT"); if (o) s.out = o;
    const char *e = getenv("DCSS3DS_EVERY"); if (e) s.every = atoi(e);
    const char *m = getenv("DCSS3DS_MAX"); if (m) s.maxVblanks = atoi(m);
}

inline void writePPM(const char *path, const u8 *bgr, int w, int h) {
    // framebuffers are portrait-rotated BGR; un-rotate to landscape RGB
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const u8 *p = bgr + 3 * (x * h + (h - 1 - y));
            fputc(p[2], f); fputc(p[1], f); fputc(p[0], f);
        }
    fclose(f);
}

inline void dump() {
    State &s = st();
    if (s.vblanks % s.every) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/top_%04d.ppm", s.out, s.frames);
    writePPM(path, s.top[0], 400, 240);
    snprintf(path, sizeof(path), "%s/topR_%04d.ppm", s.out, s.frames);
    writePPM(path, s.top[1], 400, 240);
    snprintf(path, sizeof(path), "%s/bottom_%04d.ppm", s.out, s.frames);
    writePPM(path, s.bottom, 320, 240);
    ++s.frames;
}
}

// ---- gfx
inline void gfxInitDefault() { h3ds::initScript(); }
inline void gfxExit() {}
inline void gfxSetScreenFormat(gfxScreen_t, GSPGPU_FramebufferFormat) {}
inline void gfxSetDoubleBuffering(gfxScreen_t, bool) {}
inline void gfxSet3D(bool) {}
inline void gfxFlushBuffers() {}
inline void gfxSwapBuffers() {}
inline u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *w, u16 *h) {
    if (w) *w = 240;
    if (h) *h = (screen == GFX_TOP) ? 400 : 320;
    return screen == GFX_TOP ? h3ds::st().top[side] : h3ds::st().bottom;
}
inline void gspWaitForVBlank() {
    h3ds::State &s = h3ds::st();
    s.ms += 16;
    h3ds::dump();
    if (++s.vblanks > s.maxVblanks + 200) { fprintf(stderr, "headless: hard exit\n"); exit(0); }
}
inline PrintConsole *consoleInit(gfxScreen_t, PrintConsole *) { return nullptr; }

// ---- hid: advance the script by one token per scan
inline void hidScanInput() {
    h3ds::State &s = h3ds::st();
    h3ds::initScript();
    s.up = s.down; // whatever was down last frame is released now
    s.down = s.held = s.rep = 0;
    if (s.wait > 0) { --s.wait; return; }
    if (!s.tok) return;
    const char *t = s.tok;
    if (!strncmp(t, "TOUCH:", 6)) {
        int x = 0, y = 0; sscanf(t + 6, "%d:%d", &x, &y);
        s.touch.px = (u16)x; s.touch.py = (u16)y;
        s.down = s.held = KEY_TOUCH;
    } else if (!strncmp(t, "WAIT:", 5)) {
        s.wait = atoi(t + 5);
    } else if (!strncmp(t, "SLIDER:", 7)) {
        s.slider = (float)atof(t + 7);
    } else {
        s.down = s.held = s.rep = h3ds::button(t);
    }
    s.tok = strtok(nullptr, ",");
}
inline u32 hidKeysDown() { return h3ds::st().down; }
inline u32 hidKeysHeld() { return h3ds::st().held; }
inline u32 hidKeysUp() { return h3ds::st().up; }
inline u32 hidKeysDownRepeat() { return h3ds::st().rep; }
inline void hidTouchRead(touchPosition *p) { *p = h3ds::st().touch; }
inline void hidCircleRead(circlePosition *p) { *p = h3ds::st().cpad; }
inline void hidSetRepeatParameters(u32, u32) {}

// ---- apt / os / svc / romfs
// Past the vblank budget, report "closing" so crawl saves and exits cleanly.
inline bool aptMainLoop() { return h3ds::st().vblanks <= h3ds::st().maxVblanks; }
inline u64 osGetTime() { return h3ds::st().ms; }
inline void osSetSpeedupEnable(bool) {}
inline float osGet3DSliderState() { return h3ds::st().slider; }
inline void svcSleepThread(s64 ns) { h3ds::st().ms += (u64)(ns / 1000000); }
inline Result romfsInit() { return 0; }
inline Result romfsExit() { return 0; }
