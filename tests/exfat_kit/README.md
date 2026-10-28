# exFAT test kit

Lets an app's existing card tests (written for FAT cards and mtools /
dosfstools) run unchanged on exFAT cards, written and read by the app's own
FatFs, and judged by tools that share no code with it (`fsck.exfat`,
`exfat_read.py`).

Gathered from the apps' `tests/exfat` folders: gbamp3 wrote the first
`exfat_tool.c`, `mtools.py` and `exfat_read.py`; gbaplanner, gbawriter,
gbavocab and gbareader grew them (wildcards, `mattrib`, `mdel`, `mren`,
`mtype`, `mkfs.fat` / `fsck.fat`, `mcopy -s` both ways, the layout log, the
ways of fragmenting a card). This kit merges those versions into one.

| File | What |
|---|---|
| `exfat_tool.c` | Card tool on the app's FatFs: mkdir, put, puttree, get, cat, gettree, ls, rm, mv, attr, chmod, attrib, free, fragment, fragment-files (commands in its header comment) |
| `build.py` | Builds `exfat_tool` against gbaSD's `fatfs/` with the app's `ffconf.h` (FF_USE_CHMOD turned on in the tool's copy only) |
| `mtools.py`, `bin/` | Stand-ins for `mkfs.fat`, `mkfs.vfat`, `mformat`, `fsck.fat`, `fsck.vfat`, `mmd`, `mcopy`, `mtype`, `mdir`, `mattrib`, `mdel`, `mren` |
| `exfat_read.py` | Independent exFAT reader (no FatFs code): folder listings, file contents, contiguous or FAT chain per object |
| `test_tools.py`, `run.sh` | Self-test: the same mtools steps on FAT16 (real mtools) and exFAT (stand-ins) must print the same |

## How an app's tests use it

With gbaSD as a submodule in `gbasd/` (paths below are from the app's root):

1. Build the tool against the app's FatFs configuration:

   ```sh
   python3 gbasd/tests/exfat_kit/build.py tests/exfat/build/exfat_tool --ffconf=include/ffconf.h
   ```

   (`--fatfs=DIR` builds against another FatFs copy, `--extra=file.c` links
   stubs it needs.)

2. Run the app's card tests with:

   - `PATH=<app>/gbasd/tests/exfat_kit/bin:$PATH` — the kit's `bin/` must
     come **first**, so its stand-ins are found before the real tools. Each
     stand-in hands over to the real tool of the same name (the next one on
     `PATH`, then `/usr/sbin`, `/sbin`) for a FAT card, so FAT tests are
     unaffected;
   - `EXFAT_TOOL=<path of the tool built in step 1>`;
   - `GBASD_FS=exfat`: `mkfs.fat` / `mkfs.vfat` / `mformat` then make exFAT
     cards. The apps' own names `GBAPLANNER_FS`, `GBAWRITER_FS`,
     `GBAVOCAB_FS`, `GBAREADER_FS`, `GBAMP3_FS` work too;
   - `EXFAT_FSCK=1` (recommended): every stand-in first runs `fsck.exfat -n`
     on the card, so a card the app damaged fails the test;
   - optional: `EXFAT_CLUSTER=bytes`, `EXFAT_FRAGMENT=1` and
     `EXFAT_FRAGMENT_MODE` (below), `EXFAT_LAYOUT_LOG=file`.

3. In Python tests, read cards independently with

   ```python
   sys.path.insert(0, 'gbasd/tests/exfat_kit')
   from exfat_read import Volume
   Volume('card.img').listdir('/app')   # [(name, is_dir, size, first_cluster, contiguous)]
   ```

`mtools.py`'s docstring lists every option and variable. Supported are the
options the apps' tests use; anything else stops with a message.

### Fragmented cards

`EXFAT_FRAGMENT=1` makes cards whose files and growing folders get FAT
chains. `EXFAT_FRAGMENT_MODE` chooses how (each is what one app did):

| Mode | After mkfs | App it comes from |
|---|---|---|
| `pairs` (default) | every other cluster of the free space taken (`EXFAT_FRAGMENT_MB`, 0 = all of it, or `EXFAT_FRAGMENT_PAIRS` cluster pairs; file kept: `EXFAT_FRAGMENT_FILE`, default `/FRAGMENT.BIN`) | gbaplanner; gbavocab with `EXFAT_FRAGMENT_PAIRS=4096 EXFAT_FRAGMENT_FILE=/.exfat-frag EXFAT_CLUSTER=512` |
| `files` | `EXFAT_FRAGMENT_CLUSTERS` (2048) one-cluster files in hidden `/.fragment`, every other one deleted (holes holding text) | gbawriter, with `EXFAT_CLUSTER=512` |
| `put` | nothing; every file `mcopy` puts is written in a FAT chain and leaves holes | gbareader |
| `none` | nothing | gbamp3 |

In every mode `mcopy -s` writes a folder's files a cluster in turn.

### Moving an app's tests/exfat to the kit

- Delete the app's `exfat_tool.c`, `mtools.py`, `exfat_read.py`, `bin/`;
  build the tool with `build.py`, point `PATH`, `EXFAT_TOOL` and imports at
  the kit as above.
- `exfat_tool` changes some apps will notice: `attr` prints letters
  (`ADSHR`, `-` when clear) and `chmod PATH [+-][ahrs]...` takes letters
  (gbaplanner's tool used hex; its `mtools.py` did the conversion, the
  kit's does the same); `mkdir` makes parents and accepts an existing folder
  (gbareader's failed; `mmd` checks that itself, like mtools); `cat` stays
  and `get PATH -` writes to stdout; `put` and `rm` take several paths. A
  missing path exits with 3.
- `mcopy` to an existing file without `-o` (or `-n`) fails, as gbareader's
  did; gbaplanner's overwrote.
- `fsck.fat` / `fsck.vfat` print `fsck.exfat`'s report and return its exit
  code (gbaplanner printed it on failure only, gbavocab exited with a
  message).
- The FatFs tests in `tests/fatfs/` take an app's configuration too:
  `FFCONF=include/ffconf.h sh gbasd/tests/fatfs/exfat_repair/run.sh`,
  `python3 gbasd/tests/fatfs/growth_cut.py --config=include/ffconf.h`,
  `python3 gbasd/tests/fatfs/stretch_cut.py --config=include/ffconf.h`.

## Self-test

`sh tests/exfat_kit/run.sh` (also run by `tests/fatfs/run.sh`, and in CI):
builds the tool against gbaSD's FatFs (`FFCONF=` for another ffconf.h) and
runs `test_tools.py` on a contiguous card and in every fragment mode. Needs
gcc, python3, mtools, dosfstools, exfatprogs; prints SKIP without them.

These are tests on card images, not on hardware.
