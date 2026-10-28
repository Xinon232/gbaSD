/* exFAT folder-stretch power cut (driven by tests/fatfs/stretch_cut.py; from
 * gbaplanner's tests/exfat, also in gbavocab and gbareader).
 * gbaSD's FatFs on a RAM copy of a card image made by mkfs.exfat:
 *   stretch_cut prep IN OUT DIR CLUSTERS LAYOUT
 *                                          LAYOUT fragmented: fragment the free
 *                                          space (every other cluster taken);
 *                                          contiguous: leave it; make /DIR, fill it with
 *                                          files until it has CLUSTERS clusters
 *   stretch_cut cut IN OUT DIR BUDGET      add files to /DIR until it grows by one
 *                                          cluster; the card takes only the first
 *                                          BUDGET sector writes (a power cut there),
 *                                          BUDGET -1: all, prints WRITES n
 *   stretch_cut after IN OUT DIR           "reboot": list /DIR (LIST lines), then
 *                                          add files until it grows by two more
 *                                          clusters, check them, list again (LIST2)
 * Exit 0 only when every FatFs call the step needs succeeds. */
#include "ff.h"
#include "diskio.h"
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
/* The folder's data length (exFAT: what the stream extension records). */
static long dir_size(void) {
	DIR d;
	if (f_opendir(&d, dir) != FR_OK) return -1;
	long n = (long)d.obj.objsize;
	f_closedir(&d);
	return n;
}
static int list(const char *tag) {
	DIR d;
	FILINFO i;
	FRESULT r = f_opendir(&d, dir);
	while (r == FR_OK && (r = f_readdir(&d, &i)) == FR_OK && i.fname[0])
		printf("%s %c %lu %s\n", tag, i.fattrib & AM_DIR ? 'D' : 'F', i.fattrib & AM_DIR ? 0ul : (unsigned long)i.fsize, i.fname);
	if (r == FR_OK) f_closedir(&d);
	return ok(r, "list folder");
}
/* A contiguous folder (no FAT chain) stays contiguous only while the cluster
   after it is free: then the files' data is put from the middle of the card
   on (FatFs looks for a new file's first cluster from fs.last_clst; a test
   setting, not something an app does). */
static int keep_contiguous(void) {
	DIR d;
	int c = 0;
	if (f_opendir(&d, dir) == FR_OK) { c = d.obj.stat == 2; f_closedir(&d); }
	return c;
}
/* One file: 600 bytes (two clusters on the test card), its name in it. */
static int add(int k) {
	char p[96], data[600];
	FIL f;
	UINT w;
	snprintf(p, sizeof p, "%s/stretch entry %03d.txt", dir, k);
	memset(data, 'a' + k % 26, sizeof data);
	memcpy(data, p, strlen(p));
	int contig = keep_contiguous();
	if (!ok(f_open(&f, p, FA_WRITE | FA_CREATE_NEW), "create")) return 0;
	if (contig) fs.last_clst = fs.n_fatent / 2;	/* after the folder took its next cluster (if it grew) */
	return ok(f_write(&f, data, sizeof data, &w), "write") &&
	       ok(w == sizeof data ? FR_OK : FR_DENIED, "write length") && ok(f_close(&f), "close");
}
static int read_back(int k) {
	char p[96], data[601];
	FIL f;
	UINT n = 0;
	snprintf(p, sizeof p, "%s/stretch entry %03d.txt", dir, k);
	if (!ok(f_open(&f, p, FA_READ), "open for read") || !ok(f_read(&f, data, sizeof data, &n), "read")) return 0;
	f_close(&f);
	return n == 600 && !memcmp(data, p, strlen(p)) && data[599] == 'a' + k % 26;
}
static int next_free(void) {
	char p[96];
	FILINFO i;
	int k = 0;
	for (;; ++k) {
		snprintf(p, sizeof p, "%s/stretch entry %03d.txt", dir, k);
		if (f_stat(p, &i) != FR_OK) return k;
	}
}
/* Add files until the folder has grown by `by` clusters; returns the next number or -1. */
static int grow(int by) {
	long start = dir_size(), cl = (long)fs.csize * 512;
	int k = next_free();
	if (start < 0) return -1;
	while (dir_size() < start + by * cl)
		if (!add(k++)) return -1;
	return k;
}

int main(int argc, char **argv) {
	if (argc < 5) return 2;
	load(argv[2]);
	snprintf(dir, sizeof dir, "/%s", argv[4]);
	if (!ok(f_mount(&fs, "0:", 1), "mount")) return 1;
	if (fs.fs_type != FS_EXFAT) { puts("FAIL not exFAT"); return 1; }
	if (!strcmp(argv[1], "prep") && argc == 7) {
		/* Every other cluster taken (two files growing in turn, one deleted). */
		FIL a, b;
		UINT w;
		char c[512];
		memset(c, 0, sizeof c);
		if (fs.csize != 1) { puts("FAIL cluster size is not 512 bytes"); return 1; }
		if (!strcmp(argv[6], "fragmented")) {
			if (!ok(f_open(&a, "/FRAGMENT.BIN", FA_WRITE | FA_CREATE_ALWAYS), "fragment") ||
			    !ok(f_open(&b, "/FRAGMENT.TMP", FA_WRITE | FA_CREATE_ALWAYS), "fragment")) return 1;
			for (;;) {
				if (f_write(&a, c, sizeof c, &w) != FR_OK || w != sizeof c) break;
				if (f_write(&b, c, sizeof c, &w) != FR_OK || w != sizeof c) break;
			}
			if (!ok(f_close(&a), "fragment") || !ok(f_close(&b), "fragment") || !ok(f_unlink("/FRAGMENT.TMP"), "fragment")) return 1;
		}
		if (!ok(f_mkdir(dir), "mkdir")) return 1;
		long want = atol(argv[5]) * 512;
		while (dir_size() < want)
			if (grow(1) < 0) return 1;
		f_mount(0, "0:", 0);
		save(argv[3]);
		return 0;
	}
	if (!strcmp(argv[1], "cut") && argc == 6) {
		budget = atol(argv[5]);
		int r = grow(1);
		f_mount(0, "0:", 0);
		save(argv[3]);
		if (budget < 0) printf("WRITES %ld\n", writes);
		return budget < 0 ? r < 0 : 0;	/* a cut save may fail; only the clean run must succeed */
	}
	if (!strcmp(argv[1], "after")) {
		if (!list("LIST")) return 1;
		int first = next_free(), last = grow(2);
		if (last < 0) return 1;
		f_mount(0, "0:", 0);
		if (!ok(f_mount(&fs, "0:", 1), "remount")) return 1;
		for (int k = first; k < last; ++k)
			if (!read_back(k)) { printf("FAIL stretch entry %03d not read back\n", k); return 1; }
		if (!list("LIST2")) return 1;
		f_mount(0, "0:", 0);
		save(argv[3]);
		return 0;
	}
	return 2;
}
