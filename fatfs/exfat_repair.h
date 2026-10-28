/* exfat_repair: repairs exFAT directory entry sets a power cut left half
 * written, so FatFs can read the folder again.
 *
 * ---- What it fixes ------------------------------------------------------
 * On exFAT every file / folder name is an entry set of 3 to 19 directory
 * entries (file + stream + name entries, 32 bytes each). FatFs writes a set
 * one sector at a time, so a power cut (or a failed write) between the two
 * sector writes of a set that crosses a sector boundary leaves a torn set.
 * FatFs then fails every lookup in that folder with FR_INT_ERR: nothing in
 * the folder can be opened or created until the card is repaired on a PC.
 * Rules (they mirror what FatFs' load_xdir() accepts):
 *   - a file entry whose later entries were never written (free): an
 *     unfinished create or rename (the old name is still there): the set is
 *     freed;
 *   - a complete set with a wrong checksum: an unfinished update of the
 *     stream entry (size / clusters): the checksum is made to match, so the
 *     file keeps its old data;
 *   - stream / name entries without their file entry: an unfinished delete:
 *     they are freed;
 *   - a set cut by the end of the folder (its data length) is freed, unless
 *     the folder is in a FAT chain and the rest of the set is written there
 *     (a FatFs R0.15 quirk on 512-byte clusters: then it is left alone);
 *   - anything else (sets FatFs did not write, other entry types, a sound
 *     folder) is left alone, byte for byte, and nothing is written.
 * Freeing clears bit 7 of the entry type (as a delete does). Clusters are
 * never touched. On FAT volumes it does nothing (8.3 names and LFN runs are
 * not sets; FatFs tolerates a broken LFN run).
 *
 * ---- When to call it ----------------------------------------------------
 *   1. Right after f_mount() succeeds, for the app's own folder(s), before
 *      anything else touches them (f_mkdir included):
 *          f_mount(&fs, "", 1);
 *          exfat_repair_dir(&fs, "/myapp/data", 1);  // "/", "/myapp", "/myapp/data"
 *   2. When an operation in such a folder returns FR_INT_ERR (a cut during
 *      this session): repair that folder (with its parents) and retry the
 *      operation once. Close every file of that folder first, or at least
 *      make sure none of them has unsynced changes.
 * Calling it on a sound folder is cheap (one read pass over the folder's
 * sectors, no writes) and safe to repeat.
 *
 * ---- API ----------------------------------------------------------------
 * int exfat_repair_dir(FATFS* fs, const TCHAR* path, int with_parents);
 *   path: a folder, as for f_opendir() ("/myapp", "0:/myapp", "/" for the
 *     root). with_parents != 0: the root and every folder on the way are
 *     repaired first (a torn set of a parent hides the folder), then the
 *     folder itself; with_parents == 0: the folder only.
 *   fs: the mounted volume, or NULL. Only used to return 0 at once when it
 *     is a FAT volume; the volume of `path` is the one repaired.
 *   Returns the number of repairs made (0: nothing to do; a folder that does
 *   not exist is not an error), or a negative FRESULT (-FR_DISK_ERR,
 *   -FR_INT_ERR when the folder still cannot be opened, -FR_INVALID_NAME when
 *   with_parents is set and the path is longer than EXFAT_REPAIR_PATH_MAX-1).
 *   A repair is one set freed, one checksum fixed, or one stray entry freed.
 *   After repairs the disk is synced (disk_ioctl CTRL_SYNC).
 *
 * ---- Cost ---------------------------------------------------------------
 *   No heap, no static or global data at all (nothing in .data/.bss/IWRAM).
 *   Uses fs->win (the volume's own sector buffer) for every sector it reads
 *   or writes: the FatFs window is written back first if dirty, and left
 *   invalid afterwards (FatFs re-reads it). Stack (arm-none-eabi-gcc -O2
 *   -mthumb, -fstack-usage): 192 bytes in the folder pass (the DIR object
 *   included) + 168 in exfat_repair_dir (EXFAT_REPAIR_PATH_MAX TCHARs of it
 *   for the path copy), plus f_opendir() (with FF_USE_LFN 2 FatFs puts its
 *   name buffers on the stack there, as for any f_open / f_stat). Code:
 *   about 1.9 KB of thumb, in ROM (plain functions, no section attributes:
 *   .text). With FF_FS_EXFAT 0 the function is a stub returning 0.
 *   Not reentrant; do not call it while another FatFs call runs (interrupt).
 *
 * ---- Copying it into an app ---------------------------------------------
 *   Copy exfat_repair.c and exfat_repair.h unchanged. Needs only FatFs
 *   R0.15 (ff.h, ffconf.h, diskio.h on the include path; it uses FATFS
 *   fields: win, winsect, wflag, fatbase, database, dirbase, csize, n_fatent,
 *   fsize, n_fats, pdrv, fs_type, and disk_read / disk_write / disk_ioctl).
 *   Plain C99, builds as C (gcc / arm-none-eabi-gcc) and is callable from
 *   C++ (extern "C" below). Put the .c in a source folder the build compiles
 *   (Butano: any SOURCES folder; it compiles .c files in ROM).
 *   Host test: tests/exfat_repair (run.sh builds it against the app's FatFs).
 */
#ifndef EXFAT_REPAIR_H
#define EXFAT_REPAIR_H

#include "ff.h"

#ifndef EXFAT_REPAIR_PATH_MAX
#define EXFAT_REPAIR_PATH_MAX 128 /* TCHARs, terminator included; stack, with_parents only */
#endif

#ifdef __cplusplus
extern "C" {
#endif

int exfat_repair_dir(FATFS* fs, const TCHAR* path, int with_parents);

#ifdef __cplusplus
}
#endif

#endif
