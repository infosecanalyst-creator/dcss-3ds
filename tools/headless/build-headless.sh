#!/usr/bin/env bash
# Build the real Crawl against the 3DS backend *on Linux*, with libctru
# replaced by tools/headless/3ds.h (scripted input, PPM frame dumps).
# This is how the dual-screen layout was verified without hardware.
#
#   ./build-headless.sh <crawl-ref/source dir> <out dir>
#   DCSS3DS_SCRIPT="WAIT:5,TOUCH:17:107,WAIT:40,SLIDER:1,X,WAIT:60" \
#   DCSS3DS_OUT=/tmp/frames DCSS3DS_EVERY=5 DCSS3DS_MAX=400 \
#     <out>/crawl-3ds-headless -dir <out>/save -name T -species Mi -background Be
set -euo pipefail
SRC="$(cd "$1" && pwd)"; OUT="$(mkdir -p "$2" && cd "$2" && pwd)"
HERE="$(cd "$(dirname "$0")" && pwd)"

# 1. a normal native console build (provides every object except the backend)
make -C "$SRC" -j"$(nproc)" BUILD_LUA=y BUILD_PCRE=y >/dev/null
python3 "$HERE/../patch_crawl.py" "$SRC" >/dev/null

# 2. recompile the files that change under TARGET_OS_3DS, plus the backend
F="-std=c++11 -O1 -g -w -Iutil -I. -isystem contrib/install/$(cc -dumpmachine)/include
   -DWIZARD -DASSERTS -DCLUA_BINDINGS -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=600
   -DTARGET_OS_3DS -DREGEX_PCRE -isystem $HERE -include $HERE/3ds.h"
cd "$SRC"
for f in lib3ds syscalls crash random sqldbm; do g++ $F -c $f.cc -o "$OUT/$f.o"; done
g++ $F -include lib3ds.h -c main.cc -o "$OUT/main.o"
echo 'void fakecursorxy(int, int) {}' > "$OUT/stubs.cc"; g++ -c "$OUT/stubs.cc" -o "$OUT/stubs.o"

# 3. link with the native objects, minus libunix and the five above
OBJS=$(ls *.o rltiles/*.o util/*.o | grep -v '^libunix.o$\|^main.o$\|^syscalls.o$\|^crash.o$\|^random.o$\|^sqldbm.o$\|^libw32c.o$')
g++ -o "$OUT/crawl-3ds-headless" $OBJS "$OUT"/{main,syscalls,crash,random,sqldbm,lib3ds,stubs}.o \
    contrib/install/$(cc -dumpmachine)/lib/liblua.a contrib/install/$(cc -dumpmachine)/lib/libpcre.a \
    -lsqlite3 -lz -lpthread
# data dirs next to the binary so crawl finds them
ln -sfn "$SRC/dat" "$OUT/dat"; ln -sfn "$SRC/../docs" "$OUT/docs"; ln -sfn "$SRC/../settings" "$OUT/settings"
mkdir -p "$OUT/save"; echo "clear_messages = true" > "$OUT/save/init.txt"
echo "built $OUT/crawl-3ds-headless"
