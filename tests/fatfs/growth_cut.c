/* Folder-growth power cut on FAT12 / FAT16 / FAT32 / exFAT (driven by
 * tests/fatfs/growth_cut.py). gbaSD's FatFs on a RAM copy of a card image
 * made by mkfs.fat / mkfs.exfat:
 *   growth_cut prep IN OUT DIR CLUSTERS MODE
 *       fill the free space with text and free it again (what a used card
 *       holds: old file data, not zeros), make /DIR, fill it with files with
 *       long names until it has CLUSTERS clusters and the next long name
 *       does not fit; MODE full: then fill the last free entries too (the new
 *       entry block starts in the new cluster), MODE partial: leave one entry
 *       fewer free than a long name needs (the block spans the old cluster's
 *       end and the new cluster)
 *   growth_cut cut IN OUT DIR BUDGET
 *       create one more file with a long name (the folder grows by a
 *       cluster); the card takes only the first BUDGET sector writes (a power
 *       cut there), BUDGET -1: all, prints WRITES n and GREW 1
 *   growth_cut repair IN OUT DIR
 *       "reboot" as the apps start: exfat_repair_dir(DIR, with parents),
 *       prints REPAIRED n (exFAT: an entry block the cut left half written;
 *       FAT: always 0, nothing written)
 *   growth_cut after IN OUT DIR
 *       "reboot": list /DIR (LIST lines; NEWFILE ok|empty), then add files
 *       until it has grown by two more clusters, read them back after a
 *       remount, list again (LIST2)
 * The folder's clusters are counted on the image itself (FAT chain or exFAT
 * data length), not through FatFs. Exit 0 only when every FatFs call the
 * step needs succeeds. */
#include "ff.h"
#include "diskio.h"
#include "exfat_repair.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static char dir[64];

static void load(const char *p) {
	FILE *f = fopen(p, "rb");
	if (!f || fseek(f, 0, SEEK_END)) exit(2);
	sectors = ftell(f) / 512;
	rewind(f);
	disk = malloc((size_t)sectors * 512);
	if (!disk || fread(disk, 512, (size_t)sectors, f) != (size_t)sectors) exit(2);
	fclose(f);
}
static void save(const char *p) {
	FILE *f = fopen(p, "wb");
	if (!f || fwrite(disk, 512, (size_t)sectors, f) != (size_t)sectors || fclose(f)) exit(2);
}
static int ok(FRESULT r, const char *what) {
	if (r != FR_OK) printf("FAIL %s: FRESULT %d\n", what, (int)r);
	return r == FR_OK;
}

