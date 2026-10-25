#!/usr/bin/env bash
# One-shot: fetch crawl (if needed), apply the 3DS port, build crawl.3dsx.
#
#   ./build-3ds.sh            # build (clones crawl into ./crawl on first run)
#   ./build-3ds.sh clean      # clean the crawl tree's 3DS outputs
#   CRAWL_REF=0.34.1 ./build-3ds.sh   # build a specific tag instead of master
#
# Needs DEVKITPRO set (the Docker image sets it). Run from the dcss-3ds folder.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CRAWL_DIR="${CRAWL_DIR:-$HERE/crawl}"
CRAWL_REF="${CRAWL_REF:-master}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [ -z "${DEVKITPRO:-}" ]; then
    echo "DEVKITPRO is not set (expected e.g. /opt/devkitpro)." >&2
    exit 1
fi
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

if [ ! -d "$CRAWL_DIR/.git" ]; then
    echo "==> cloning crawl ($CRAWL_REF) into $CRAWL_DIR"
    git clone --branch "$CRAWL_REF" --depth 1 https://github.com/crawl/crawl.git "$CRAWL_DIR"
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

echo "==> building crawl.3dsx with $JOBS jobs"
make -C "$SRC" TARGET_3DS=y -j"$JOBS" 3dsx 2>&1 | tee "$HERE/build-3ds.log"

OUT="$HERE/out"
mkdir -p "$OUT/3ds/crawl"
cp "$SRC/crawl.3dsx" "$SRC/crawl.smdh" "$OUT/3ds/crawl/"
echo
echo "==> done: $OUT/3ds/crawl/crawl.3dsx"
echo "    copy the out/3ds folder to the root of the SD card and launch from the Homebrew Launcher"
