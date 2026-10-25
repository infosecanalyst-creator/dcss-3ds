# DCSS 3DS — Dungeon Crawl Stone Soup for the New 3DS

A console-mode port of DCSS (current master, 0.34-era) to the Nintendo 3DS,
done as a small platform layer on top of the upstream code rather than a fork.

**Status: dual-screen layout working and verified on desktop; untested on
hardware.** The real Crawl binary was linked against this backend on Linux
(libctru swapped for a stub, `tools/headless/`), driven by scripted input and
screenshotted: new game, autoexplore, the level map, inventory, the touch
keyboard and the stereoscopic pair all behave (`docs/preview-*.png`). The
first ARM build happens on your machine; expect a round of newlib link
complaints, not logic bugs.

## What's here

| Path | What |
|---|---|
| `source/lib3ds.cc` | The whole port: crawl's ~30-function console interface on libctru, plus the dual-screen router (below). |
| `tools/headless/` | Stub `3ds.h` + build script: links the real Crawl against this backend on Linux for scripted, screenshot-verified testing. |
| `source/lib3ds.h` | Early-init hook called from `main()` (romfs, save dirs, debug console). |
| `source/font5x10.h` | Generated 5×10 bitmap font, 501 glyphs (ASCII, Latin-1, box drawing, crawl's unicode glyph set). Regenerate with `tools/gen_font.py`. |
| `source/Makefile.3ds-block` / `-rules` | Cross-build section inserted into crawl's Makefile, plus `3dsx` / `cia` / `romfs` targets. |
| `source/3ds/crawl.rsf` | makerom spec for the `.cia` — requests the New 3DS **124 MB extended memory** mode. |
| `tools/patch_crawl.py` | Applies the port to a crawl checkout (6 small edits in upstream files + copies the new files). Idempotent; stops with a message if an upstream anchor moved. |
| `build-3ds.sh`, `Dockerfile` | One-shot build. |

## Build (recommended: Docker)

The simplest route on Windows is Docker Desktop with the official devkitPro
image — it avoids wrestling crawl's perl/python/flex build steps into msys2.

```powershell
cd dcss-3ds
docker build -t dcss-3ds .
docker run --rm -v "${PWD}:/work" dcss-3ds ./build-3ds.sh
```

First run clones crawl into `dcss-3ds/crawl`, pulls the four contrib libs
(Lua, PCRE, SQLite, zlib — all built for ARM), patches, and builds
`out/3ds/crawl/crawl.3dsx`. Copy `out/3ds` onto the SD card root and launch
from the Homebrew Launcher.

### Native devkitPro (what the SimGolf setup installed)

Works too, inside devkitPro's msys2 shell: `pacman -S make git perl python
flex bison pkg-config` (plus a host `gcc`/`libpng-devel` for crawl's tile
table generator), then `DEVKITPRO=/opt/devkitpro ./build-3ds.sh`.

### `.cia` (for the full 124 MB on the New 3DS)

`make -C crawl/crawl-ref/source TARGET_3DS=y cia` needs `makerom` from
[Project_CTR](https://github.com/3DSGuy/Project_CTR) on the PATH (the
Dockerfile has a commented-out line that builds it). Install with FBI.

## Memory — the real risk

Measured on 64-bit Linux: a fresh game plus 60 autoexplore steps and
several stair trips peaks at **89 MB RSS**, which includes the 20 MB
unstripped binary. A 32-bit ARM build with a stripped binary should land
around 55–70 MB. That is over the Old 3DS's 64 MB and inside the New 3DS's
124 MB — so this is a **New 3DS only** project, and running as a `.cia`
(or a `.3dsx` launched in a way that grants extended memory) matters.
If you see crashes on level generation, memory is the first suspect:
`osGetMemRegionFree(MEMREGION_APPLICATION)` printed from `lib3ds.cc` tells
you how close you are.

## Layout

Crawl still believes it has an 80×24 terminal; the backend routes each cell
by the region crawl assigns it (`crawl_view.viewp/hudp/msgp/mlistp`):

- **Top screen:** the dungeon viewport at 10×10 px per cell (the 5×10 font
  doubled in width) in the upper 170 px, drawn in three depth layers for the
  3D slider — floor glyphs sink behind the screen plane, walls/doors/items
  sit on it, monsters and `@` float in front (3 px parallax per layer at
  full slider; `kParallaxPx`). The 80-column message pane fills the bottom
  70 px at 5×10 — exactly 400 px wide.
- **Bottom screen:** the HUD and monster list at 5×10 in the left 210 px; a
  column of touch command buttons on the right (explore, fight, rest,
  search, pick up, inventory, map, travel, pray, cast, esc, enter, stairs,
  keyboard).
- **CRT mode:** any full-screen interface (main menu, character creation,
  inventory, the level map, help…) is detected automatically — text landing
  in the viewport that didn't come through `puttext()`, or a `puttext()`
  that isn't viewport-sized — and the plain 80×24 grid is shown on the top
  screen with the touch keyboard on the bottom. It switches back the moment
  the map is redrawn.

## Controls

| Input | Key sent | Plain / with L held |
|---|---|---|
| D-pad | arrows | move / **run** (Shift+arrow) |
| Circle pad | numpad 1–9 | 8-way move with auto-repeat |
| A / B | Enter / Esc | |
| X | `o` autoexplore | `G` travel |
| Y | Tab autofight | `p` pray |
| R | `5` rest | `s` wait a turn |
| ZL / ZR | `i` inventory / `g` pick up | `I` spells / `,` |
| Start | `S` save and quit | |
| Select | toggle the touch keyboard overlay | |
| Touch | command buttons (play) / full keyboard with sticky Shift+Ctrl (overlay, menus) | |
| HOME | treated as a hangup → crawl saves and exits | |

Edit `button_map[]` in `lib3ds.cc` to change any of this.

## Where things live on the 3DS

- Game data: baked into romfs (`romfs:/dat`, `romfs:/settings`, `romfs:/docs`)
- Saves, morgues, rc file: `sdmc:/3ds/crawl/` — put your `init.txt` there

## Testing without a 3DS

```bash
tools/headless/build-headless.sh crawl/crawl-ref/source /tmp/hl
DCSS3DS_SCRIPT="WAIT:5,TOUCH:17:107,WAIT:40,SLIDER:1,X,WAIT:60,TOUCH:214:86,WAIT:30,B" \
DCSS3DS_OUT=/tmp/frames DCSS3DS_EVERY=5 DCSS3DS_MAX=400 \
  /tmp/hl/crawl-3ds-headless -dir /tmp/hl/save -rc /tmp/hl/save/init.txt \
  -name T -species Mi -background Be
```

Script tokens: button names (`A B X Y L R ZL ZR UP DOWN LEFT RIGHT START
SELECT`), `TOUCH:x:y`, `WAIT:n`, `SLIDER:f`. Each token is consumed on one
input poll, and crawl polls during travel/explore too (that is how a key
interrupts them), so pad with `WAIT`s. Frames land as `top_NNNN.ppm`,
`topR_NNNN.ppm` (right eye) and `bottom_NNNN.ppm`.

## Ideas for later

- Tap-to-travel: a tap on a bottom-screen minimap could drive the level-map
  cursor (`X`, arrows, `.`) — crawl's console build has no mouse travel.
- Finer depth classes (e.g. items vs. walls) by colour as well as glyph.

## Known gaps / things to expect on first build

- `fsync`/`usleep`/`nl_langinfo`: handled (shims in `syscalls.cc`,
  `main.cc`). If the linker complains about another libc symbol, add a stub
  in the `TARGET_OS_3DS` block of `syscalls.cc`.
- `util/configure.py` probes the compiler; it should just work with
  `arm-none-eabi-g++`, but if `config.h` comes out wrong, that is where to look.
- Crawl's `-Werror`-style warnings are off for the port files; upstream
  warnings on ARM may need `CFWARN_L += -Wno-...` in the Makefile block.
- No sound, no networking, no webtiles — console mode only, by design.