/* The image as it is (FatFs has synced it: every step closes its files). */
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
/* The folder's clusters (at most 64) and the number of entries in use (up to the end marker). */
static int dir_clusters(DWORD *cl, long *used) {
	DIR d;
	int n = 0;
	if (f_opendir(&d, dir) != FR_OK) return -1;
	DWORD c = d.obj.sclust;
	long csz = (long)fs.csize * 512;
	if (fs.fs_type == FS_EXFAT) {
		long len = (long)d.obj.objsize;
		for (long at = 0; at < len; at += csz) {
			cl[n++] = c;
			c = d.obj.stat == 2 ? c + 1 : fat_entry(c);
		}
	} else {
		while (c >= 2 && c < fs.n_fatent && n < 64) { cl[n++] = c; c = fat_entry(c); }
	}
	f_closedir(&d);
	*used = 0;
	for (int k = 0; k < n; ++k) {
		const unsigned char *p = disk + ((size_t)fs.database + (size_t)(cl[k] - 2) * fs.csize) * 512;
		for (long e = 0; e < csz / 32; ++e, ++*used)
			if (p[e * 32] == 0) return n;
	}
	return n;
}
static long entries_per_cluster(void) { return (long)fs.csize * 512 / 32; }
/* Entries a name takes: FAT: its LFN entries + the SFN entry (a valid upper-case 8.3 name: 1); exFAT: file + stream + name entries. */
static long need(const char *name) {
	size_t l = strlen(name);
	if (fs.fs_type == FS_EXFAT) return 2 + (long)((l + 14) / 15);
	int sfn = l <= 12 && strspn(name, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.") == l;
	return sfn ? 1 : 1 + (long)((l + 12) / 13);
}
static long free_entries(int *ncl) {
	DWORD cl[64];
	long used;
	*ncl = dir_clusters(cl, &used);
	return *ncl * entries_per_cluster() - used;
}

static int put(const char *name, const char *text) {
	char p[160];
	FIL f;
	UINT w;
	snprintf(p, sizeof p, "%s/%s", dir, name);
	return ok(f_open(&f, p, FA_WRITE | FA_CREATE_NEW), "create") && ok(f_write(&f, text, (UINT)strlen(text), &w), "write") &&
	       ok(w == strlen(text) ? FR_OK : FR_DENIED, "write length") && ok(f_close(&f), "close");
}
static void long_name(char *out, size_t n, int k) { snprintf(out, n, "growth entry %03d with a long name.txt", k); }
#define NEW_NAME "the new entry that makes the folder grow.txt"
#define NEW_TEXT "written after the folder grew\n"

/* The free space full of text, then freed: a new folder cluster holds old data until it is cleared. */
static int dirty_free_space(void) {
	FIL f;
	UINT w;
	static char buf[32768];
	static const char text[] = "Old words from a deleted note. ";
	for (size_t i = 0; i < sizeof buf; ++i) buf[i] = text[i % (sizeof text - 1)];
	if (!ok(f_open(&f, "/OLDDATA.TXT", FA_WRITE | FA_CREATE_ALWAYS), "old data")) return 0;
	while (f_write(&f, buf, sizeof buf, &w) == FR_OK && w == sizeof buf) ;
	return ok(f_close(&f), "old data") && ok(f_unlink("/OLDDATA.TXT"), "old data");
}

static int list(const char *tag) {
	DIR d;
	FILINFO i;
	FRESULT r = f_opendir(&d, dir);
	while (r == FR_OK && (r = f_readdir(&d, &i)) == FR_OK && i.fname[0])
		printf("%s %s\n", tag, i.fname);
	if (r == FR_OK) f_closedir(&d);
	return ok(r, "list folder");
}

int main(int argc, char **argv) {
	if (argc < 5) return 2;
	load(argv[2]);
	snprintf(dir, sizeof dir, "/%s", argv[4]);
	if (!ok(f_mount(&fs, "0:", 1), "mount")) return 1;
	printf("FS %d CLUSTER %ld\n", fs.fs_type, (long)fs.csize * 512);
	if (!strcmp(argv[1], "prep") && argc == 7) {
		int want = atoi(argv[5]), partial = !strcmp(argv[6], "partial"), ncl, k = 0;
		char name[96];
		if (!dirty_free_space() || !ok(f_mkdir(dir), "mkdir")) return 1;
		long target = partial ? need(NEW_NAME) - 1 : 0, fr;
		/* exFAT sets take 3 entries or more: stop the long names while 3..7 entries are left to fill */
		long spare = fs.fs_type == FS_EXFAT ? 3 : 0;
		for (;;) {	/* long names while they fit or the folder is smaller than wanted */
			long_name(name, sizeof name, k);
			fr = free_entries(&ncl);
			if (ncl > want) { puts("FAIL folder overshot"); return 1; }
			if (ncl == want && fr - target < need(name) + spare) break;
			if (!put(name, "old entry\n")) return 1;
			++k;
		}
		fr = free_entries(&ncl);
		for (int s = 0; fr > target; ++s) {	/* the rest with the smallest entries that keep it exact */
			char small[40];
			long left = fr - target, len = 1;
			if (fs.fs_type == FS_EXFAT) {	/* entry sets of 3 (1-15 chars), 4 (16-30), 5 (31-45) entries */
				if (left < 3) break;
				long e = left == 4 || left == 7 ? 4 : left == 5 ? 5 : 3;
				len = e == 3 ? 6 : e == 4 ? 20 : 35;
				snprintf(small, sizeof small, "%0*d", (int)len, s);
			} else {
				snprintf(small, sizeof small, "S%04d.TXT", s);
			}
			if (!put(small, "old entry\n")) return 1;
			fr = free_entries(&ncl);
		}
		if (ncl != want || fr != target) { printf("FAIL folder layout: %d clusters, %ld entries free\n", ncl, fr); return 1; }
		printf("PREP clusters %d free %ld need %ld\n", ncl, fr, need(NEW_NAME));
		f_mount(0, "0:", 0);
		save(argv[3]);
		return 0;
	}
	if (!strcmp(argv[1], "cut") && argc == 6) {
		int before, after;
		free_entries(&before);
		budget = atol(argv[5]);
		int r = put(NEW_NAME, NEW_TEXT);
		if (budget < 0) {
			free_entries(&after);
			printf("WRITES %ld\nGREW %d\n", writes, after - before);
		}
		f_mount(0, "0:", 0);
		save(argv[3]);
		return budget < 0 ? !r : 0;	/* a cut save may fail; only the clean run must succeed */
	}
	if (!strcmp(argv[1], "repair")) {
		int n = exfat_repair_dir(&fs, dir, 1);
		printf("REPAIRED %d\n", n);
		if (n < 0) return 1;
		f_mount(0, "0:", 0);
		save(argv[3]);
		return 0;
	}
	if (!strcmp(argv[1], "after")) {
		char p[160], buf[64];
		FIL f;
		UINT n = 0;
		int ncl, start, k = 0;
		if (!list("LIST")) return 1;
		snprintf(p, sizeof p, "%s/%s", dir, NEW_NAME);
		if (f_open(&f, p, FA_READ) == FR_OK) {
			if (!ok(f_read(&f, buf, sizeof buf, &n), "read new file")) return 1;
			f_close(&f);
			printf("NEWFILE %s\n", n == 0 ? "empty" : n == strlen(NEW_TEXT) && !memcmp(buf, NEW_TEXT, n) ? "ok" : "BAD");
		}
		free_entries(&start);
		for (free_entries(&ncl); ncl < start + 2; free_entries(&ncl)) {
			snprintf(p, sizeof p, "added after the reboot %03d, a long name.txt", k++);
			if (!put(p, p)) return 1;
		}
		f_mount(0, "0:", 0);
		if (!ok(f_mount(&fs, "0:", 1), "remount")) return 1;
		for (int j = 0; j < k; ++j) {
			char q[260];
			snprintf(p, sizeof p, "added after the reboot %03d, a long name.txt", j);
			snprintf(q, sizeof q, "%s/%s", dir, p);
			if (!ok(f_open(&f, q, FA_READ), "open added") || !ok(f_read(&f, buf, sizeof buf, &n), "read added")) return 1;
			f_close(&f);
			if (n != strlen(p) || memcmp(buf, p, n)) { printf("FAIL %s not read back\n", p); return 1; }
		}
		if (!list("LIST2")) return 1;
		f_mount(0, "0:", 0);
		save(argv[3]);
		return 0;
	}
	return 2;
}
