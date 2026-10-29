# Plan: FAT 8.3 alias left by a power cut during delete or rename

Status: **planned, not implemented.** Found 2026-10-29 by gbawriter's FAT16
power-cut sweep (`tests/test_writer_fatfs.cpp` in gbawriter, counter
`alias_left`). exFAT is not affected.

## What happens

On FAT12/16/32 a file with a long name has one or more long-name (LFN)
entries followed by its 8.3 short-name entry (SFN), e.g. `ADIARY~1.TXT` for
`A diary 13.07.2026.txt`. `dir_remove()` in `fatfs/ff.c` marks these
entries deleted **in order, LFN entries first, SFN entry last**. When the
entries cross a sector boundary, FatFs writes the first sector (LFN entries
deleted) before it moves its window to the next one (SFN entry). A power cut
between those two sector writes leaves:

- the LFN entries deleted,
- the SFN entry still in use.

The file is then visible under its 8.3 alias only (`ADIARY~1.TXT`). Its data
is intact and the card is consistent for FAT (fsck.fat sees an ordinary
file), but the apps list it as an extra file with an unexpected name.

`f_rename()` registers the new entry and then calls `dir_remove()` on the
old one, so an interrupted rename can leave the alias next to the renamed
file; `f_unlink()` can leave it instead of deleting the file.

Measured in gbawriter's sweep (save = staging copy, renames, delete of the
backup): 42 of 2,861 cuts on FAT16 left an alias (`ADIARY~1.TXT`, `.GWI`,
`.GWT`, `.GWB`); 0 on exFAT. In gbawriter a leftover `.TXT` alias would show
up on Home as an extra document; the others are recovery files the app may
refuse ("RECOVERY: CHECK SD ON PC").

## Proposed fix (in gbaSD, for all five apps)

1. **Reverse the deletion order on FAT in `dir_remove()`:** mark the SFN
   entry deleted first (the entry `dp` points at, `last`), write it, then the
   LFN entries. A cut in between then leaves **orphaned LFN entries** with
   no SFN entry. Readers (FatFs, Windows, Linux, mtools) skip orphaned LFN
   entries; the file is gone as intended; fsck.fat reports and removes them
   as harmless leftovers. exFAT keeps its current order (its entry sets are
   handled by `exfat_repair`).
   - Behind a switch like the folder-growth fix
     (`GBASD_FF_FAT_REMOVE_ORDER_FIX`, default 1; 0 restores R0.15 for the
     tests), FAT-only, commented with `gbaSD:`.
   - Same number of sector writes; no RAM cost.
2. **Rename:** with (1), an interrupted rename leaves the new entry plus
   orphaned LFN entries of the old one, never a visible alias. Check
   whether FatFs's same-folder rename path needs the same ordering.
3. **Optional, decide later:** a FAT counterpart of `exfat_repair` that
   clears orphaned LFN entries at start, so fsck finds nothing. Not needed
   for correct listings; only cosmetic for PC checks.

## Tests to add (gbaSD `tests/fatfs/`)

- New power-cut sweep `remove_cut.py` (like `growth_cut.py`): FAT12, FAT16,
  FAT32 with 512 B and larger clusters; files whose LFN + SFN entries cross
  a sector boundary at every offset; `f_unlink`, `f_rename` (same folder and
  across folders); cut after every sector write. After each cut: FatFs
  listing and `mdir -b` show only the old or the new names (never an
  `~N` alias), fsck.fat reports at most orphaned LFN entries, FatFs can
  create/rename/delete afterwards.
- Must **fail** with the switch at 0 (R0.15 order) and pass with it on.
- Equivalence: only `dir_remove` (and its inlined callers) change, as with
  the folder-growth fix.

## Rollout

Same as the FatFs move: new gbaSD version, then each app's submodule bump,
all app tests on FAT and exFAT on all three cart models. gbawriter's sweep
then turns `alias_left` from a counter into a must-be-zero check.
