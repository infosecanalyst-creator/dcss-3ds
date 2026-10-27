# DCSS 3DS — Dungeon Crawl Stone Soup for the New 3DS

A console-mode port of DCSS (current master, 0.34-era) to the Nintendo 3DS,
done as a small platform layer on top of the upstream code rather than a fork.

**Status: runs on a New 3DS XL** (databases, character creation, play).
The tiles frontend below is verified in a desktop harness (`tools/harness/`
in spirit: the real Crawl linked on Linux against a fake libctru, scripted
buttons/stylus, framebuffers dumped to images) before each hardware build.

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

Crawl is built with `USE_TILE_WEB`, which makes it compute Crawl's own tile
for every map square; the frontend reads those straight out of the view
buffer (no websocket) and draws them from a prebuilt 16 px atlas of the
official tile art (`source/3ds/tiles16.bin`, made by `tools/gen_atlas.py`
from crawl's `rltiles` sheets).

- **Bottom (touch) screen — the game.** The dungeon as tiles, 20×15
  squares centred on you, so your whole line of sight fits. Tap a square to
  travel there or attack what's on it; tap yourself to take the stairs,
  pick up, or wait. Translucent button columns sit at the two edges
  (outside line of sight): explore, autofight, rest, wait, pick up, stairs
  up/down | items, cast, abilities, level map, character, messages,
  keyboard. Yes/no questions get big Yes/No buttons; `--more--` continues
  on any tap.
- **Top screen — the dashboard.** Crawl's status panel (name, HP/MP,
  stats, status lights, weapon/quiver) in a 6×13 font; the monsters in
  view with their tiles; a level minimap with the touch map's window; the
  message pane in 5×8.
- **Menus** (inventory, spells, character creation, help…) are detected
  automatically: the text appears on the top screen in 5×8, and the bottom
  screen turns every menu entry into a touch row (one to three columns),
  with Esc / Enter / Page up / Page down / Keys buttons. Text prompts open
  the touch keyboard by themselves.

## Controls

| Input | In the dungeon | In menus |
|---|---|---|
| Stylus | travel / attack / use stairs | pick an entry |
| D-pad / Circle pad | move (8-way on the pad) | move |
| A | autofight (Yes on y/n, continue on --more--) | Enter |
| B | Esc (No on y/n) | Esc |
| X | autoexplore | `?` help |
| Y | pick up | next page |
| L / R | wait a turn / rest | page up / down |
| ZL / ZR | inventory / cast | |
| Select | touch keyboard | touch keyboard |
| HOME | crawl saves and exits | |

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
