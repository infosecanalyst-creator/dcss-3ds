#!/usr/bin/env bash
# Build Dungeon Crawl Stone Soup for the 3DS inside devkitPro's MSYS2:
#   bash /c/devkitPro/3ds-projects/dcss-3ds/build-dcss.sh
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=/opt/devkitpro/devkitARM
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
cd "$(dirname "$0")" || exit 1
LOG=$PWD/dcss-build.log; : > "$LOG"
{
  [ -d crawl ] || tar xzf crawl-src.tgz
  # console build: tilegen only fopen()s each tile PNG, so zero-byte placeholders suffice
  [ -f crawl/crawl-ref/source/rltiles/gui/prompt_yes.png ] || tar xzf rltiles-placeholders.tgz
  # crawl's generators (species, mutations...) need PyYAML
  python3 -c "import yaml" 2>/dev/null || python3 -m pip install --user pyyaml || python -m pip install --user pyyaml
  which perl python3 flex bison arm-none-eabi-g++ 3dsxtool smdhtool
  cd crawl/crawl-ref/source
  echo "=== building crawl.3dsx"
  make TARGET_3DS=y -j"$(nproc)" 3dsx
  echo "EXIT: $?"
  mkdir -p /c/devkitPro/3ds-projects/SD-CARD/3ds/crawl
  [ -f crawl.3dsx ] && mv -f crawl.3dsx /c/devkitPro/3ds-projects/SD-CARD/3ds/crawl/ && echo "moved to SD-CARD/3ds/crawl/"
  ls -la /c/devkitPro/3ds-projects/SD-CARD/3ds/crawl/
} 2>&1 | tee -a "$LOG"
