/* Power cut while a file with a long name is deleted or renamed, on FAT12 /
 * FAT16 / FAT32 / exFAT (driven by tests/fatfs/remove_cut.py). gbaSD's FatFs
 * on a card image made by mkfs.fat / mkfs.exfat, changed in place (mmap: the
 * script copies the image first):
 *   remove_cut prep IMG LOC NAME OFFSET
 *       make /A and /B (with one file in /B), fill LOC (/, /A) with files
 *       with 8.3 names (one entry each) until the next entry is at OFFSET in
 *       its sector (0-15), then create LOC/NAME holding NAME as its text;
 *       prints BLOCK first last (the new file's entries, offsets in LOC)
 *   remove_cut cut IMG LOC OP OLD NEW BUDGET
 *       OP unlink: f_unlink(LOC/OLD); rename: f_rename(LOC/OLD, LOC/NEW);
 *       move: f_rename(LOC/OLD, /B/NEW). The card takes only the first
 *       BUDGET sector writes (a power cut there), BUDGET -1: all, prints
 *       WRITES n
 *   remove_cut repair IMG LOC
 *       "reboot" as the apps start: exfat_repair_dir on LOC and /B, prints
 *       REPAIRED n (exFAT: an entry set the cut left half written; FAT:
 *       always 0, nothing written)
 *   remove_cut after IMG LOC OLD NEW
 *       "reboot": list LOC and /B (LIST folder name), read LOC/OLD, LOC/NEW
 *       and /B/NEW where they exist (READ name ok|BAD), then create, rename
 *       and delete files with long names in LOC and /B, remount and list
 *       again (LIST2 folder name)
 * Exit 0 only when every FatFs call the step needs succeeds. */
#include "ff.h"
#include "diskio.h"
#include "exfat_repair.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char *disk;
static long sectors, budget = -1, writes;

DSTATUS disk_initialize(BYTE d) { return d ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE d) { return d ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) {
	if (d || (long)(s + n) > sectors) return RES_PARERR;
	memcpy(b, disk + (size_t)s * 512, (size_t)n * 512);
	return RES_OK;
}
DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) {
	if (d || (long)(s + n) > sectors) return RES_PARERR;
	for (UINT i = 0; i < n; ++i) {	/* sector by sector: a cut can land inside a multi-sector write */
		if (budget >= 0 && writes >= budget) return RES_ERROR;	/* the power is gone: nothing more reaches the card */
		memcpy(disk + ((size_t)s + i) * 512, b + (size_t)i * 512, 512);
		++writes;
	}
	return RES_OK;
}
DRESULT disk_ioctl(BYTE d, BYTE c, void *b) {
	if (d) return RES_PARERR;
	if (c == CTRL_SYNC) return RES_OK;
	if (c == GET_SECTOR_COUNT) { *(LBA_t *)b = (LBA_t)sectors; return RES_OK; }
	if (c == GET_SECTOR_SIZE) { *(WORD *)b = 512; return RES_OK; }
	if (c == GET_BLOCK_SIZE) { *(DWORD *)b = 1; return RES_OK; }
	return RES_PARERR;
}
DWORD get_fattime(void) { return (DWORD)(46 << 25 | 1 << 21 | 1 << 16); }

static FATFS fs;

