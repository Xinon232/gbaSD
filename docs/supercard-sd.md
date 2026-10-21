# SuperCard SD driver

Used by gbareader, gbawriter, gbavocab and gbamp3 ("the apps" below), which
share it through gbaSD. From SuperFW by David Guillen Fandos (see
`CREDITS.md`). See also [ezflash-omega.md](ezflash-omega.md) and
[everdrive-gba.md](everdrive-gba.md).

## How it works

- The SuperCard maps the SD card's command line and four data lines into the
  ROM address space. The GBA clocks the card itself: every read of the data
  register is one clock, and the latch holds the last nibbles in the top half
  of the word (`supercard_io.s`).
- **Start** (`sdcard_init`): CMD0, CMD8, ACMD41 (SDHC requested on v2
  cards), CID, RCA, CSD (capacity), select, 4-bit bus, 512-byte blocks.
- **Reads** (`sdcard_read_blocks`): CMD17 (one sector) or CMD18 (several,
  stopped with CMD12), then the assembly loop waits for each block's start
  bit and reads 512 data bytes, the 8 CRC bytes and the end bit.
- **Writes** (`sdcard_write_blocks`): CMD25 with the CRC16 of every block
  (`crc.c`), CMD12, busy wait and CMD13 status; failed writes are retried
  `GBASD_SC_WRITE_RETRIES` times.

## Read CRC16 check

The card sends, after each 512-byte block, a CRC16 for each of its four data
lines (CRC16-CCITT, polynomial 0x1021, initial value 0, of the bits that line
carried), interleaved like the data: 16 nibbles, nibble bit *n* belongs to
line *n*, most significant bit first. The upstream driver read these 8 bytes
and threw them away, so a transfer error gave wrong data silently.

**In the loop.** The assembly computes the four CRCs while it reads (the
interleaved form from SuperFW's `crc.c`: the four 16-bit states kept as one
64-bit value in two registers, 8 ARM instructions per 4 data bytes with the
byte swap folded in) and then compares the state with the 8 CRC bytes it
reads anyway. The CRC bytes arrive through the latch exactly like data
bytes, so they are merged the same way. Both read loops have it: the fast one
for word-aligned buffers (`ldmia`, unrolled `GBASD_SC_READ_UNROLL` times) and
the byte loop for unaligned buffers.

**On a mismatch** (check mode, the default) the loop returns at once, with the
failing sector read in full and the transfer still open. The C code (in ROM,
`sdcard_read_recover`):

1. stops the transfer and reads again from the failing sector on (the sectors
   before it are kept), up to `GBASD_SC_READ_RETRIES` (2) more times per
   failing sector;
2. if the sector still fails: `SD_ERR_BADREAD`, which the apps show as an SD
   error. Wrong data is never returned as good once the check is trusted
   (below).

**Safety switch.** That the CRC bytes come through the SuperCard latch like
the data bytes is inferred from the write path, which sends them the same
way, and holds on the model; it has not been shown on every SuperCard
variant (see *Hardware test*). If it were wrong for a cart, almost every
sector would mismatch and every read would fail. So:

- until `GBASD_SC_CRC_TRUST` (64) sectors have matched since `sdcard_init()`,
  a sector that still mismatches after its retries is returned as read (as
  without the check) and counted as *suspect*;
- `GBASD_SC_CRC_SUSPECTS` (3) suspect sectors in that time switch the check
  off until the next `sdcard_init()`: data is returned as read, as before;
- after 64 matching sectors the layout is proven for this cart and session;
  from then on a persistent mismatch is a read error.

64 rather than "the first match": an all-zero sector has CRC 0, which a
wrongly placed CRC may also read as 0, so a few matches could be luck. A
wrong layout fails on every non-zero sector, and the boot sector, FAT and
directories come first, so it is switched off within the first reads (3
sectors, 9 reads, a few milliseconds). With a right layout and real errors,
a retry nearly always succeeds, so 3 sectors that fail three times each
before 64 have matched is very unlikely; if it happens the result is the old
behaviour, not a failure.

**Counters** (`include/supercard_driver.h`):

