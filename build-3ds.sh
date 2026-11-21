#!/usr/bin/env bash
# One-shot: fetch crawl (if needed), apply the 3DS port, build crawl.3dsx.
#
#   ./build-3ds.sh            # build (clones crawl into ./crawl on first run)
#   ./build-3ds.sh clean      # clean the crawl tree's 3DS outputs
#   CRAWL_REF=0.34.1 ./build-3ds.sh   # build a specific tag or commit instead
#
# CRAWL_REF defaults to the crawl commit that 3ds/tiles16.bin was built from.
# The atlas is indexed by crawl's tile numbers, so a newer crawl that adds tiles
# shifts every number; the game then rejects the atlas and draws glyphs. To move
# to a newer crawl, rebuild the atlas from it (tools/gen_atlas.py) and update
# both together.
#
# Needs DEVKITPRO set (the Docker image sets it). Run from the dcss-3ds folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
# Port version (this repo, not crawl) for the START menu; taken before anything is built.
PORT_VERSION="$(git -C "$HERE" describe --always --dirty 2>/dev/null || echo unknown)"
CRAWL_DIR="${CRAWL_DIR:-$HERE/crawl}"
CRAWL_REF="${CRAWL_REF:-07a8d25743754f847368dc1d0950c7dd6d4aff9a}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ -z "${DEVKITPRO:-}" ]; then
    echo "DEVKITPRO is not set (expected e.g. /opt/devkitpro)." >&2
    exit 1
fi
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

if [ ! -d "$CRAWL_DIR/.git" ]; then
    echo "==> cloning crawl ($CRAWL_REF) into $CRAWL_DIR"
    git init -q "$CRAWL_DIR"
    git -C "$CRAWL_DIR" remote add origin https://github.com/crawl/crawl.git
    git -C "$CRAWL_DIR" fetch -q --depth 1 origin "$CRAWL_REF"
    git -C "$CRAWL_DIR" checkout -q FETCH_HEAD
fi
cd "$CRAWL_DIR"
echo "==> contrib submodules (lua, pcre, sqlite, zlib)"
git submodule update --init --depth 1 \
    crawl-ref/source/contrib/lua crawl-ref/source/contrib/pcre \
    crawl-ref/source/contrib/sqlite crawl-ref/source/contrib/zlib

SRC="$CRAWL_DIR/crawl-ref/source"
if [ "${1:-}" = "clean" ]; then
    make -C "$SRC" TARGET_3DS=y clean clean-3ds || true
    exit 0
fi

echo "==> applying the 3DS port"
python3 "$HERE/tools/patch_crawl.py" "$SRC"

# The map's tile art: tiles16.bin, cut from crawl's own (CC0) tile sheets.
# Not kept in git; a local copy in source/3ds is used when present (copied in
# by patch_crawl.py), otherwise it is made here from this crawl checkout so
# its tile numbers always match the build.
if [ ! -f "$SRC/3ds/tiles16.bin" ]; then
    echo "==> making the tile atlas (3ds/tiles16.bin) from crawl's rltiles"
    ( cd "$SRC" && perl util/art-data.pl )      # writes rltiles/dc-unrand.txt
    RLT="$(mktemp -d)/rltiles"
    cp -r "$SRC/rltiles" "$RLT"
    env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS \
        make -C "$RLT" -j"$JOBS" TILES=y CC=gcc CXX=g++ STDFLAG=-std=c++17 > "$HERE/atlas-build.log" 2>&1 \
        || { tail -30 "$HERE/atlas-build.log"; echo "tile sheets failed; the map will use glyphs"; }
    if [ -f "$RLT/main.png" ]; then
        mkdir -p "$SRC/3ds"
        python3 "$HERE/tools/gen_atlas.py" "$RLT" "$SRC/3ds/tiles16.bin" 16
        ls -la "$SRC/3ds/tiles16.bin"
    fi
fi

# DC-01: databases + level cache built here, shipped in romfs:/cache, so the first
# start on the console does not build them for minutes (tools/gen_caches.sh).
# CACHES=0 skips this (the console then builds them itself, as before);
# CACHES=require (CI) fails the build when they could not be made.
CACHES="${CACHES:-auto}"
rm -rf "$SRC/3ds/cache"
if [ "$CACHES" != 0 ]; then
    if bash "$HERE/tools/gen_caches.sh" "$CRAWL_DIR"; then
        :
    elif [ "$CACHES" = require ]; then
        echo "prebuilt caches are required (CACHES=require) and could not be made" >&2
        exit 1
    else
        echo "==> no prebuilt caches: the first start on the console will build them"
        rm -rf "$SRC/3ds/cache"
    fi
fi

echo "==> building crawl.3dsx with $JOBS jobs"
make -C "$SRC" TARGET_3DS=y PORT_VERSION="$PORT_VERSION" -j"$JOBS" 3dsx 2>&1 | tee "$HERE/build-3ds.log"

if [ -d "$SRC/romfs/cache" ]; then
    echo "==> prebuilt caches shipped: $(du -sh "$SRC/romfs/cache" | cut -f1) of $(du -sh "$SRC/crawl.3dsx" | cut -f1) (.3dsx)"
elif [ "$CACHES" = require ]; then
    echo "prebuilt caches were made but not packed into romfs (data stamp differs?)" >&2
    exit 1
fi
OUT="$HERE/out"
mkdir -p "$OUT/3ds"
# The .3dsx lives directly in /3ds on the SD card (/3ds/crawl.3dsx); saves and
# settings stay in the game folder /3ds/crawl/, which the game creates itself.
rm -rf "$OUT/3ds/crawl"
cp "$SRC/crawl.3dsx" "$OUT/3ds/"
# Symbols for reading crash addresses: demangled nm map, plus the gzipped ELF (addr2line)
# when it is small enough for the builds branch.
ELF="$SRC/crawl"; [ -f "$SRC/crawl.elf" ] && ELF="$SRC/crawl.elf"
arm-none-eabi-nm -C -n "$ELF" | gzip -9 > "$OUT/crawl.sym.gz"
gzip -9c "$ELF" > "$OUT/crawl.elf.gz"
[ "$(stat -c%s "$OUT/crawl.elf.gz")" -lt 50000000 ] || rm -f "$OUT/crawl.elf.gz"
echo "==> port version $PORT_VERSION"
echo
echo "==> done: $OUT/3ds/crawl.3dsx"
echo "    copy it to sdmc:/3ds/crawl.3dsx (FTP via ftpd) and launch from the Homebrew Launcher"