static void map(const char *p) {
	int fd = open(p, O_RDWR);
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) exit(2);
	sectors = (long)(st.st_size / 512);
	disk = mmap(NULL, (size_t)sectors * 512, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (disk == MAP_FAILED) exit(2);
	close(fd);
}
static int ok(FRESULT r, const char *what) {
	if (r != FR_OK) printf("FAIL %s: FRESULT %d\n", what, (int)r);
	return r == FR_OK;
}
static void path(char *out, size_t n, const char *dir, const char *name) {
	snprintf(out, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

/* The image as it is (FatFs has synced it). */
static DWORD fat_entry(DWORD c) {
	const unsigned char *fat = disk + (size_t)fs.fatbase * 512;
	switch (fs.fs_type) {
	case FS_FAT12: { DWORD o = c + c / 2, v = fat[o] | fat[o + 1] << 8; v = c & 1 ? v >> 4 : v & 0xFFF; return v >= 0xFF8 ? 0xFFFFFFFF : v; }
	case FS_FAT16: { DWORD v = fat[2 * c] | fat[2 * c + 1] << 8; return v >= 0xFFF8 ? 0xFFFFFFFF : v; }
	default: { DWORD v = (fat[4 * c] | fat[4 * c + 1] << 8 | fat[4 * c + 2] << 16 | (DWORD)fat[4 * c + 3] << 24);
		if (fs.fs_type == FS_FAT32) { v &= 0x0FFFFFFF; return v >= 0x0FFFFFF8 ? 0xFFFFFFFF : v; }
		return v >= 0xFFFFFFF7 ? 0xFFFFFFFF : v; }
	}
}
/* Entries in use in folder dir, up to its end marker (read from the image, not through FatFs). */
static long used_entries(const char *dir) {
	DIR d;
	long used = 0, csz = (long)fs.csize * 512;
	if (f_opendir(&d, dir) != FR_OK) return -1;
	DWORD c = d.obj.sclust;
	int cont = fs.fs_type == FS_EXFAT && d.obj.sclust && d.obj.stat == 2;
	long len = fs.fs_type == FS_EXFAT && d.obj.sclust ? (long)d.obj.objsize : 0x7FFFFFFF;
	f_closedir(&d);
	if (!c && fs.fs_type <= FS_FAT16) {	/* FAT12/16 root: a fixed area */
		const unsigned char *p = disk + (size_t)fs.dirbase * 512;
		for (; used < fs.n_rootdir && p[used * 32]; ++used) ;
		return used;
	}
	if (!c) c = (DWORD)fs.dirbase;	/* FAT32 / exFAT root */
	for (long at = 0; at < len && c >= 2 && c < fs.n_fatent; at += csz) {
		const unsigned char *p = disk + ((size_t)fs.database + (size_t)(c - 2) * fs.csize) * 512;
		for (long e = 0; e < csz / 32; ++e, ++used)
			if (p[e * 32] == 0) return used;
		c = cont ? c + 1 : fat_entry(c);
	}
	return used;
}
/* Entries a name takes: FAT: its LFN entries + the SFN entry (an upper-case 8.3 name: 1); exFAT: file + stream + name entries. */
static long need(const char *name) {
	size_t l = strlen(name);
	if (fs.fs_type == FS_EXFAT) return 2 + (long)((l + 14) / 15);
	int sfn = l <= 12 && strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.") == l;
	return sfn ? 1 : 1 + (long)((l + 12) / 13);
}

static int put(const char *dir, const char *name, const char *text) {
	char p[600];
	FIL f;
	UINT w;
	path(p, sizeof p, dir, name);
	return ok(f_open(&f, p, FA_WRITE | FA_CREATE_NEW), "create") && ok(f_write(&f, text, (UINT)strlen(text), &w), "write") &&
	       ok(w == strlen(text) ? FR_OK : FR_DENIED, "write length") && ok(f_close(&f), "close");
}
static int list(const char *tag, const char *dir) {
	DIR d;
	FILINFO i;
	FRESULT r = f_opendir(&d, dir);
	while (r == FR_OK && (r = f_readdir(&d, &i)) == FR_OK && i.fname[0])
		printf("%s %s %s\n", tag, dir, i.fname);
	if (r == FR_OK) f_closedir(&d);
	return ok(r, "list folder");
}
/* LOC/name or /B/name: READ name ok|BAD when it exists (its text: the name it was created with). */
static int check_read(const char *dir, const char *name, const char *text) {
	char p[600], buf[600];
	FIL f;
	UINT n = 0;
	path(p, sizeof p, dir, name);
	if (f_open(&f, p, FA_READ) != FR_OK) return 1;
	if (!ok(f_read(&f, buf, sizeof buf, &n), "read")) return 0;
	f_close(&f);
	printf("READ %s %s\n", p, n == strlen(text) && !memcmp(buf, text, n) ? "ok" : "BAD");
	return 1;
}

int main(int argc, char **argv) {
	if (argc < 4) return 2;
	map(argv[2]);
	const char *loc = argv[3];
	if (!ok(f_mount(&fs, "0:", 1), "mount")) return 1;
	if (!strcmp(argv[1], "prep") && argc == 6) {
		const char *name = argv[4];
		long off = atol(argv[5]), used;
		if (!ok(f_mkdir("/A"), "mkdir A") || !ok(f_mkdir("/B"), "mkdir B") ||
		    !put("/B", "a file that stays in folder B.txt", "stays\n")) return 1;
		for (int k = 0; (used = used_entries(loc)) % 16 != off; ++k) {
			char small[24];
			snprintf(small, sizeof small, "P%03d.TXT", k);
			if (used < 0 || k > 40 || !put(loc, small, "padding\n")) return 1;
		}
		if (!put(loc, name, name)) return 1;
		long first = used, last = used + need(name) - 1;
		if (used_entries(loc) != last + 1) { puts("FAIL entry count"); return 1; }
		printf("BLOCK %ld %ld\n", first, last);
		f_mount(0, "0:", 0);
		return 0;
	}
	if (!strcmp(argv[1], "cut") && argc == 8) {
		char po[600], pn[600];
		FRESULT r;
		path(po, sizeof po, loc, argv[5]);
		path(pn, sizeof pn, !strcmp(argv[4], "move") ? "/B" : loc, argv[6]);
		budget = atol(argv[7]);
		r = !strcmp(argv[4], "unlink") ? f_unlink(po) : f_rename(po, pn);
		if (budget < 0) printf("WRITES %ld\n", writes);
		f_mount(0, "0:", 0);
		return budget < 0 ? !ok(r, argv[4]) : 0;	/* a cut operation may fail; only the clean run must succeed */
	}
	if (!strcmp(argv[1], "repair")) {
		int n = exfat_repair_dir(&fs, loc, 1), m = exfat_repair_dir(&fs, "/B", 1);
		printf("REPAIRED %d\n", n < 0 || m < 0 ? -1 : n + m);
		f_mount(0, "0:", 0);
		return n < 0 || m < 0;
	}
	if (!strcmp(argv[1], "after") && argc == 6) {
		const char *old = argv[4], *nw = argv[5];
		char p[600], q[600];
		if (!list("LIST", loc) || !list("LIST", "/B")) return 1;
		if (!check_read(loc, old, old) || !check_read(loc, nw, old) || !check_read("/B", nw, old)) return 1;
		/* FatFs goes on working in both folders: create, rename, delete (long names) */
		if (!put(loc, "created after the reboot, with a long name.txt", "new\n") ||
		    !put("/B", "another new file after the reboot.txt", "new\n")) return 1;
		path(p, sizeof p, loc, "created after the reboot, with a long name.txt");
		path(q, sizeof q, loc, "renamed after the reboot, a long name too.txt");
		if (!ok(f_rename(p, q), "rename after")) return 1;
		if (!ok(f_unlink("/B/another new file after the reboot.txt"), "unlink after")) return 1;
		f_mount(0, "0:", 0);
		if (!ok(f_mount(&fs, "0:", 1), "remount") || !list("LIST2", loc) || !list("LIST2", "/B")) return 1;
		if (!check_read(loc, "renamed after the reboot, a long name too.txt", "new\n")) return 1;
		f_mount(0, "0:", 0);
		return 0;
	}
	return 2;
}
