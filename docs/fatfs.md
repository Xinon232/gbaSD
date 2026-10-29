# FatFs in gbaSD

`fatfs/` is the FatFs the GBA apps share: gbaplanner, gbawriter, gbavocab
and gbareader (Butano), and gbamp3 (its own build). Before it, each app
carried its own copy; the copies of the four Butano apps were the same code,
and gbamp3's had three changes of its own. They are now one source with
switches.

## Contents

| File | What |
|---|---|
| `ff.c`, `ff.h` | FatFs R0.15 w/patch1 by ChaN (`FF_DEFINED` 80286), with the changes below |
| `ffunicode.c`, `ffsystem.c`, `diskio.h` | FatFs R0.15, unchanged |
| `diskio.c` | The Butano apps' disk glue: `disk_read` / `disk_write` forward to `cart_read` / `cart_write` (`include/cart_io.h`), `disk_ioctl` answers `GET_SECTOR_SIZE` (512) |
| `exfat_repair.c`, `exfat_repair.h` | Repairs exFAT entry sets a power cut left half written (written for gbaplanner, used by all five apps; the header is its full reference) |
| `ffconf_example.h` | An example `ffconf.h` (the Butano apps' settings, every gbaSD switch shown). Not compiled: the app keeps its own `ffconf.h` |

All files use LF line endings (ChaN's files are CRLF; only the line endings
differ). There is no `ffconf.h` in `fatfs/`: `ff.h` includes `"ffconf.h"`,
and since `fatfs/` has none, the compiler finds the app's on its include
path.

## Changes to R0.15

Every change carries a `gbaSD:` comment in `ff.c` / `ff.h`.

1. **exFAT, `create_chain`: end the chain at a new fragment before linking
   it.** When a file or folder in a FAT chain gets a cluster that is not
   next to its last one, R0.15 linked the last fragment to the new cluster
   before the new cluster's FAT entry was written. A power cut in between
   left a link to a stale FAT entry, and the next stretch failed with
   FR_INT_ERR. Now the new cluster is marked end-of-chain first. (From
   gbaplanner / gbawriter / gbavocab / gbareader / gbamp3.)
2. **exFAT, `dir_next`: a sub-folder ends at its data length**, not at the
   end of its FAT chain. A cut while a folder grew can leave one cluster
   linked but not counted (and not cleared); R0.15 then read it as entries.
   Stretching the folder again takes that cluster and clears it. (Same
   origin.)
3. **exFAT, `dir_register`: a new entry set that takes two new clusters
   extends the folder by both** (512-byte clusters, a long name, the
   folder's last cluster full). R0.15 added one cluster, leaving the set's
   end past the data length (fsck.exfat error; with change 2: FR_INT_ERR).
   (Same origin.)
4. **FAT12/16/32, `dir_next`: a new folder cluster is cleared before the
   folder is linked to it** (new in gbaSD; `GBASD_FF_FAT_STRETCH_FIX`, on).
   When a folder grew, R0.15 marked the new cluster end-of-chain and linked
   the folder's last cluster to it in the FAT window, then `dir_clear`
   wrote that window back before writing the zeros. A cut after the FAT
   sector and before the last zero sector left the folder ending in a
   cluster of old file data: PCs list garbage names, fsck.fat finds bad
   entries, FatFs may fail. Now `dir_next` takes the cluster with
   `create_chain_ex(obj, clst, 0)` (the same cluster `create_chain` would
   choose, marked end-of-chain, not linked), clears it, and links it after
   that. A cut at any write leaves the folder with exactly its old entries
   (plus, after the link, an empty cluster, then the new entries).
   Leftovers a cut can cause, all harmless (fsck.fat -n reports them; a PC
   or fsck frees or fixes them; FatFs ignores them):
   - a cluster marked end-of-chain that nothing links to ("Reclaimed 1
     unused cluster"): the new folder cluster (cut before the link), or the
     new file's data cluster (cut before its entry; R0.15 does the same);
   - "FATs differ but appear to be intact. Using first FAT.": the cut came
     between a FAT sector and its copy in the second FAT (every FAT write in
     R0.15 works this way; the first FAT is the newer one, and FatFs and PCs
     use it);
   - "Free cluster summary wrong" (FAT32): the FSINFO free-cluster hint;
   - "Orphaned long file name part" / "A new long file name starts within
     an old one": a new entry block spanning two sectors whose long-name
     entries reached the card but not its short-name entry (no file; R0.15
     writes any block over a sector boundary this way, growth or not).
   Cost: the FAT sector holding the link is written once more per folder
   growth (two sector writes with two FATs), about 50 bytes of code (Thumb
   -O2). exFAT keeps its own order (changes 1 and 2 cover it).
   `GBASD_FF_FAT_STRETCH_FIX 0` brings back R0.15's order; only the tests
   use it.

### Switches (from gbamp3, off by default)

Set in the app's `ffconf.h` (`ff.h` gives each the default when it is not
set):

| Switch | Default | With 1 |
|---|---|---|
| `GBASD_FF_FAST_EXFAT_LOOKUP` | 0 | An exFAT file lookup (`dir_find`) reads the name hash of each entry block from its stream entry in place, and skips a block whose hash differs without copying it and checking its checksum (`dir_skip`). gbamp3: a lookup in a folder of songs was several times slower than on FAT and starved the audio. A torn block whose hash differs is then not seen by the lookup (FR_NO_FILE instead of FR_INT_ERR); `exfat_repair_dir` still finds it |
| `GBASD_FF_FSIZE32` | 0 | `FSIZE_t` stays 32-bit with exFAT (no 64-bit divisions in seeks; a playlist opened a frame later on any card); exFAT sizes of 4 GiB or more read as 4 GiB - 1 (`ld_fsize`); `f_write`'s 4 GiB check applies to exFAT too and `f_lseek`'s clip is gone |
| `GBASD_FF_OPEN_ENTRY` | 0 | `f_open_entry(fp, parent, fno)` / `f_opendir_entry(dp, parent, fno)` open what `f_readdir` just returned without a second path lookup (the file read-only); `FILINFO` gains `fstat` (exFAT: 2 = contiguous) and `fclust` (start cluster), and on exFAT `fsize` also holds a folder's size |

gbaplanner, gbawriter, gbavocab and gbareader use none; gbamp3 sets all three.

### Same code as the apps' copies

`tests/fatfs/equivalence.py` (a local check: it needs the app checkouts next
to gbaSD and devkitARM) compiles `fatfs/ff.c` with each app's `ffconf.h` and
compiler flags (Butano's `-O2 -mthumb`, gbamp3's `-O3 -marm -mlong-calls
...`, and the host gcc at `-O2`) and compares the object code with the
app's own `ff.c`, section by section (`objdump -dr` of every function, and
the data):

- switches off, `GBASD_FF_FAT_STRETCH_FIX 0`: identical to gbaplanner's,
  gbawriter's, gbavocab's and gbareader's FatFs;
- switches on, `GBASD_FF_FAT_STRETCH_FIX 0`: identical to gbamp3's;
- with the fix (the default): only `create_chain` (now `create_chain_ex`),
  `dir_next`, and the functions the compiler inlines them into or lays out
  around them (`f_write`, `f_lseek`, `f_mkdir`, ...) differ.

It also checks that `ffunicode.c`, `ffsystem.c`, `diskio.h`, `diskio.c` and
`ff.h` (switch blocks taken out) are the apps' text.

## Using it

### From a Butano app

With gbaSD as a submodule in `gbasd/`:

```make
SOURCES     :=  src gbasd/src gbasd/fatfs
INCLUDES    :=  include gbasd/include gbasd/fatfs
```

- Keep `include/ffconf.h` (start from `gbasd/fatfs/ffconf_example.h`; the
  app's current one works as it is).
- Delete the app's own `ff.c`, `ffunicode.c`, `ffsystem.c`, `diskio.c`,
  `exfat_repair.c` and **also** `ff.h`, `diskio.h`, `exfat_repair.h`: with
  `include` first in `INCLUDES`, a stale `include/ff.h` would be found
  before `gbasd/fatfs/ff.h`.
- `gbasd/fatfs/diskio.c` uses `cart_io` from `gbasd/src`, which the app
  already compiles.

### From gbamp3 (build.py)

gbamp3 keeps its own `player/src/diskio.c` (it polls the audio during card
access), so it compiles `ff.c`, `ffunicode.c` and `exfat_repair.c` from
`gbasd/fatfs` by name instead of the folder (not `diskio.c`; `ffsystem.c` is
not needed with `FF_USE_LFN 1`):

```python
FATFS = R / 'gbasd/fatfs'
FATFS_SRC = [FATFS / 'ff.c', FATFS / 'ffunicode.c', FATFS / 'exfat_repair.c']
# in the source list: FATFS_SRC instead of sorted((D/'vendor/fatfs').glob('*.c'))
# (and no player/src/exfat_repair.c); in flags, next to -I vendor/fatfs (now only ffconf.h):
flags += ['-I' + str(FATFS)]
```

- `player/vendor/fatfs/` keeps only `ffconf.h`, with
  `#define GBASD_FF_FAST_EXFAT_LOOKUP 1`, `#define GBASD_FF_FSIZE32 1`,
  `#define GBASD_FF_OPEN_ENTRY 1` added; delete its `ff.c`, `ff.h`,
  `ffunicode.c`, `diskio.h`, and `player/src/exfat_repair.c/.h`.
- The objects keep their names (`ff`, `ffunicode`, `exfat_repair`), so the
  `THUMB` set in build.py and the link order work as before.

### Calling exfat_repair_dir

```c
#include "exfat_repair.h"      /* C++: extern "C" is in the header */

/* 1. After mounting, before anything touches the app's folders (also before f_mkdir): */
if (f_mount(&fs, "", 1) == FR_OK) {
    exfat_repair_dir(&fs, "/myapp/data", 1);  /* repairs "/", "/myapp", "/myapp/data" */
    f_mkdir("/myapp");
}

/* 2. An operation in such a folder fails with FR_INT_ERR: repair, retry once. */
FRESULT r = f_open(&fil, "/myapp/data/x.txt", FA_READ);
if (r == FR_INT_ERR && exfat_repair_dir(&fs, "/myapp/data", 1) > 0)
    r = f_open(&fil, "/myapp/data/x.txt", FA_READ);
```

Close (or sync) the files of that folder first; not from an interrupt. On FAT
volumes it returns 0 and touches nothing. Cost: one read pass over the
folder (no writes on a sound folder), `fs->win` only, about 360 bytes of
stack, about 1.9 KB of Thumb code. See `exfat_repair.h`.

## Tests

`sh tests/fatfs/run.sh` (CI job `fatfs`; needs Linux, gcc, exfatprogs,
dosfstools, mtools, and prints SKIP for parts whose tools are missing), with
the switches off and on:

- `tests/fatfs/exfat_repair/`: the exfat_repair host test (360 cases of
  sets torn at every offset and cut point; fsck.exfat checked). An app
  runs it with its own configuration: `FFCONF=include/ffconf.h sh
  gbasd/tests/fatfs/exfat_repair/run.sh`.
- `tests/fatfs/stretch_cut.py`: exFAT folder growth, a cut after every
  sector write, the folder fragmented and contiguous.
- `tests/fatfs/growth_cut.py`: folder growth on FAT12, FAT16, FAT32 and
  exFAT (512-byte and 1-4 KB clusters, the free space holding old data), a
  cut after every sector write; fsck.fat / fsck.exfat may report only the
  leftovers listed under change 4, and mtools `mdir` (FAT) or
  `exfat_read.py` (exFAT) must list exactly the old or the new entries, as
  FatFs does, which then grows the folder further. On exFAT a new entry
  block over two sectors can be left half written; `exfat_repair_dir` runs
  first, as in the apps. With `GBASD_FF_FAT_STRETCH_FIX 0` this test fails
  on every FAT type (`--unfixed`, run by run.sh).
- `tests/exfat_kit/`: the exFAT card-tool kit's self-test (see its README).
- `tests/fatfs/equivalence.py`: the comparison above (SKIP without the app
  checkouts).

## Licence

FatFs is by ChaN, under the FatFs licence (a BSD-style licence, kept in the
header of every FatFs file and quoted in `CREDITS.md`). The gbaSD changes
and `exfat_repair` are GPL-3.0-or-later like the rest of gbaSD; the FatFs
licence allows this, provided its notice is kept.

## Known issue (planned fix)

On FAT12/16/32 a power cut while a file with a long name is deleted or renamed can leave the file visible under its 8.3 alias (e.g. `ADIARY~1.TXT`): FatFs deletes the long-name entries before the short-name entry. exFAT is not affected. Plan: [plans/fat-lfn-alias-powercut.md](plans/fat-lfn-alias-powercut.md).
