# crc_diag: SuperCard SD read CRC diagnostics ROM

A small test ROM that checks, on a real SuperCard SD, that the read CRC16
check of gbaSD's SuperCard driver sees the CRC bytes where it expects them
(see [docs/supercard-sd.md](../../docs/supercard-sd.md)). It uses gbaSD's own
driver files unchanged. Nothing is written to the card.

Two ROMs:

| ROM | Driver mode | Use |
|---|---|---|
| `crc_diag.gba` | count only (`GBASD_SC_READ_CRC=1`) | **The test.** Every CRC mismatch is counted, nothing is retried, the data is used as read. |
| `crc_diag_check.gba` | check and retry (the apps' default) | Shows what the apps do: retries, read errors, safety switch. |

Prebuilt: `crc_diag.gba` and `crc_diag_check.gba` in this folder. Build:
`make` (devkitARM and libgba from devkitPro; `DEVKITPRO` defaults to
`/opt/devkitpro`).

## Running it

1. Put a card that already has files on it (a few MB of music, books or
   anything) in the SuperCard. FAT16, FAT32 and exFAT all work.
2. Copy `crc_diag.gba` to the card and start it from the SuperCard menu like
   any game.
3. It finds the volume and reads, over and over, its first 64 sectors (boot
   sector and FAT) and the first 8 MiB of its data area (where the files
   are), in reads of 1 to 32 sectors, a third of them into unaligned buffers
   (both read loops). Let it run for at least two passes (a pass takes about
   half a minute).

| Line | Meaning |
|---|---|
| Phase, Pass | What it reads now; the current pass |
| Sectors | Sectors read so far |
| Non-zero | Of these, sectors that are not all zero (only these prove the layout: a zero sector has CRC 0) |
| Speed | Read speed of the last half second |
| Read errs | Reads that failed (timeouts; check ROM: also CRC failures after retries) |
| CRC checked | Sectors whose CRC was checked |
| Mismatches | Sectors whose CRC did not match |
| reread same / other | Count ROM: a read with a mismatch is read again and compared. *same*: the data was the same, so the CRC bytes are suspect (layout); *other*: the data changed, a real transfer error |
| Retries, Failed, Suspect, Trusted | Check ROM: re-reads, reads given up (SD error in the apps), mismatches kept before the layout was trusted, 64 sectors matched |
| Check disabled | Check ROM: the safety switch turned the check off |
| First bad read | Start sector of the first read with a mismatch |
| RESULT | `0 mismatches, CRC OK` after 2048 non-zero sectors without a mismatch |

**What to report:** a photo of the screen after two or more passes, the
SuperCard model (SD, Lite, Rumble, other) and the SD card (brand, size).

- **0 mismatches** over thousands of non-zero sectors: the layout is right
  on this cart; the check in the apps is safe.
- **Mismatches on every sector** (Mismatches close to CRC checked, reread
  *same*): the CRC bytes are not where the driver looks on this cart. The
  apps' safety switch turns the check off after 3 sectors, so they keep
  working as before, but please report it.
- **A few mismatches with reread *other***: real transfer errors that the
  check now catches; the apps read those sectors again.

Hold **L+R** while starting to force the SuperCard path (as in the apps). On
an EZ-Flash Omega or EverDrive it says "Not a SuperCard".

## Emulator test

`tests/emu/crc_diag_test.py` runs both ROMs on mGBA against the SuperCard
model with injected faults (needs libmgba, mtools, dosfstools).
