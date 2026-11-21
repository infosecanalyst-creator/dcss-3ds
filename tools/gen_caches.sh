#!/usr/bin/env bash
# DC-01: build crawl's text databases and level-design cache ahead of time, so
# the 3DS does not spend its first start (minutes) building them.
#
#   tools/gen_caches.sh <crawl checkout, already patched by patch_crawl.py>
#
# Writes <crawl>/crawl-ref/source/3ds/cache/{stamp.txt,db/*.db,des/*}; the
# romfs rule (Makefile.3ds-rules) ships them as romfs:/cache/ when stamp.txt
# equals the stamp of the data it packs.
#
# How: the same patched crawl + lib3ds.cc is built as a desktop program against
# the fake libctru in tools/harness (HARNESS_3DS), as 32-bit little-endian with
# unsigned char like the 3DS (HARNESS_M32: -m32 -funsigned-char), and run with
# -builddb. The databases are SQLite files (same on every machine); the level
# cache is crawl's own save-file marshalling (fixed widths, little-endian) plus
# Lua 5.4 bytecode, whose layout depends only on what a 32-bit little-endian
# build shares with the 3DS (4-byte instructions, 8-byte integers and doubles).
# The 3DS checks that itself before using the files (luachk.bin, maps.cc).
#
# The files are then proven with the same program, i.e. the 3DS's own readers:
#   1. a second run finds the files it wrote current (nothing regenerated);
#   2. with the files only in romfs:/cache and an empty "SD card", a run loads
#      everything from romfs and writes nothing;
#   3. a new game then starts from those romfs files (its first level is built
#      from the level cache) and is saved;
#   4. with another data stamp the romfs copies are refused and the caches are
#      built on the "SD card" (the fallback the console keeps).
# Any failed step fails the script.
#
# Needs a host g++ that can build 32-bit programs (Debian/Ubuntu: g++-multilib).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CRAWL="$(cd "$1" && pwd)"
SRC="$CRAWL/crawl-ref/source"
GEN="${GEN_DIR:-$HERE/../cachegen}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
HOSTENV=(env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS -u AR)

echo 'int main(){return sizeof(long)==4?0:1;}' > /tmp/gen_caches_m32.c
if ! gcc -m32 /tmp/gen_caches_m32.c -o /tmp/gen_caches_m32 2>/dev/null || ! /tmp/gen_caches_m32; then
    echo "gen_caches: this machine's gcc cannot build 32-bit programs (install g++-multilib)" >&2
    exit 3
fi

G="$GEN/crawl-ref/source"
if [ -n "${GEN_REUSE:-}" ] && [ -x "$G/crawl-harness" ]; then
    echo "==> cache generator: reusing $GEN (GEN_REUSE)"
    rm -rf "$GEN"/run-* "$GEN/romfs-stale" "$G/3ds/cache"
else
    echo "==> cache generator: copying the patched crawl tree"
    rm -rf "$GEN"
    mkdir -p "$GEN"
    cp -a "$CRAWL/crawl-ref" "$GEN/crawl-ref"
    # a tree that already built for the 3DS: no ARM objects, no ARM contribs, no old romfs/cache
    find "$G" -type f \( -name '*.o' -o -name '*.d' -o -name '*.a' -o -name '*.cflags' -o -name '*.ldflags' \) -delete
    rm -rf "$G/contrib/install" "$G/romfs" "$G/3ds/cache" "$G/crawl-harness" "$G/crawl.elf"
fi

H="$GEN/harness"
mkdir -p "$H/fake"
cp "$HERE/harness/3ds.h" "$H/fake/3ds.h"
"${HOSTENV[@]}" g++ -m32 -O1 -I"$H/fake" -c "$HERE/harness/harness3ds.cc" -o "$H/harness3ds.o"
ar rcs "$H/libharness3ds.a" "$H/harness3ds.o"

echo "==> cache generator: building (32-bit desktop build of the 3DS code, $JOBS jobs)"
GMAKE=("${HOSTENV[@]}" make -C "$G" TARGET_3DS=y HARNESS_3DS="$H" HARNESS_M32=y NO_OPTIMIZE=y)
if ! "${GMAKE[@]}" -j"$JOBS" crawl-harness > "$GEN/build.log" 2>&1; then
    grep -n -B2 -A12 'error' "$GEN/build.log" | head -80 || true
    tail -30 "$GEN/build.log"
    echo "gen_caches: the generator did not build (log: $GEN/build.log)" >&2
    exit 1
fi
"${GMAKE[@]}" romfs > "$GEN/romfs.log" 2>&1 || { cat "$GEN/romfs.log"; exit 1; }
STAMP="$(cat "$G/romfs/stamp.txt")"

