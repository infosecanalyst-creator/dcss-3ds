// Fake libctru for the desktop harness: just enough of the API that the
// crawl 3DS frontend uses. Framebuffers are real memory; input comes from a
// script; frames can be dumped as PPM. See harness3ds.cc.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef s32 Result;
#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
#define BIT(n) (1U << (n))
enum {
    KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
    KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
    KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
    KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20),
    KEY_CSTICK_RIGHT = BIT(24), KEY_CSTICK_LEFT = BIT(25), KEY_CSTICK_UP = BIT(26), KEY_CSTICK_DOWN = BIT(27),
    KEY_CPAD_RIGHT = BIT(28), KEY_CPAD_LEFT = BIT(29), KEY_CPAD_UP = BIT(30), KEY_CPAD_DOWN = BIT(31),
    KEY_UP = KEY_DUP | KEY_CPAD_UP, KEY_DOWN = KEY_DDOWN | KEY_CPAD_DOWN,
    KEY_LEFT = KEY_DLEFT | KEY_CPAD_LEFT, KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT,
};
typedef struct { u16 px, py; } touchPosition;
typedef struct { s16 dx, dy; } circlePosition;
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef enum { GSP_RGBA8_OES = 0, GSP_BGR8_OES = 1, GSP_RGB565_OES = 2, GSP_RGB5_A1_OES = 3, GSP_RGBA4_OES = 4 } GSPGPU_FramebufferFormat;
typedef struct PrintConsole PrintConsole;

void gfxInitDefault(void);
void gfxExit(void);
u8 *gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16 *width, u16 *height);
void gfxFlushBuffers(void);
void gfxSwapBuffers(void);
void gfxSet3D(bool enable);
void gfxSetScreenFormat(gfxScreen_t screen, GSPGPU_FramebufferFormat fmt);
void gfxSetDoubleBuffering(gfxScreen_t screen, bool enable);
void gspWaitForVBlank(void);
PrintConsole *consoleInit(gfxScreen_t screen, PrintConsole *console);
void hidScanInput(void);
u32 hidKeysDown(void);
u32 hidKeysHeld(void);
u32 hidKeysUp(void);
u32 hidKeysDownRepeat(void);
void hidTouchRead(touchPosition *pos);
void hidCircleRead(circlePosition *pos);
void hidSetRepeatParameters(u32 delay, u32 interval);
float osGet3DSliderState(void);
void osSetSpeedupEnable(bool enable);
u64 osGetTime(void);
u64 svcGetSystemTick(void);
void svcSleepThread(s64 ns);
bool aptMainLoop(void);
Result romfsInit(void);
Result romfsExit(void);
Result psInit(void);
void psExit(void);
Result PS_GenerateRandomBytes(void *out, size_t len);
#define SYSCLOCK_ARM11 268111856ULL
#ifdef __cplusplus
}
#endif
