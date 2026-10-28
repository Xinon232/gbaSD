#!/bin/sh
# gbaSD's FatFs tests (see run.py): exfat_repair, exFAT folder stretch and
# FAT/exFAT folder-growth power cuts, the exFAT kit, in both switch
# configurations. Prints SKIP for parts whose tools are missing.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
if ! command -v python3 >/dev/null; then
  echo "SKIP: gbaSD FatFs tests need python3"
  exit 0
fi
exec python3 -B "$here/run.py" "$@"
