# Credits

- **SuperCard SD driver** (`src/supercard_driver.c`, `src/supercard_io.s`,
  `src/supercard_write.s`, `src/crc.c`, `include/supercard_driver.h`,
  `include/crc.h`): from SuperFW by David Guillen Fandos, Copyright (C)
  2024-2025, GPL-3.0-or-later. Changed by Halim Jarrar for gbareader and
  gbamp3 (single-block reads, stop after a failed read, strict write status,
  correct SDSC capacity, settings for timeouts, retries and placement, the
  read CRC16 check in the read loops, which uses the interleaved four-line
  CRC16 formulation of SuperFW's `crc.c`).
- **EZ-Flash Omega driver** (`src/ezfo_driver.c`): Halim Jarrar,
  GPL-3.0-or-later, written after the EZ-Flash Omega and Omega DE kernels
  (github.com/ez-flash/omega-kernel, omega-de-kernel) and ezfo-disc_io by
  Felix Jones and ApacheThunder, all Apache-2.0. No file of those projects is
  included; see `docs/ezflash-omega/`.
- **EverDrive GBA driver** (`src/edgba_driver.c`): port of edio-gba
  (`everdrive.c`, `disk.c`) by krikzz, github.com/krikzz/gba-ed-pub,
  Copyright (c) 2024 krikzz, MIT licence (notice kept in the file). Changes
  by Halim Jarrar, GPL-3.0-or-later.
- **Cart detection, models and tests**: Halim Jarrar, GPL-3.0-or-later.
