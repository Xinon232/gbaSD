#!/bin/sh
# Compiles gbaSD in the default configuration (gbareader, gbawriter, gbavocab:
# Thumb, ROM), in the read CRC16 variants and in gbamp3's (ARM, IWRAM/EWRAM
# sections, short timeouts, read loop unrolled twice).
# Needs devkitARM (DEVKITARM or arm-none-eabi-gcc on PATH).
set -eu
cd "$(dirname "$0")/.."
CC=${DEVKITARM:+$DEVKITARM/bin/}arm-none-eabi-gcc
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
COMMON="-mcpu=arm7tdmi -mthumb-interwork -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Iinclude"

build() {
  name=$1; shift
  for f in src/*.c; do
    "$CC" $COMMON "$@" -c "$f" -o "$OUT/$name-$(basename "$f").o"
  done
  for f in src/*.s; do   # assembly with C preprocessor directives
    "$CC" $COMMON "$@" -x assembler-with-cpp -c "$f" -o "$OUT/$name-$(basename "$f").o"
  done
  echo "gbaSD $name configuration: $(ls "$OUT" | grep -c "^$name-") objects OK"
}

build default -mthumb -O2
# Read CRC16 modes (count only, off) and loop unroll counts.
build crc-count -mthumb -O2 -DGBASD_SC_READ_CRC=1
build crc-off -mthumb -O2 -DGBASD_SC_READ_CRC=0
for u in 1 2 8 16 32; do build unroll$u -mthumb -O2 -DGBASD_SC_READ_UNROLL=$u; done
build gbamp3 -marm -O3 -mlong-calls -DGBASD_SC_READ_UNROLL=2 \
  -DGBASD_SC_IDLE_TIMEOUT=0x8000 -DGBASD_SC_DATA_TIMEOUT=0x4000 \
  -DGBASD_SC_WRITE_RETRIES=0 -DGBASD_SC_MAX_WRITE_BLOCKS=16 \
  '-DGBASD_SC_FAST_FN=__attribute__((section(".iwram"),noinline,noclone))' \
  '-DGBASD_SC_CRC7_FN=__attribute__((section(".iwram"),noinline,noclone))' \
  '-DGBASD_SC_CRC16_FN=__attribute__((section(".ewram"),target("arm"),noinline))' \
  -DGBASD_SC_CMD_IN_IWRAM -DGBASD_SC_WRITE_IN_EWRAM
python3 -m py_compile tests/check_ram_code.py
echo "gbaSD build check PASS"
