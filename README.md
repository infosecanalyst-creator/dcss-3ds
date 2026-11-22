# DCSS 3DS — Dungeon Crawl Stone Soup for the New 3DS

A console-mode port of DCSS (current master, 0.34-era) to the Nintendo 3DS,
done as a small platform layer on top of the upstream code rather than a fork.

DCSS is free software, so no game files need to be supplied (there is nothing to bring): the build
fetches the upstream source and data itself.

Install: `crawl.3dsx` goes directly in `sdmc:/3ds/` (as `sdmc:/3ds/crawl.3dsx`); the game makes `sdmc:/3ds/crawl/` for saves and settings. Logs there (`boot.log`, `sound.log`, `home.log`, `frame.log`) hold only failures and the exit line unless an empty `sdmc:/3ds/crawl/debug.txt` exists; a log past 1 MB starts over. The databases and level cache come prebuilt, so the first start takes seconds; if the game ever builds them itself ("Regenerating" lines), HOME works meanwhile.

*Not affiliated with or endorsed by the DCSS team or Nintendo.*

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
`out/3ds/crawl.3dsx`. Copy it to `sdmc:/3ds/crawl.3dsx` (directly in `/3ds`, not
in the game folder; FTP it with ftpd) and launch from the Homebrew Launcher.
The game creates its own folder `sdmc:/3ds/crawl/` for saves and settings.

### Native devkitPro

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
  view with their tiles; a map of the explored level at 2 px a square with the touch map's window; the
  message pane in 5×8.
- **Menus** (inventory, spells, character creation, help…) are detected
  automatically: the text appears on the top screen in 5×8, and the bottom
  screen turns every menu entry into a touch row (one to three columns; a
  list too long for one screen is paged with More / Back),
  with Esc / Enter / Page up / Page down / Keys buttons. Text prompts open
  the touch keyboard by themselves.

## Controls

| Input | In the dungeon | In menus |
|---|---|---|
| Stylus | travel / attack / use stairs | pick an entry |
| D-pad / Circle pad | move (8-way on the pad) | move |
| A | autofight (Yes on y/n, continue on --more--) | Enter |
| B | cancel (No on y/n); stops explore, travel and rest; closes the touch keyboard | Esc (does nothing on the main menu: leave with START > EXIT GAME) |
| X | autoexplore (keeps going if pressed again) | |
| Y | pick up | next page |
| L / R | zoom the map out / in (nothing while the touch keyboard is open) | page up / down (the port's own pages of buttons first) |
| ZL | swap the screens (the stylus is off while swapped) | |
| ZR | cast | |
| C-Stick up | inventory | |
| Select | touch keyboard | touch keyboard |
| START | port menu: resume, manual, keyboard, brightness, exit (asks first) | the same |
| HOME | crawl saves and exits | |

## Where things live on the 3DS

- Game data: baked into romfs (`romfs:/dat`, `romfs:/settings`, `romfs:/docs`)
- Text databases and level cache: prebuilt in `romfs:/cache/db` and `romfs:/cache/des` (see below); `sdmc:/3ds/crawl/saves/db` and `saves/des` are only written if the game ever has to build them itself
- Saves, morgues, rc file: `sdmc:/3ds/crawl/` — put your `init.txt` there
- Logs: `stdout.txt` (errors), `boot.log` (first line of each launch, failures); an empty `sdmc:/3ds/crawl/debug.txt` adds every boot step, `sound.log` plays/loads and `home.log` (each log starts over past 1 MB)

## Prebuilt databases and level cache

Crawl compiles its description text into SQLite databases and its 146 level
design files into a cache (index + Lua bytecode) the first time it runs. On
the console that took minutes, so the build does it instead:

- `tools/gen_caches.sh` (run by `build-3ds.sh`) builds this same code as a
  32-bit little-endian desktop program (`HARNESS_3DS` + `HARNESS_M32`, fake
  libctru from `tools/harness/`), runs it with `-builddb`, and checks the
  result with the same readers the 3DS uses: files current on a second run;
  loaded straight from `romfs:/cache` with an empty SD card and nothing
  written; refused and rebuilt on the SD card when the data stamp differs.
- "Current" is decided by a data stamp, not by file times: `romfs:/stamp.txt`
  is a checksum of everything in `dat/clua dlua des descript database`, and
  every romfs file reports it as its modification time. Each database and
  cache file records the stamp it was built from; a file with another stamp
  (or another save version, word size, or Lua bytecode layout —
  `cache/des/luachk.bin`) is ignored and Crawl builds its own copy under
  `sdmc:/3ds/crawl/saves/`, showing its "Regenerating ..." lines as before.
- Needs a host compiler that can build 32-bit programs (`g++-multilib`; in
  the Dockerfile). Without one the build still works and the console builds
  the caches itself; `CACHES=0` skips the step, `CACHES=require` (CI) fails
  the build instead.

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
- No networking, no webtiles — console mode only, by design.

## Licence

Dungeon Crawl Stone Soup is copyright its contributors (the DCSS devteam
and everyone credited in crawl's `CREDITS.txt`) and is licensed under the
GNU General Public License, version 2 or (at your option) any later
version. This port's own files are distributed under the same terms:
**GPL-2.0-or-later**. The full GPL v2 text is in [`LICENSE`](LICENSE).

The tile art the build bakes into `tiles16.bin` comes from crawl's
`rltiles`, which upstream releases as CC0. The sound effects are CC0;
every clip's source is listed in [`SOUND-CREDITS.txt`](SOUND-CREDITS.txt).
