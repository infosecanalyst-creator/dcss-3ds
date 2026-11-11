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
CRAWL_DIR="${CRAWL_DIR:-$HERE/crawl}"
CRAWL_REF="${CRAWL_REF:-5e89c473da01f5603fbae87a11286a5d52c773e5}"
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

echo "==> building crawl.3dsx with $JOBS jobs"
make -C "$SRC" TARGET_3DS=y -j"$JOBS" 3dsx 2>&1 | tee "$HERE/build-3ds.log"

OUT="$HERE/out"
mkdir -p "$OUT/3ds/crawl"
cp "$SRC/crawl.3dsx" "$SRC/crawl.smdh" "$OUT/3ds/crawl/"
echo
echo "==> done: $OUT/3ds/crawl/crawl.3dsx"
echo "    copy the out/3ds folder to the root of the SD card and launch from the Homebrew Launcher"
