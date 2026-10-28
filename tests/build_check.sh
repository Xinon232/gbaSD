#!/bin/sh
# Compiles gbaSD in the default configuration (gbareader, gbawriter, gbavocab:
# Thumb, ROM) and in gbamp3's (ARM, IWRAM/EWRAM sections, short timeouts).
# fatfs/ too: with fatfs/ffconf_example.h (every GBASD_FF_* switch off) in the
# default configuration, with the three switches on in gbamp3's.
# Needs devkitARM (DEVKITARM or arm-none-eabi-gcc on PATH).
set -eu
cd "$(dirname "$0")/.."
CC=${DEVKITARM:+$DEVKITARM/bin/}arm-none-eabi-gcc
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
COMMON="-mcpu=arm7tdmi -mthumb-interwork -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude"

mkdir -p "$OUT/conf-default" "$OUT/conf-gbamp3"
cp fatfs/ffconf_example.h "$OUT/conf-default/ffconf.h"
sed -E 's/#define(\s+)(GBASD_FF_FAST_EXFAT_LOOKUP|GBASD_FF_FSIZE32|GBASD_FF_OPEN_ENTRY)(\s+)0/#define\1\2\31/' \
  fatfs/ffconf_example.h > "$OUT/conf-gbamp3/ffconf.h"
[ "$(grep -Ec '#define\s+GBASD_FF_\w+\s+1' "$OUT/conf-gbamp3/ffconf.h")" = 3 ]

build() {
  name=$1; shift
  for f in src/*.c; do
    "$CC" $COMMON "$@" -c "$f" -o "$OUT/$name-$(basename "$f").o"
  done
  for f in fatfs/*.c; do
    # exfat_repair.c: GCC 16 at -O3 -marm takes s.o0 for maybe uninitialized
    # (a false report: it is set before any entry of a set is used; the file
    # is the apps' copy, unchanged)
    case $f in *exfat_repair.c) quiet=-Wno-maybe-uninitialized ;; *) quiet= ;; esac
    "$CC" $COMMON "$@" $quiet -Ifatfs -I"$OUT/conf-$name" -c "$f" -o "$OUT/$name-fatfs-$(basename "$f").o"
  done
  for f in src/*.s; do   # assembly with C preprocessor directives
    "$CC" $COMMON "$@" -x assembler-with-cpp -c "$f" -o "$OUT/$name-$(basename "$f").o"
  done
  echo "gbaSD $name configuration: $(ls "$OUT" | grep -c "^$name-") objects OK"
}

build default -mthumb -O2
build gbamp3 -marm -O3 -mlong-calls \
  -DGBASD_SC_IDLE_TIMEOUT=0x8000 -DGBASD_SC_DATA_TIMEOUT=0x4000 \
  -DGBASD_SC_WRITE_RETRIES=0 -DGBASD_SC_MAX_WRITE_BLOCKS=16 \
  '-DGBASD_SC_FAST_FN=__attribute__((section(".iwram"),noinline,noclone))' \
  '-DGBASD_SC_CRC7_FN=__attribute__((section(".iwram"),noinline,noclone))' \
  '-DGBASD_SC_CRC16_FN=__attribute__((section(".ewram"),target("arm"),noinline))' \
  -DGBASD_SC_CMD_IN_IWRAM -DGBASD_SC_WRITE_IN_EWRAM
python3 -m py_compile tests/check_ram_code.py
echo "gbaSD build check PASS"
