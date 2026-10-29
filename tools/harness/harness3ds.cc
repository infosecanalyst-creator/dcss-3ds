// Desktop harness for the crawl 3DS frontend: fake libctru.
//   HARNESS_SCRIPT=<file>  one command per line:
//     press A|B|X|Y|L|R|ZL|ZR|START|SELECT|UP|DOWN|LEFT|RIGHT [n]  (n taps)
//     hold KEY frames | touch x y [frames] | drag x1 y1 x2 y2 frames
//     wait frames | shot name | slider 0..1 | quit
//   HARNESS_OUT=<dir>      where shot_<name>_{top,bot}.ppm go
#include "3ds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <time.h>
#include <unistd.h>

static u8 fb_top_l[240 * 400 * 3], fb_top_r[240 * 400 * 3], fb_bot[240 * 320 * 3];
static u32 kdown, kheld, kup, kheld_prev;
static touchPosition tpos;
static float slider = 0.f;
static long frame_no = 0;
static bool quitting = false;
static int quit_grace = 0;

struct Step { std::string cmd; std::vector<std::string> a; };
static std::vector<Step> steps;
static size_t step_i = 0;
static int step_frames = 0;     // frames left in the current step
static bool loaded = false;

static u32 keybit(const std::string &k) {
    static const struct { const char *n; u32 b; } m[] = {
        {"A",KEY_A},{"B",KEY_B},{"X",KEY_X},{"Y",KEY_Y},{"L",KEY_L},{"R",KEY_R},{"ZL",KEY_ZL},{"ZR",KEY_ZR},
        {"START",KEY_START},{"SELECT",KEY_SELECT},{"UP",KEY_DUP},{"DOWN",KEY_DDOWN},{"LEFT",KEY_DLEFT},{"RIGHT",KEY_DRIGHT}};
    for (auto &e : m) if (k == e.n) return e.b;
    fprintf(stderr, "harness: unknown key %s\n", k.c_str());
    return 0;
}

static void load_script() {
    loaded = true;
    const char *p = getenv("HARNESS_SCRIPT");
    if (!p) return;
    FILE *f = fopen(p, "r");
    if (!f) { perror(p); return; }
    char line[512];
    while (fgets(line, sizeof line, f)) {
        Step s; char *save = nullptr;
        for (char *t = strtok_r(line, " \t\r\n", &save); t; t = strtok_r(nullptr, " \t\r\n", &save)) {
            if (s.cmd.empty()) { if (t[0] == '#') break; s.cmd = t; } else s.a.push_back(t);
        }
        if (!s.cmd.empty()) steps.push_back(s);
    }
    fclose(f);
}

