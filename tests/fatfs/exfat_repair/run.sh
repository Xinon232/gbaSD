#!/bin/sh
# Host test of the exfat_repair module (fatfs/exfat_repair.c), against
# gbaSD's FatFs (fatfs/) with an ffconf.h, FF_USE_MKFS turned on for the
# test build only. Builds with ASan + UBSan, as C99 and the module also as
# C++ (callable from C++). From gbaplanner's tests/exfat_repair (run.sh and
# test_exfat_repair.c, with gbamp3's and gbavocab's changes).
#   FFCONF=path/to/ffconf.h sh tests/fatfs/exfat_repair/run.sh
# FFCONF: the configuration to test (an app's include/ffconf.h; default
# fatfs/ffconf_example.h). FATFS_SRC (ff.c, ffunicode.c, ffsystem.c if
# there), FATFS_INC (ff.h, diskio.h), MODULE_SRC, MODULE_INC (exfat_repair.c,
# .h) default to gbaSD's fatfs/; BUILD to a temporary folder.
# Needs gcc, g++ and fsck.exfat (exfatprogs; skipped when missing).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$here/../../.." && pwd)
FATFS_SRC=${FATFS_SRC:-$ROOT/fatfs}
FATFS_INC=${FATFS_INC:-$ROOT/fatfs}
FFCONF=${FFCONF:-$ROOT/fatfs/ffconf_example.h}
MODULE_SRC=${MODULE_SRC:-$ROOT/fatfs}
MODULE_INC=${MODULE_INC:-$ROOT/fatfs}
PATH=$PATH:/usr/sbin:/sbin
if ! command -v fsck.exfat >/dev/null; then
  echo "SKIP: fsck.exfat (exfatprogs) not installed"
  exit 0
fi
if [ -z "${BUILD:-}" ]; then
  BUILD=$(mktemp -d)
  trap 'rm -rf "$BUILD"' EXIT
fi
mkdir -p "$BUILD/fatfs" "$BUILD/module"
for f in ff.c ffunicode.c ffsystem.c; do
  if [ -f "$FATFS_SRC/$f" ]; then cp "$FATFS_SRC/$f" "$BUILD/fatfs/"; fi
done
cp "$FATFS_INC/ff.h" "$FATFS_INC/diskio.h" "$BUILD/fatfs/"
grep -Eq '#define[[:space:]]+FF_FS_EXFAT[[:space:]]+1' "$FFCONF" || { echo "FAIL: FF_FS_EXFAT is 0 in $FFCONF"; exit 1; }
sed -E 's/#define([[:space:]]+)FF_USE_MKFS([[:space:]]+)0/#define\1FF_USE_MKFS\21/' "$FFCONF" > "$BUILD/fatfs/ffconf.h"
grep -Eq '#define[[:space:]]+FF_USE_MKFS[[:space:]]+1' "$BUILD/fatfs/ffconf.h"
# The test makes entry sets of up to 19 entries (names a PC can write): a
# configuration with shorter names (gbavocab: FF_MAX_LFN / FF_LFN_BUF 63) gets
# 255 for both here. The module does not depend on them.
sed -i -E 's/#define([[:space:]]+)FF_(MAX_LFN|LFN_BUF)([[:space:]]+)[0-9]+/#define\1FF_\2\3255/' "$BUILD/fatfs/ffconf.h"
[ "$(grep -Ec '#define[[:space:]]+FF_(MAX_LFN|LFN_BUF)[[:space:]]+255' "$BUILD/fatfs/ffconf.h")" = 2 ]
# The module header on its own: next to an ff.h its #include "ff.h" would
# find that one (and an ffconf.h beside it) instead of the copies.
cp "$MODULE_INC/exfat_repair.h" "$BUILD/module/"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
CF="-std=c99 -O1 -g -Wall -Wextra -Werror $SAN -I$BUILD/fatfs -I$BUILD/module"
OBJS=""
for f in "$BUILD"/fatfs/*.c; do
  gcc -O1 -g $SAN -I"$BUILD/fatfs" -c "$f" -o "$f.o"
  OBJS="$OBJS $f.o"
done
gcc $CF -pedantic -c "$MODULE_SRC/exfat_repair.c" -o "$BUILD/exfat_repair.o"
g++ -x c++ -std=c++17 -Wall -Wextra -Werror -I"$BUILD/fatfs" -I"$BUILD/module" -fsyntax-only "$MODULE_SRC/exfat_repair.c"
gcc $CF "$here/test_exfat_repair.c" "$BUILD/exfat_repair.o" $OBJS -o "$BUILD/test_exfat_repair"
ASAN_OPTIONS=detect_leaks=0 "$BUILD/test_exfat_repair" "$BUILD"
