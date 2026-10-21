# Changes

## Unreleased

- **SuperCard SD: read CRC16 check.** Every sector read is checked against
  the CRC16 the card sends with it (inside the assembly read loops, both the
  aligned and the unaligned one). A mismatching sector is read again up to
  twice; still wrong: the read fails (`SD_ERR_BADREAD`) instead of returning
  wrong data. Safety switch: if the check fails from the start (3 sectors
  before 64 have matched), it is turned off until the next `sdcard_init()`
  and data is returned as before. Counters: `sdcard_crc_stats()`. Settings:
  `GBASD_SC_READ_CRC` (check / count only / off), `GBASD_SC_READ_UNROLL`,
  `GBASD_SC_READ_RETRIES`, `GBASD_SC_CRC_TRUST`, `GBASD_SC_CRC_SUSPECTS`.
  Cost: about +1100 cycles per sector (+25%). See `docs/supercard-sd.md`.
  - **gbamp3 must add `-DGBASD_SC_READ_UNROLL=2`** to its gbaSD flags when it
    updates the submodule (its IWRAM then shrinks by 48 bytes; with the
    default of 4 the link fails its IWRAM assert).
  - Butano apps need nothing (defaults); if they change `GBASD_SC_READ_CRC`
    or `GBASD_SC_READ_UNROLL`, set it in `USERASFLAGS` too.
  - `sdcard_read_blocks` keeps only the command and the transfer in
    `GBASD_SC_FAST_FN`; the stop, retries and error paths run from ROM.
  - The read routines return `uintptr_t` (0, 1 or, on a CRC mismatch, the
    buffer address after the failing sector) and have a second name with
    the mode (`sc_read_sectors_crc2_w0`), which the driver calls.
- `tests/models/scsd_model.h` sends the correct read CRC16 (was a constant)
  and can inject faults: `SD_CRC_FLIP`, `SD_CRC_FLIP_LBA`, `SD_CRC_BAD`,
  `SD_CRC_STICKY`, `SD_CRC_FLIP_BIT`, `SD_CRC_CONST`; `scsd_report()`.
- `tools/crc_diag`: diagnostics ROM for real SuperCards (count-only and
  check builds); `tests/emu/crc_diag_test.py` runs it on the model.
