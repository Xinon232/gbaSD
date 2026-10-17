#!/bin/sh
# Host tests of the SuperCard driver C code (bus routines replaced by a
# scripted card), in the default and in gbamp3's configuration.
# The inline-asm mode switch is cut from a temporary copy of the driver.
set -eu
cd "$(dirname "$0")/../.."
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 - "$OUT/supercard_host.c" <<'PY'
import sys
s=open('src/supercard_driver.c').read()
a=s.index('void write_supercard_mode(');b=s.index('void set_supercard_mode(',a)
open(sys.argv[1],'w').write(s[:a]+'void write_supercard_mode(uint16_t m){(void)m;}\n'+s[b:])
PY
CC=${CC:-cc}
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
for cfg in default gbamp3; do
  if [ "$cfg" = gbamp3 ]; then
    D="-DGBASD_SC_IDLE_TIMEOUT=0x8000 -DGBASD_SC_DATA_TIMEOUT=0x4000 -DGBASD_SC_WRITE_RETRIES=0 -DGBASD_SC_MAX_WRITE_BLOCKS=16"
  else
    D=""
  fi
  for t in init write; do
    $CC -std=c11 -O1 -g $SAN -fwrapv -DSDDRV_TIMEOUT_MULT=4096 $D -Iinclude \
      "$OUT/supercard_host.c" src/crc.c "tests/host/test_supercard_$t.c" -o "$OUT/$t-$cfg"
    printf '%s: ' "$cfg"; "$OUT/$t-$cfg"
  done
done
echo "gbaSD host tests PASS"