```c
const t_sc_crc_stats *st = sdcard_crc_stats();
st->sectors      /* sectors read with the check on (each attempt) */
st->mismatches   /* of these, CRC did not match */
st->retries      /* re-reads after a mismatch */
st->failures     /* reads failed after all retries (SD_ERR_BADREAD) */
st->suspects     /* mismatching sectors returned as read before trust */
st->mode         /* GBASD_SC_READ_CRC */
st->trusted      /* 64 sectors matched (updated by sdcard_crc_stats()) */
st->disabled     /* safety switch: check off until sdcard_init() */
```

**Modes** (`GBASD_SC_READ_CRC`, `include/gbasd_config.h`): 2 check and retry
(default), 1 count only (mismatches counted, data returned, nothing
retried: `tools/crc_diag`), 0 off (the loops without the CRC instructions).
The read routines carry the mode in a second symbol name
(`sc_read_sectors_crc2_w0`), so C and assembly compiled with different values
do not link. Butano passes `USERFLAGS` to C only: a Butano app that changes
`GBASD_SC_READ_CRC` or `GBASD_SC_READ_UNROLL` sets them in `USERASFLAGS`
too.

**SuperCard Lite / SuperChis I/O** (`SUPERCARD_LITE_IO`, `SUPERCHIS_IO`, not
used by the apps): no CRC check (the counters stay 0).

### Cost

Measured on mGBA (WAITCNT 0x40c0, cycle counts from the emulator's bus
timing, 16 sectors per call):

| Read loop | Cycles per sector, buffer in EWRAM / IWRAM | IWRAM code (supercard_io.s) |
|---|---|---|
| Before (no CRC, unrolled 16 times) | 4256 / 3616 | 588 B |
| CRC off (`GBASD_SC_READ_CRC=0`) | 4261 / 3621 | 620 B |
| Check, unroll 1 | 5541 / 4901 | 484 B |
| Check, unroll 2 (gbamp3) | 5413 / 4773 | 572 B |
| Check, unroll 4 (default) | 5349 / 4709 | 748 B |
| Check, unroll 8 | 5317 / 4677 | 1100 B |
| Check, unroll 16 | 5301 / 4661 | 1804 B |
| Count only, unroll 4 | 5352 / 4712 | 776 B |
| Unaligned buffer, before / with CRC | 6736 / 7270 (EWRAM) | |

About +1100 cycles (+25%) per sector, about 66 µs at 16.78 MHz. The
unaligned loop now reads 4 bytes per pass instead of 2. Retries and the stop
command run from ROM; `sdcard_read_blocks` itself got smaller (gbamp3:
its cart overlay `.cart_sc` is 48 bytes smaller with unroll 2).

gbamp3 at 320 kbps reads about 79 sectors per second (one sector per
command). In its 60-second 320 kbps stress run on the model (mono), the
time in SD reads went from 6502 to 7725 cycles per sector (longest read 6898
to 8038 cycles): about 96000 more cycles per second, 0.6% of the CPU. The
run passes (no underrun) before and after, and also with a flipped bit
(one retry) and with the old constant CRC (check switched off after 3
sectors, 6 extra reads).

## Hardware test

`tools/crc_diag` builds a diagnostics ROM that reads many megabytes of the
card in count-only mode and shows the counters. 0 mismatches over a few
thousand non-zero sectors proves the CRC layout on that cart. See
[tools/crc_diag/README.md](../tools/crc_diag/README.md).

## How it was tested

- `tests/host/run.sh`: the CRC (a bit-by-bit reference, `crc.c` and a C copy
  of the loop's instructions agree; every single data bit flip in all four
  lines and every CRC bit flip is detected) and the driver's retry, read
  error and safety switch against a scripted card, in all three modes.
- `tests/emu/crc_diag_test.py`: both diagnostics ROMs on mGBA against
  `tests/models/scsd_model.h`, which sends the correct CRC16 and can inject
  faults (`SD_CRC_FLIP`, `SD_CRC_FLIP_LBA`, `SD_CRC_BAD`, `SD_CRC_STICKY`,
  `SD_CRC_CONST`): clean reads, a flipped bit once (retried, data equal to
  the card image, aligned and unaligned buffers), a wrong CRC, a permanent
  flip (read error), a permanent flip before trust (kept) and the old
  constant CRC (check switched off, data still right).
- The apps' exact-ROM tests use the same model, so every SuperCard test now
  runs the check.

This is a model, not proof on hardware: please run `crc_diag` on real
SuperCards and report.
