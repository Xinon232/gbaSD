#!/bin/sh
# Self-test of the exFAT test kit: builds exfat_tool against gbaSD's FatFs
# (FFCONF: the ffconf.h to use, default fatfs/ffconf_example.h) and runs
# test_tools.py contiguous and in every EXFAT_FRAGMENT_MODE.
# Needs gcc, python3, mtools, dosfstools and exfatprogs (SKIP otherwise).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
PATH=$PATH:/usr/sbin:/sbin
for t in mkfs.exfat fsck.exfat mkfs.fat fsck.fat mcopy mdir mtype mattrib mren mdel mmd; do
  if ! command -v "$t" >/dev/null; then
    echo "SKIP: exFAT kit self-test ($t not installed: exfatprogs, dosfstools, mtools)"
    exit 0
  fi
done
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
python3 "$here/build.py" "$OUT/exfat_tool" ${FFCONF:+--ffconf="$FFCONF"} >/dev/null
export EXFAT_TOOL="$OUT/exfat_tool" ASAN_OPTIONS=detect_leaks=0
EXFAT_FRAGMENT=0 python3 -B "$here/test_tools.py"
for mode in pairs files put none; do
  EXFAT_FRAGMENT=1 EXFAT_FRAGMENT_MODE=$mode python3 -B "$here/test_tools.py"
done
echo "exFAT kit self-test PASS"
