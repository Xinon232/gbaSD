# Changelog

## Unreleased

- **FatFs in gbaSD** (`fatfs/`, see `docs/fatfs.md`): the FatFs R0.15 the
  apps carry, one copy for all: `ff.c`, `ff.h`, `ffunicode.c`,
  `ffsystem.c`, the Butano apps' `diskio.c` / `diskio.h`, and
  `exfat_repair.c/.h`. Apps keep their own `ffconf.h`
  (`fatfs/ffconf_example.h` shows every setting).
  - The three exFAT power-cut fixes of gbaplanner, gbawriter, gbavocab,
    gbareader and gbamp3.
  - gbamp3's changes behind switches set in `ffconf.h`, off by default:
    `GBASD_FF_FAST_EXFAT_LOOKUP`, `GBASD_FF_FSIZE32`,
    `GBASD_FF_OPEN_ENTRY`. Compiled with each app's settings the code is
    the same as the app's own copy (`tests/fatfs/equivalence.py`).
  - New fix, FAT12/16/32: a folder's new cluster is cleared on the card
    before the folder is linked to it. A power cut while a folder grew
    could leave old file data in the folder, listed as garbage names.
- **Tests**: `tests/fatfs/run.sh` (CI): exfat_repair (360 cases), exFAT
  folder growth on fragmented and contiguous cards, and a new power-cut
  sweep of folder growth on FAT12, FAT16, FAT32 and exFAT, each in both
  switch configurations. `tests/exfat_kit/`: the apps' exFAT card-tool kit
  (mtools stand-ins, exfat_tool, exfat_read.py) merged into one, for the
  apps' tests to share. `tests/build_check.sh` compiles `fatfs/` too.