static void write_ppm(const char *path, const u8 *fb, int W) {
    // fb is portrait: pixel (x,y) of a W x 240 landscape screen at (x*240 + 239-y)*3, BGR
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d 240\n255\n", W);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < W; ++x) {
            const u8 *p = fb + (x * 240 + 239 - y) * 3;
            u8 rgb[3] = { p[2], p[1], p[0] };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void shot(const std::string &name) {
    const char *out = getenv("HARNESS_OUT");
    std::string d = out ? out : ".";
    write_ppm((d + "/shot_" + name + "_top.ppm").c_str(), fb_top_l, 400);
    write_ppm((d + "/shot_" + name + "_topR.ppm").c_str(), fb_top_r, 400);
    write_ppm((d + "/shot_" + name + "_bot.ppm").c_str(), fb_bot, 320);
    fprintf(stderr, "harness: shot %s (frame %ld)\n", name.c_str(), frame_no);
}

// Advance the script by one frame: sets kheld / touch for this frame.
static void step_script() {
    if (!loaded) load_script();
    kheld = 0;
    for (;;) {
        if (step_i >= steps.size()) { if (!quitting) { quitting = true; quit_grace = 60; } return; }
        Step &s = steps[step_i];
        if (step_frames == 0) {
            // start of step
            if (s.cmd == "press") { int n = s.a.size() > 1 ? atoi(s.a[1].c_str()) : 1; step_frames = n * 6; }
            else if (s.cmd == "hold") step_frames = s.a.size() > 1 ? atoi(s.a[1].c_str()) : 1;
            else if (s.cmd == "touch") step_frames = (s.a.size() > 2 ? atoi(s.a[2].c_str()) : 3) + 3;
            else if (s.cmd == "drag") step_frames = atoi(s.a[4].c_str()) + 3;
            else if (s.cmd == "wait") step_frames = atoi(s.a[0].c_str());
            else if (s.cmd == "shot") { shot(s.a[0]); ++step_i; continue; }
            else if (s.cmd == "slider") { slider = atof(s.a[0].c_str()); ++step_i; continue; }
            else if (s.cmd == "quit") { quitting = true; quit_grace = 0; ++step_i; return; }
            else { fprintf(stderr, "harness: bad cmd %s\n", s.cmd.c_str()); ++step_i; continue; }
            s.a.push_back(std::to_string(step_frames));   // remember total in last slot
        }
        const int total = atoi(s.a.back().c_str());
        const int k = total - step_frames;                 // 0-based frame within step
        if (s.cmd == "press") { if (k % 6 < 2) kheld = keybit(s.a[0]); }
        else if (s.cmd == "hold") kheld = keybit(s.a[0]);
        else if (s.cmd == "touch") { if (k < total - 3) { kheld = KEY_TOUCH; tpos.px = atoi(s.a[0].c_str()); tpos.py = atoi(s.a[1].c_str()); } }
        else if (s.cmd == "drag") {
            const int n = total - 3;
            if (k < n) {
                const int x1 = atoi(s.a[0].c_str()), y1 = atoi(s.a[1].c_str()), x2 = atoi(s.a[2].c_str()), y2 = atoi(s.a[3].c_str());
                kheld = KEY_TOUCH; tpos.px = x1 + (x2 - x1) * k / (n > 1 ? n - 1 : 1); tpos.py = y1 + (y2 - y1) * k / (n > 1 ? n - 1 : 1);
            }
        }
        if (--step_frames == 0) { s.a.pop_back(); ++step_i; }
        return;
    }
}

extern "C" {
void gfxInitDefault(void) {}
void gfxExit(void) {}
u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height) {
    if (width) *width = 240;
    if (height) *height = screen == GFX_TOP ? 400 : 320;
    if (screen == GFX_BOTTOM) return fb_bot;
    return side == GFX_RIGHT ? fb_top_r : fb_top_l;
}
void gfxFlushBuffers(void) {}
void gfxSwapBuffers(void) {}
void gfxScreenSwapBuffers(gfxScreen_t, bool) {}
void gfxSet3D(bool) {}
void gfxSetScreenFormat(gfxScreen_t, GSPGPU_FramebufferFormat) {}
void gfxSetDoubleBuffering(gfxScreen_t, bool) {}
void gspWaitForVBlank(void) { ++frame_no; }
PrintConsole *consoleInit(gfxScreen_t, PrintConsole *c) { return c; }
void hidScanInput(void) {
    kheld_prev = kheld;
    step_script();
    kdown = kheld & ~kheld_prev;
    kup = kheld_prev & ~kheld;
}
u32 hidKeysDown(void) { return kdown; }
u32 hidKeysHeld(void) { return kheld; }
u32 hidKeysUp(void) { return kup; }
u32 hidKeysDownRepeat(void) { return kdown; }
void hidTouchRead(touchPosition *pos) { *pos = tpos; }
void hidCircleRead(circlePosition *pos) { pos->dx = pos->dy = 0; }
void hidSetRepeatParameters(u32, u32) {}
float osGet3DSliderState(void) { return slider; }
void osSetSpeedupEnable(bool) {}
u64 osGetTime(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000ULL + t.tv_nsec / 1000000; }
u64 svcGetSystemTick(void) { return osGetTime() * 268111ULL; }
void svcSleepThread(s64) {}
bool aptMainLoop(void) {
    if (quitting) { if (quit_grace-- <= 0) return false; }
    return true;
}
Result romfsInit(void) { return 0; }
Result romfsExit(void) { return 0; }
Result psInit(void) { return 0; }
void psExit(void) {}
Result PS_GenerateRandomBytes(void *out, size_t len) { u8 *o = (u8 *)out; for (size_t i = 0; i < len; ++i) o[i] = (u8)(i * 131 + 7); return 0; }
}