# run <name> <romfs dir>: one -builddb run with its own empty "SD card"
run() {
    local W="$GEN/run-$1"
    if [ ! -d "$W" ]; then
        mkdir -p "$W/sdmc:/3ds/crawl"
        ln -s "$2" "$W/romfs:"
    fi
    ( cd "$W" && "$G/crawl-harness" -builddb ) > "$W/out.$3.txt" 2>&1 \
        || { tail -20 "$W/out.$3.txt"; echo "gen_caches: run $1 ($3) failed" >&2; exit 1; }
    cat "$W/sdmc:/3ds/crawl/stdout.txt" >> "$W/out.$3.txt" 2>/dev/null || true
    REGEN="$(grep -c '^Regenerating' "$W/out.$3.txt" || true)"
    SD="$W/sdmc:/3ds/crawl/saves"
}
fail() { echo "gen_caches: $*" >&2; exit 1; }

echo "==> cache generator: building the databases and the level cache (stamp $STAMP)"
run gen "$G/romfs" 1
NDB="$(ls "$SD/db"/*.db | wc -l)"; NIDX="$(ls "$SD/des"/*.idx | wc -l)"; NDSC="$(ls "$SD/des"/*.dsc | wc -l)"
echo "    built: $REGEN files regenerated -> $NDB databases, $NIDX level index files"
[ "$NDB" -ge 11 ] && [ "$NIDX" -ge 50 ] && [ "$NIDX" = "$NDSC" ] && [ -s "$SD/des/luachk.bin" ] \
    || fail "the first run did not write the expected files"
[ "$REGEN" = "$((NDB + NIDX))" ] || fail "regenerated $REGEN files but found $NDB databases + $NIDX level files"

echo "==> check 1: the files are current for the program that wrote them"
run gen "$G/romfs" 2
[ "$REGEN" = 0 ] || fail "check 1: $REGEN files were rebuilt on the second run"

CACHE="$G/3ds/cache"
mkdir -p "$CACHE/db" "$CACHE/des"
cp "$SD/db"/*.db "$CACHE/db/"
cp "$SD/des"/*.idx "$SD/des"/*.dsc "$SD/des/luachk.bin" "$CACHE/des/"
cp "$SD/des"/*.lux "$CACHE/des/" 2>/dev/null || true
echo "$STAMP" > "$CACHE/stamp.txt"
"${GMAKE[@]}" romfs > "$GEN/romfs.log" 2>&1 || { cat "$GEN/romfs.log"; exit 1; }
grep 'prebuilt caches' "$GEN/romfs.log" || true
[ -f "$G/romfs/cache/des/luachk.bin" ] || fail "the romfs rule did not pack the caches"

echo "==> check 2: shipped in romfs, with an empty SD card: nothing is built or written"
run romfs "$G/romfs" 1
WROTE="$( { find "$SD/db" "$SD/des" -type f 2>/dev/null || true; } | { grep -v '\.lk$' || true; } | wc -l)"
[ "$REGEN" = 0 ] && [ "$WROTE" = 0 ] || fail "check 2: $REGEN files rebuilt, $WROTE cache files written to the SD card"

echo "==> check 3: a new game starts on the romfs caches (its first level is built from them)"
W="$GEN/run-romfs"
printf 'wait 400\nquit\n' > "$GEN/play.script"
( cd "$W" && HARNESS_SCRIPT="$GEN/play.script" HARNESS_OUT="$GEN" timeout 300 "$G/crawl-harness" \
      -name T -species Mi -background Cj -seed 7 ) > "$W/out.play.txt" 2>&1 \
    || { tail -20 "$W/out.play.txt"; fail "check 3: the game did not run"; }
WROTE="$( { find "$SD/db" "$SD/des" -type f 2>/dev/null || true; } | { grep -v '\.lk$' || true; } | wc -l)"
[ -s "$SD/T.cs" ] && [ "$WROTE" = 0 ] || fail "check 3: no saved game, or $WROTE cache files written to the SD card"

echo "==> check 4: other data (another stamp): the romfs copies are refused, the SD card gets its own"
cp -a "$G/romfs" "$GEN/romfs-stale"
echo "$(( (STAMP + 1) % 4294967295 ))" > "$GEN/romfs-stale/stamp.txt"
run stale "$GEN/romfs-stale" 1
SNDB="$(ls "$SD/db"/*.db 2>/dev/null | wc -l)"; SNIDX="$(ls "$SD/des"/*.idx 2>/dev/null | wc -l)"
[ "$REGEN" = "$((NDB + NIDX))" ] && [ "$SNDB" = "$NDB" ] && [ "$SNIDX" = "$NIDX" ] \
    || fail "check 4: stale caches: $REGEN rebuilt, $SNDB databases, $SNIDX level files (wanted $NDB + $NIDX)"
run stale "$GEN/romfs-stale" 2
[ "$REGEN" = 0 ] || fail "check 4: the SD card caches were rebuilt again ($REGEN)"

rm -rf "$SRC/3ds/cache"
mkdir -p "$SRC/3ds"
cp -r "$CACHE" "$SRC/3ds/cache"
echo "==> prebuilt caches: $(du -sh "$SRC/3ds/cache" | cut -f1) in $SRC/3ds/cache (stamp $STAMP), all checks passed"
if [ -z "${GEN_KEEP:-}" ]; then rm -rf "$GEN"; fi
