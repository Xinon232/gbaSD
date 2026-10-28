# Credits

- **SuperCard SD driver** (`src/supercard_driver.c`, `src/supercard_io.s`,
  `src/supercard_write.s`, `src/crc.c`, `include/supercard_driver.h`,
  `include/crc.h`): from SuperFW by David Guillen Fandos, Copyright (C)
  2024-2025, GPL-3.0-or-later. Changed by Halim Jarrar for gbareader and
  gbamp3 (single-block reads, stop after a failed read, strict write status,
  correct SDSC capacity, settings for timeouts, retries and placement).
- **EZ-Flash Omega driver** (`src/ezfo_driver.c`): Halim Jarrar,
  GPL-3.0-or-later, written after the EZ-Flash Omega and Omega DE kernels
  (github.com/ez-flash/omega-kernel, omega-de-kernel) and ezfo-disc_io by
  Felix Jones and ApacheThunder, all Apache-2.0. No file of those projects is
  included; see `docs/ezflash-omega/`.
- **EverDrive GBA driver** (`src/edgba_driver.c`): port of edio-gba
  (`everdrive.c`, `disk.c`) by krikzz, github.com/krikzz/gba-ed-pub,
  Copyright (c) 2024 krikzz, MIT licence (notice kept in the file). Changes
  by Halim Jarrar, GPL-3.0-or-later.
- **FatFs** (`fatfs/ff.c`, `fatfs/ff.h`, `fatfs/ffunicode.c`,
  `fatfs/ffsystem.c`, `fatfs/diskio.h`, and `fatfs/ffconf_example.h` from
  its `ffconf.h`): FatFs R0.15 w/patch1 by ChaN, elm-chan.org/fsw/ff,
  Copyright (C) 2022, ChaN, under the FatFs licence (notice kept in each
  file):

  > FatFs module is an open source software. Redistribution and use of FatFs in
  > source and binary forms, with or without modification, are permitted provided
  > that the following condition is met:
  >
  > 1. Redistributions of source code must retain the above copyright notice,
  >    this condition and the following disclaimer.
  >
  > This software is provided by the copyright holder and contributors "AS IS"
  > and any warranties related to this software are DISCLAIMED.
  > The copyright owner or contributors be NOT LIABLE for any damages caused
  > by use of this software.

  Changes (marked `gbaSD:`, see `docs/fatfs.md`): the exFAT power-cut
  fixes from gbaplanner, gbawriter, gbavocab, gbareader and gbamp3, the
  FAT folder-growth fix, and gbamp3's faster exFAT lookup, 32-bit file
  sizes and open-by-entry functions behind switches; by Halim Jarrar,
  GPL-3.0-or-later. `fatfs/diskio.c` follows ChaN's disk I/O skeleton
  (the apps' glue to `cart_io`).
- **exfat_repair** (`fatfs/exfat_repair.c`, `fatfs/exfat_repair.h`):
  Halim Jarrar, from gbaplanner (used by all five apps), GPL-3.0-or-later.
- **exFAT test kit** (`tests/exfat_kit/`) and the FatFs tests
  (`tests/fatfs/`): from the tests of gbamp3 (exfat_tool, mtools
  stand-ins, exfat_read), gbaplanner (stretch_cut, test_tools,
  exfat_repair test), gbawriter, gbavocab and gbareader; Halim Jarrar,
  GPL-3.0-or-later.
- **Cart detection, models and tests**: Halim Jarrar, GPL-3.0-or-later.
