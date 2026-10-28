/* Host test of exfat_repair (gbaSD fatfs/exfat_repair.c; written for
 * gbaplanner, with gbamp3's change for its exFAT lookup), self-contained:
 * the FatFs under test (with FF_USE_MKFS on, see run.sh) on a RAM disk.
 *
 * Every case builds the card byte-exactly: a folder filled so that the
 * target entry set starts at a chosen entry (the set crosses a sector
 * boundary, at every offset; sets of 3, 4, 10 and 19 entries, names up to
 * 255 characters), then one FatFs operation (create, append, rewrite,
 * delete, rename, mkdir) with every sector write recorded. For every power
 * cut point k (the card before the operation plus its first k writes) the
 * test mounts the card, runs exfat_repair_dir() and checks:
 *   - it returns >= 0; with 0 it wrote nothing; every byte it changed is a
 *     type byte losing bit 7 (an entry freed) or a set checksum;
 *   - after a repair the card is fsck.exfat clean, a file can be created in
 *     the folder (fsck clean again) and a second repair changes nothing;
 *   - the folder lists to its end; every file that existed before reads
 *     back exactly; the target is in one of the states the cut allows.
 * Cases where a repair happened are counted per operation (each must have
 * some). Also: the folder's own set torn in the root (repaired with
 * with_parents, not without), the folder in FAT chains (512-byte clusters,
 * a file taking the cluster after it) and contiguous (4 KB clusters),
 * healthy folders and FAT12/16/FAT32 volumes left byte-identical with no
 * writes, path forms, read errors.
 *
 *   tests/fatfs/exfat_repair/run.sh   (needs gcc and fsck.exfat from exfatprogs)
 */
#include "exfat_repair.h"
#include "diskio.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef GBASD_FF_FAST_EXFAT_LOOKUP /* a FatFs without gbaSD's switches (an app's own copy) */
#define GBASD_FF_FAST_EXFAT_LOOKUP 0
#endif

#define SS 512
static BYTE* img;
static DWORD img_sectors;
static unsigned char* dirty; /* sectors written since the last snapshot / restore */

/* Recording of writes */
typedef struct {
  LBA_t sect;
  BYTE data[SS];
  BYTE old[SS];
} wrec;
static wrec* recs;
static int nrec, caprec, recording;
static long writes, fail_read_at = -1, reads;

static void rec_push(LBA_t s, const BYTE* d) {
  if (nrec == caprec) {
    caprec = caprec ? caprec * 2 : 64;
    recs = realloc(recs, sizeof *recs * (size_t)caprec);
  }
  recs[nrec].sect = s;
  memcpy(recs[nrec].data, d, SS);
  memcpy(recs[nrec].old, img + (size_t)s * SS, SS);
  ++nrec;
}

DSTATUS disk_status(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_initialize(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  if (pdrv || sector + count > img_sectors) return RES_PARERR;
  if (fail_read_at >= 0 && reads++ >= fail_read_at) return RES_ERROR;
  memcpy(buff, img + (size_t)sector * SS, (size_t)count * SS);
  return RES_OK;
}
DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  UINT i;
  if (pdrv || sector + count > img_sectors) return RES_PARERR;
  for (i = 0; i < count; ++i) {
    if (recording) rec_push(sector + i, buff + (size_t)i * SS);
    memcpy(img + (size_t)(sector + i) * SS, buff + (size_t)i * SS, SS);
    dirty[sector + i] = 1;
    ++writes;
  }
  return RES_OK;
}
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  if (pdrv) return RES_PARERR;
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*)buff = img_sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)buff = SS; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)buff = 1; return RES_OK;
    default: return RES_PARERR;
  }
}
DWORD get_fattime(void) { return ((DWORD)(2026 - 1980) << 25) | (10u << 21) | (28u << 16); }

/* ---- helpers ------------------------------------------------------------ */
static FATFS fs;
static const char* tmpdir;
static long checks;

#define FAIL(...)                                    \
  do {                                               \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__);                    \
    fprintf(stderr, "  [case %s]\n", ctx);           \
    exit(1);                                         \
  } while (0)
#define CHECK(c)                       \
  do {                                 \
    ++checks;                          \
    if (!(c)) FAIL("%s", #c);          \
  } while (0)
static char ctx[200] = "setup";

static void mount(void) {
  f_mount(0, "", 0);
  if (f_mount(&fs, "", 1) != FR_OK) FAIL("mount");
}

static void format(BYTE fmt, DWORD mb, DWORD au) {
  static BYTE work[32768];
  MKFS_PARM opt;
  opt.fmt = (BYTE)(fmt | FM_SFD);
  opt.n_fat = 0;
  opt.align = 0;
  opt.n_root = 0;
  opt.au_size = au;
  free(img);
  free(dirty);
  img_sectors = mb * 2048;
  img = calloc(img_sectors, SS);
  dirty = calloc(img_sectors, 1);
  f_mount(0, "", 0);
  if (f_mkfs("", &opt, work, sizeof work) != FR_OK) FAIL("f_mkfs");
  mount();
}

static BYTE* snapshot(void) {
  BYTE* s = malloc((size_t)img_sectors * SS);
  memcpy(s, img, (size_t)img_sectors * SS);
  memset(dirty, 0, img_sectors);
  return s;
}
static void restore(const BYTE* s) {
  DWORD i;
  for (i = 0; i < img_sectors; ++i)
    if (dirty[i]) {
      memcpy(img + (size_t)i * SS, s + (size_t)i * SS, SS);
      dirty[i] = 0;
    }
}

/* fsck.exfat (exfatprogs 1.2.2) reads an entry set from at most two
   clusters: a set across three (512-byte clusters, 18-19 entries) is
   reported as a wrong checksum plus unknown entries, although it is valid
   (exfat_read.py and FatFs read it). fsck_clean() accepts exactly those
   reports, at the entries of such sets (found by sets3()), nothing else. */
static size_t allowed[4096];
static int nallowed;
static long fsck_tolerated;
static void sets3(void);

static int fsck_clean(void) {
  char path[600], cmd[1400], line[512];
  FILE* f;
  int rc, bad = 0, seen = 0;
  snprintf(path, sizeof path, "%s/fsck.img", tmpdir);
  f = fopen(path, "wb");
  if (!f || fwrite(img, SS, img_sectors, f) != img_sectors) FAIL("write %s", path);
  fclose(f);
  snprintf(cmd, sizeof cmd, "fsck.exfat -n '%s' > '%s/fsck.log' 2>&1", path, tmpdir);
  rc = system(cmd);
  if (!rc) return 1;
  nallowed = 0;
  sets3();
  snprintf(path, sizeof path, "%s/fsck.log", tmpdir);
  f = fopen(path, "r");
  while (f && fgets(line, sizeof line, f)) {
    const char* at;
    unsigned long off;
    int k, ok = 0;
    if (strncmp(line, "ERROR:", 6)) continue;
    at = strrchr(line, 'x');
    if ((strstr(line, "the checksum of a file is wrong at 0x") || strstr(line, ": unknown entry type 0xc")) && at &&
        sscanf(at + 1, "%lx", &off) == 1)
      for (k = 0; k < nallowed; ++k) ok |= allowed[k] == off;
    if (ok) ++seen;
    else bad = 1;
  }
  if (f) fclose(f);
  if (!bad && seen) {
    ++fsck_tolerated;
    return 1;
  }
  snprintf(cmd, sizeof cmd, "cat '%s/fsck.log' >&2", tmpdir);
  if (system(cmd)) {}
  return 0;
}

/* Content of file number n: deterministic, 1..3000 bytes, some bigger than
   a 512-byte cluster. */
static int content(int n, BYTE* out) {
  int len = (n * 397) % 3000 + 1, i;
  for (i = 0; i < len; ++i) out[i] = (BYTE)(n * 31 + i * 7);
  return len;
}

static void put(const char* path, const BYTE* d, int len) {
  FIL f;
  UINT bw;
  FRESULT r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
  if (r != FR_OK) FAIL("create %s: %d", path, (int)r);
  if (len && (f_write(&f, d, (UINT)len, &bw) != FR_OK || bw != (UINT)len)) FAIL("write %s", path);
  if (f_close(&f) != FR_OK) FAIL("close %s", path);
}
/* -1: no such file (other errors fail) */
static int get(const char* path, BYTE* out, int cap) {
  FIL f;
  UINT br;
  FRESULT r = f_open(&f, path, FA_READ);
  if (r == FR_NO_FILE) return -1;
  if (r != FR_OK) FAIL("open %s: %d", path, (int)r);
  if (f_size(&f) > (FSIZE_t)cap) FAIL("size of %s", path);
  if (f_read(&f, out, (UINT)cap, &br) != FR_OK) FAIL("read %s", path);
  f_close(&f);
  return (int)br;
}

/* Lists a folder to its end; returns the number of names. */
static int list(const char* dir) {
  DIR d;
  FILINFO fi;
  int n = 0;
  FRESULT r = f_opendir(&d, dir);
  if (r != FR_OK) FAIL("opendir %s: %d", dir, (int)r);
  for (;;) {
    r = f_readdir(&d, &fi);
    if (r != FR_OK) FAIL("readdir %s: %d", dir, (int)r);
    if (!fi.fname[0]) break;
    ++n;
  }
  f_closedir(&d);
  return n;
}

/* The entry index of `name`'s set in `dir` (f_readdir's blk_ofs), -1 if none. */
static long entry_of(const char* dir, const char* name) {
  DIR d;
  FILINFO fi;
  long at = -1;
  if (f_opendir(&d, dir) != FR_OK) FAIL("opendir %s", dir);
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
    if (!strcmp(fi.fname, name)) at = (long)(d.blk_ofs / 32);
  f_closedir(&d);
  return at;
}

/* Byte offset in the image of entry `idx` of folder `dir`, 0 past its end
   (raw: a folder in a FAT chain ends where its chain does, a contiguous one
   at its data length). */
static size_t entry_pos(const char* dir, long idx) {
  DIR d;
  DWORD clst, csz;
  int contig;
  long per;
  if (f_opendir(&d, dir) != FR_OK) FAIL("opendir %s", dir);
  clst = d.obj.sclust;
  contig = (d.obj.stat & 2) != 0;
  if (contig && (FSIZE_t)idx * 32 >= d.obj.objsize) { /* no FAT chain: the folder ends at its data length */
    f_closedir(&d);
    return 0;
  }
  if (!clst) {
    clst = fs.dirbase;
    contig = 0;
  }
  f_closedir(&d);
  csz = fs.csize;
  per = (long)csz * SS / 32;
  while (idx >= per) {
    if (contig) ++clst;
    else {
      const BYTE* p = img + (size_t)fs.fatbase * SS + (size_t)clst * 4;
      clst = (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24;
    }
    if (clst < 2 || clst >= fs.n_fatent) return 0;
    idx -= per;
  }
  return ((size_t)fs.database + (size_t)(clst - 2) * csz) * SS + (size_t)idx * 32;
}
/* The first entry of `dir` with type 0 (raw). */
static long first_unused(const char* dir) {
  long i;
  for (i = 0;; ++i) {
    size_t p = entry_pos(dir, i);
    if (!p || img[p] == 0) return i;
  }
}

/* The image offsets of every entry of each set spanning three clusters, in
   every folder (fsck_clean()). */
static void sets3_in(const char* dir) {
  DIR d;
  FILINFO fi;
  long i;
  char sub[600];
  for (i = 0;; ++i) {
    const size_t p = entry_pos(dir, i);
    if (!p || img[p] == 0) break;
    if (img[p] == 0x85) {
      const int n = img[p + 1] + 1;
      const size_t clus = (size_t)fs.csize * SS;
      size_t last = entry_pos(dir, i + n - 1);
      int k, spans = 1;
      size_t prev = p / clus;
      for (k = 1; k < n && last; ++k) {
        const size_t q = entry_pos(dir, i + k);
        if (q / clus != prev) ++spans;
        prev = q / clus;
      }
      if (spans >= 3)
        for (k = 0; k < n && nallowed < 4096; ++k) allowed[nallowed++] = entry_pos(dir, i + k);
    }
  }
  if (f_opendir(&d, dir) != FR_OK) return;
  while (f_readdir(&d, &fi) == FR_OK && fi.fname[0])
    if (fi.fattrib & AM_DIR) {
      snprintf(sub, sizeof sub, "%s/%s", strcmp(dir, "/") ? dir : "", fi.fname);
      sets3_in(sub);
    }
  f_closedir(&d);
}
static void sets3(void) { sets3_in("/"); }

/* A name of `len` characters: prefix, then filler letters. */
static void make_name(char* out, const char* prefix, int len) {
  int i, n = (int)strlen(prefix);
  for (i = 0; i < len; ++i) out[i] = i < n ? prefix[i] : (char)('a' + (i * 7) % 26);
  out[len] = 0;
}
static int name_len_for(int entries, int variant) { /* entries: 3..19 */
  int hi = 15 * (entries - 2), lo = hi - 14;
  return variant ? lo : hi;
}

/* Files the case knows (that existed before the operation). */
typedef struct {
  char path[600];
  int n;
} known_t;
static known_t known[64];
static int nknown;

static void add_file(const char* dir, const char* name, int n) {
  BYTE buf[3000];
  known_t* k = &known[nknown++];
  snprintf(k->path, sizeof k->path, "%s/%s", dir, name);
  put(k->path, buf, content(n, buf));
  k->n = n;
}

/* Fills `dir` with filler files until its next free entry is `s`. */
static void fill_to(const char* dir, long s) {
  long next = first_unused(dir);
  int f = 0;
  while (next < s) {
    long need = s - next;
    int z = need > 19 ? (need - 19 >= 3 ? 19 : (int)need - 3) : (int)need;
    char name[300], pre[16];
    if (z < 3) FAIL("cannot place a set at entry %ld (next free %ld)", s, next);
    snprintf(pre, sizeof pre, "f%d_", f);
    make_name(name, pre, name_len_for(z, f & 1));
    add_file(dir, name, 100 + f);
    ++f;
    next = first_unused(dir);
  }
  if (next != s) FAIL("fill: next free %ld, wanted %ld", next, s);
}

/* ---- the operations ------------------------------------------------------ */
enum { OP_CREATE, OP_APPEND, OP_REWRITE, OP_DELETE, OP_RENAME, OP_MKDIR, OP_N };
static const char* op_name[OP_N] = {"create", "append", "rewrite", "delete", "rename", "mkdir"};
static long torn_cases[OP_N], cut_points;

static char tpath[600], tpath2[600];
static BYTE told[3000], tnew[6000];
static int told_len, tnew_len;

static FRESULT do_op(int op) {
  FIL f;
  UINT bw;
  FRESULT r;
  switch (op) {
    case OP_CREATE:
    case OP_REWRITE:
      if ((r = f_open(&f, tpath, FA_WRITE | FA_CREATE_ALWAYS)) != FR_OK) return r;
      if (f_write(&f, tnew, (UINT)tnew_len, &bw) != FR_OK || f_close(&f) != FR_OK) FAIL("write");
      break;
    case OP_APPEND:
      if (f_open(&f, tpath, FA_WRITE | FA_OPEN_APPEND) != FR_OK) FAIL("append open");
      if (f_write(&f, tnew + told_len, (UINT)(tnew_len - told_len), &bw) != FR_OK) FAIL("append");
      if (f_close(&f) != FR_OK) FAIL("append close");
      break;
    case OP_DELETE:
      if (f_unlink(tpath) != FR_OK) FAIL("unlink");
      break;
    case OP_RENAME:
      return f_rename(tpath, tpath2);
    case OP_MKDIR: return f_mkdir(tpath);
  }
  return FR_OK;
}

/* Repairs `dir` (with parents) and checks the bytes it changed. */
static int repair_checked(const char* dir) {
  int r, i, j;
  nrec = 0;
  recording = 1;
  r = exfat_repair_dir(&fs, dir, 1);
  recording = 0;
  if (r < 0) FAIL("exfat_repair_dir(%s) = %d", dir, r);
  if (r == 0 && nrec) FAIL("no repair reported, %d sectors written", nrec);
  if (r > 0 && !nrec) FAIL("%d repairs reported, nothing written", r);
  for (i = 0; i < nrec; ++i)
    for (j = 0; j < SS; ++j) {
      const BYTE o = recs[i].old[j], n = recs[i].data[j];
      if (o == n) continue;
      if (j % 32 == 0 && n == (o & 0x7F)) continue;  /* an entry freed */
      if (j % 32 == 2 || j % 32 == 3) continue;      /* a set checksum */
      FAIL("repair changed sector %lu byte %d: %02x -> %02x", (unsigned long)recs[i].sect, j, o, n);
    }
  return r;
}

static void check_known(int skip_target) {
  BYTE buf[6000], want[3000];
  int i;
  for (i = 0; i < nknown; ++i) {
    int len;
    if (skip_target && !strcmp(known[i].path, tpath)) continue;
    len = get(known[i].path, buf, sizeof buf);
    if (len != content(known[i].n, want) || memcmp(buf, want, (size_t)len)) FAIL("%s lost or changed", known[i].path);
  }
}

static int same(const BYTE* a, int alen, const BYTE* b, int blen) { return alen == blen && !memcmp(a, b, (size_t)alen); }

static void check_target(int op) {
  BYTE buf[6000];
  int len, len2;
  DIR d;
  FRESULT r;
  switch (op) {
    case OP_CREATE:
      len = get(tpath, buf, sizeof buf);
      CHECK(len == -1 || len == 0 || same(buf, len, tnew, tnew_len));
      break;
    case OP_APPEND:
      len = get(tpath, buf, sizeof buf);
      CHECK(same(buf, len, told, told_len) || same(buf, len, tnew, tnew_len));
      break;
    case OP_REWRITE:
      len = get(tpath, buf, sizeof buf);
      CHECK(len == 0 || same(buf, len, told, told_len) || same(buf, len, tnew, tnew_len));
      break;
    case OP_DELETE:
      len = get(tpath, buf, sizeof buf);
      CHECK(len == -1 || same(buf, len, told, told_len));
      break;
    case OP_RENAME:
      len = get(tpath, buf, sizeof buf);
      CHECK(len == -1 || same(buf, len, told, told_len));
      len2 = get(tpath2, buf, sizeof buf);
      CHECK(len2 == -1 || same(buf, len2, told, told_len));
      CHECK(len != -1 || len2 != -1);
      break;
    case OP_MKDIR:
      r = f_opendir(&d, tpath);
      CHECK(r == FR_OK || r == FR_NO_PATH);
      if (r == FR_OK) {
        f_closedir(&d);
        CHECK(list(tpath) == 0);
      }
      break;
  }
}

/* One case: the folder `dir` (cluster size au), the target set of `entries`
   entries starting at entry `s`, operation op. */
static void run_case(DWORD au, const char* dir, int entries, long s, int op) {
  char name[260], name2[260];
  BYTE* before;
  wrec* ops;
  int nops, k, i;
  long at, torn_here = 0;
  const int is_root_dir = !strcmp(dir, "");

  snprintf(ctx, sizeof ctx, "au %lu, %s%s, %d-entry set at entry %ld, %s", (unsigned long)au, dir[0] ? dir : "/",
           "", entries, s, op_name[op]);
  format(FM_EXFAT, 4, au);
  nknown = 0;
  if (!is_root_dir) {
    if (f_mkdir(dir) != FR_OK) FAIL("mkdir %s", dir);
    if (au == 512) add_file("", "PAD", 1); /* takes the cluster after the folder: it grows in a FAT chain */
  }
  fill_to(is_root_dir ? "/" : dir, s);
  make_name(name, "T_", name_len_for(entries, s & 1));
  if (op == OP_CREATE || op == OP_MKDIR) name[0] = 'N';
  snprintf(tpath, sizeof tpath, "%s/%s", dir, name);
  make_name(name2, "R_", name_len_for(entries == 19 ? 3 : 19, 0));
  snprintf(tpath2, sizeof tpath2, "%s/%s", dir, name2);
  told_len = content(7, told);
  memcpy(tnew, told, (size_t)told_len);
  for (i = told_len; i < told_len + 2500; ++i) tnew[i] = (BYTE)(i * 13);
  tnew_len = told_len + 2500;
  if (op == OP_REWRITE) {
    tnew_len = 700;
    for (i = 0; i < tnew_len; ++i) tnew[i] = (BYTE)(i * 3 + 1);
  }
  if (op != OP_CREATE && op != OP_MKDIR) {
    FIL f;
    UINT bw;
    const FRESULT r = f_open(&f, tpath, FA_WRITE | FA_CREATE_ALWAYS);
    if (r != FR_OK || f_write(&f, told, (UINT)told_len, &bw) != FR_OK || f_close(&f) != FR_OK) FAIL("target");
    at = entry_of(is_root_dir ? "/" : dir, name);
    if (at != s) FAIL("target at entry %ld", at);
    add_file(dir, "AFTER.BIN", 3); /* an entry set after the target */
  }
  /* A sound folder: no repair, no write. */
  writes = 0;
  CHECK(exfat_repair_dir(&fs, is_root_dir ? "/" : dir, 1) == 0 && writes == 0);
  before = snapshot();
  /* The operation, its writes recorded. */
  nrec = 0;
  recording = 1;
  {
    const FRESULT r = do_op(op);
    recording = 0;
    if (r != FR_OK) FAIL("%s: %d", op_name[op], (int)r);
  }
  if (op == OP_CREATE || op == OP_MKDIR) {
    at = entry_of(is_root_dir ? "/" : dir, name);
    if (at != s) FAIL("new set at entry %ld", at);
  }
  nops = nrec;
  ops = malloc(sizeof *ops * (size_t)(nops ? nops : 1));
  memcpy(ops, recs, sizeof *ops * (size_t)nops);
  for (k = 0; k <= nops; ++k) {
    int r;
    char probe[700];
    restore(before);
    for (i = 0; i < k; ++i) {
      memcpy(img + (size_t)ops[i].sect * SS, ops[i].data, SS);
      dirty[ops[i].sect] = 1;
    }
    mount();
    ++cut_points;
    snprintf(ctx, sizeof ctx, "au %lu, %s, %d-entry set at entry %ld, %s, cut after %d of %d writes",
             (unsigned long)au, dir[0] ? dir : "/", entries, s, op_name[op], k, nops);
    r = repair_checked(is_root_dir ? "/" : dir);
    if (r > 0) {
      ++torn_here;
      if (!fsck_clean()) FAIL("fsck.exfat after the repair");
    }
    CHECK(list("/") >= 0);
    if (!is_root_dir) CHECK(list(dir) >= 0);
    check_known(1);
    check_target(op);
    /* nothing left: a second repair changes nothing */
    writes = 0;
    CHECK(exfat_repair_dir(&fs, is_root_dir ? "/" : dir, 1) == 0 && writes == 0);
    if (r > 0) {
      BYTE b[3000];
      snprintf(probe, sizeof probe, "%s/PROBE.TXT", dir);
      put(probe, told, told_len);
      CHECK(get(probe, b, sizeof b) == told_len && !memcmp(b, told, (size_t)told_len));
      check_known(1);
      if (!fsck_clean()) FAIL("fsck.exfat after a create in the repaired folder");
    }
  }
  if (torn_here) ++torn_cases[op];
  free(ops);
  free(before);
}

/* ---- other cases ------------------------------------------------------------ */
static void healthy_and_fat(void) {
  static const BYTE fmts[3] = {FM_FAT, FM_FAT, FM_FAT32};
  static const DWORD mbs[3] = {4, 16, 40};
  int v, i;
  /* exFAT: many sets (long names across two / three sectors), holes. */
  for (v = 0; v < 2; ++v) {
    BYTE* snap;
    snprintf(ctx, sizeof ctx, "healthy exFAT folder, %d-byte clusters", v ? 4096 : 512);
    format(FM_EXFAT, 4, v ? 4096 : 512);
    nknown = 0;
    f_mkdir("/APP");
    f_mkdir("/APP/SUB"); /* /APP grows in a FAT chain */
    for (i = 0; i < 40; ++i) {
      char name[300], pre[16];
      snprintf(pre, sizeof pre, "h%d_", i);
      make_name(name, pre, 1 + (i * 37) % 255);
      add_file("/APP", name, i);
      if (i % 4 == 1) f_unlink(known[--nknown].path);
    }
    snap = snapshot();
    writes = 0;
    CHECK(exfat_repair_dir(&fs, "/APP/SUB", 1) == 0 && exfat_repair_dir(&fs, "/APP", 0) == 0 && writes == 0);
    CHECK(!memcmp(snap, img, (size_t)img_sectors * SS));
    CHECK(fsck_clean());
    check_known(0);
    free(snap);
  }
  /* Path forms. */
  CHECK(exfat_repair_dir(0, "/", 1) == 0 && exfat_repair_dir(0, "", 1) == 0 && exfat_repair_dir(0, "0:/APP", 1) == 0);
  CHECK(exfat_repair_dir(0, "/APP/", 1) == 0 && exfat_repair_dir(0, "APP//SUB", 1) == 0 && exfat_repair_dir(0, "/", 0) == 0);
  CHECK(exfat_repair_dir(0, "/NOPE/NOPE", 1) == 0 && exfat_repair_dir(0, known[0].path, 1) == 0);
  {
    char longp[EXFAT_REPAIR_PATH_MAX + 8];
    memset(longp, 'a', sizeof longp);
    longp[0] = '/';
    longp[sizeof longp - 1] = 0;
    CHECK(exfat_repair_dir(0, longp, 1) == -(int)FR_INVALID_NAME);
    CHECK(exfat_repair_dir(0, longp, 0) == 0); /* without parents: no copy */
  }
  /* Not mounted yet (f_mount with opt 0): the call mounts it. */
  f_mount(0, "", 0);
  f_mount(&fs, "", 0);
  CHECK(exfat_repair_dir(&fs, "/APP", 1) == 0);
  /* A read error: a negative FRESULT. */
  mount();
  reads = 0;
  fail_read_at = 1;
  {
    FATFS* f = &fs;
    int r;
    f->winsect = (LBA_t)0 - 1;
    r = exfat_repair_dir(f, "/APP", 0);
    CHECK(r < 0);
  }
  fail_read_at = -1;
  mount();
  /* FAT12/16/32: nothing read for repair, nothing written. */
  for (v = 0; v < 3; ++v) {
    BYTE* snap;
    snprintf(ctx, sizeof ctx, "FAT volume %d", v);
    format(fmts[v], mbs[v], 512);
    CHECK(fs.fs_type != FS_EXFAT);
    nknown = 0;
    f_mkdir("/APP");
    for (i = 0; i < 20; ++i) {
      char name[300], pre[16];
      snprintf(pre, sizeof pre, "fat%d_", i);
      make_name(name, pre, 5 + (i * 41) % 200);
      add_file("/APP", name, i);
    }
    f_unlink(known[3].path);
    known[3] = known[--nknown];
    snap = snapshot();
    writes = 0;
    CHECK(exfat_repair_dir(&fs, "/APP", 1) == 0 && exfat_repair_dir(0, "/APP", 1) == 0 && writes == 0);
    CHECK(!memcmp(snap, img, (size_t)img_sectors * SS));
    check_known(0);
    free(snap);
  }
}

/* fsck.exfat finds something (no report printed). */
static int fsck_finds_errors(void) {
  char path[600], cmd[1400];
  FILE* f;
  snprintf(path, sizeof path, "%s/fsck.img", tmpdir);
  f = fopen(path, "wb");
  if (!f || fwrite(img, SS, img_sectors, f) != img_sectors) FAIL("write %s", path);
  fclose(f);
  snprintf(cmd, sizeof cmd, "fsck.exfat -n '%s' > /dev/null 2>&1", path);
  return system(cmd) != 0;
}

/* The app folder's own set torn in the root: without parents the folder
   cannot be opened (-FR_INT_ERR), with parents it is repaired. */
static void torn_in_root(void) {
  long s;
  for (s = 14; s <= 15; ++s) {
    BYTE* before;
    int k, nops, found = 0;
    wrec* ops;
    snprintf(ctx, sizeof ctx, "root: folder set at entry %ld", s);
    format(FM_EXFAT, 4, 4096);
    nknown = 0;
    fill_to("/", s);
    before = snapshot();
    nrec = 0;
    recording = 1;
    if (f_mkdir("/APPDIR") != FR_OK || f_mkdir("/APPDIR/DATA") != FR_OK) FAIL("mkdir");
    add_file("/APPDIR/DATA", "X.TXT", 9);
    recording = 0;
    CHECK(entry_of("/", "APPDIR") == s);
    nops = nrec;
    ops = malloc(sizeof *ops * (size_t)nops);
    memcpy(ops, recs, sizeof *ops * (size_t)nops);
    for (k = 0; k < nops; ++k) {
      int i, r;
      FRESULT st;
      FILINFO fi;
      restore(before);
      for (i = 0; i < k; ++i) {
        memcpy(img + (size_t)ops[i].sect * SS, ops[i].data, SS);
        dirty[ops[i].sect] = 1;
      }
      mount();
      /* GBASD_FF_FAST_EXFAT_LOOKUP (gbamp3): the lookup (dir_find) reads the
         name hash from the stream entry in place and skips a block whose
         hash differs without checking it, so a set whose second sector is
         missing is not found (FR_NO_FILE) instead of FR_INT_ERR; fsck.exfat
         still finds it. Without the switch only FR_INT_ERR is accepted. */
      st = f_stat("/APPDIR", &fi);
      if (st != FR_INT_ERR && (!GBASD_FF_FAST_EXFAT_LOOKUP || st != FR_NO_FILE || !fsck_finds_errors())) continue;
      ++found;
      CHECK(exfat_repair_dir(&fs, "/APPDIR/DATA", 0) == (st == FR_INT_ERR ? -(int)FR_INT_ERR : 0));
      r = repair_checked("/APPDIR/DATA");
      CHECK(r > 0);
      CHECK(f_stat("/APPDIR", &fi) == FR_NO_FILE);
      CHECK(fsck_clean());
      nknown--; /* X.TXT was not there before */
      check_known(0);
      nknown++;
      CHECK(f_mkdir("/APPDIR") == FR_OK && fsck_clean());
    }
    CHECK(found > 0);
    free(ops);
    free(before);
  }
}

/* FatFs R0.15's dir_register() added one cluster to an exFAT folder's data
   length when a new entry set took two new clusters (512-byte clusters, a
   long name when the folder's last cluster is full): with gbaplanner's
   FatFs change (a folder ends at its data length) the create failed with
   FR_INT_ERR, unmodified FatFs left the set past the data length (fsck.exfat
   reports it). ff.c's "exFAT fix:" counts every cluster; the first part
   below is its regression test (fails without it). The second part keeps
   the module's rule for FatFs copies without the fix: such a set past the
   data length, made by hand. */
static WORD set_sum(const BYTE* set, int n) {
  WORD sum = 0;
  int i;
  for (i = 0; i < n * 32; ++i)
    if (i != 2 && i != 3) sum = (WORD)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + set[i]);
  return sum;
}
static void stretch_bug(void) {
  char name[260], path[600];
  BYTE buf[3000], set[96];
  FRESULT r;
  int rep, k;
  for (k = 0; k < 4; ++k) { /* the folder full at 1 / 2 clusters; file or folder; chained or contiguous */
    static const long full[4] = {16, 32, 16, 16};
    DIR d;
    FSIZE_t size;
    snprintf(ctx, sizeof ctx, "FatFs two-cluster stretch, case %d", k);
    format(FM_EXFAT, 4, 512);
    nknown = 0;
    f_mkdir("/APP");
    if (k != 3) f_mkdir("/APP/SUB"); /* /APP grows in a FAT chain; k 3: contiguous */
    fill_to("/APP", full[k]);
    make_name(name, "L_", 255);
    snprintf(path, sizeof path, "/APP/%s", name);
    if (k == 2) {
      r = f_mkdir(path);
    } else {
      FIL f;
      UINT bw;
      r = f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS);
      if (r == FR_OK) CHECK(f_write(&f, told, 100, &bw) == FR_OK && f_close(&f) == FR_OK);
    }
    if (r != FR_OK) FAIL("create: %d (FatFs counted one new cluster)", (int)r);
    mount();
    CHECK(f_opendir(&d, "/APP") == FR_OK);
    size = d.obj.objsize;
    f_closedir(&d);
    if (size != (FSIZE_t)(full[k] + 32) * 32) FAIL("data length %lu", (unsigned long)size);
    CHECK(entry_of("/APP", name) == full[k]);
    CHECK(fsck_clean());
    writes = 0;
    CHECK(exfat_repair_dir(&fs, "/APP", 1) == 0 && writes == 0);
    if (k == 2) CHECK(list(path) == 0);
    else CHECK(get(path, buf, sizeof buf) == 100);
    check_known(0);
    add_file("/APP", "NEXT.TXT", 5);
    check_known(0);
    CHECK(fsck_clean());
  }
  printf("a set taking two new clusters (512-byte clusters, 255-character name): created, data length right, "
         "fsck.exfat clean\n");
  /* The same state made by hand, so both FatFs copies test it: a 19-entry
     set at entries 29-47 of /APP, then /APP's data length one cluster short
     (1024: the set's last 16 entries past it, in the chain). Written: left
     alone; never written (zero): freed. */
  for (k = 0; k < 2; ++k) {
    long at;
    size_t p0;
    int j;
    snprintf(ctx, sizeof ctx, "set past the data length, %s", k ? "never written" : "written");
    format(FM_EXFAT, 4, 512);
    nknown = 0;
    f_mkdir("/APP");
    f_mkdir("/APP/SUB");
    fill_to("/APP", 29);
    add_file("/APP", name, 77);
    CHECK(entry_of("/APP", name) == 29);
    at = entry_of("/", "APP");
    for (j = 0; j < 3; ++j) memcpy(set + 32 * j, img + entry_pos("/", at + j), 32);
    CHECK(set[32] == 0xC0 && set[32 + 24] == 0x00 && set[32 + 25] == 0x06); /* 1536 bytes */
    set[32 + 8 + 1] = set[32 + 24 + 1] = 0x04;                                /* 1024 */
    set[2] = (BYTE)set_sum(set, 3);
    set[3] = (BYTE)(set_sum(set, 3) >> 8);
    for (j = 0; j < 3; ++j) memcpy(img + entry_pos("/", at + j), set + 32 * j, 32);
    p0 = entry_pos("/APP", 32);
    if (k) for (j = 32; j < 48; ++j) memset(img + entry_pos("/APP", j), 0, 32);
    nknown--; /* never written: it goes; written: FatFs with gbaplanner's change cannot read it */
    mount();
    CHECK(entry_pos("/APP", 32) == p0);
    rep = repair_checked("/APP");
    CHECK(rep == k);
    if (k) CHECK(img[entry_pos("/APP", 29)] == 0x05 && list("/APP") >= 0);
    check_known(0);
    CHECK(exfat_repair_dir(&fs, "/APP", 1) == 0);
  }
  printf("set past the folder's data length in its chain: written, left alone; never written, freed\n");
}

int main(int argc, char** argv) {
  static const DWORD aus[2] = {512, 4096};
  static const int sizes[4] = {3, 4, 10, 19};
  int a, z, op, cases = 0;
  long s;
  tmpdir = argc > 1 ? argv[1] : ".";
  healthy_and_fat();
  printf("healthy exFAT folders and FAT12/16/32 volumes: unchanged, no writes; path forms, read error\n");
  torn_in_root();
  printf("folder set torn in the root: repaired with its parents\n");
  stretch_bug();
  for (a = 0; a < 2; ++a)
    for (z = 0; z < 4; ++z)
      for (s = 3; s <= 18; ++s) {
        const int L = sizes[z];
        const long off = s % 16;
        if (off + L <= 16) continue; /* the set must cross a sector boundary */
        if (L <= 4 && s >= 16) continue;
        for (op = 0; op < OP_N; ++op) {
          run_case(aus[a], "/APP", L, s, op);
          ++cases;
        }
      }
  printf("%d cases, %ld power-cut points; cases with a repair:", cases, cut_points);
  for (op = 0; op < OP_N; ++op) printf(" %s %ld", op_name[op], torn_cases[op]);
  printf("\n");
  snprintf(ctx, sizeof ctx, "summary");
  for (op = 0; op < OP_N; ++op)
    if (op != OP_APPEND && op != OP_REWRITE) CHECK(torn_cases[op] > 0);
  CHECK(torn_cases[OP_APPEND] + torn_cases[OP_REWRITE] > 0);
  printf("fsck.exfat reports on sets across three 512-byte clusters (its limit, sets valid) accepted %ld times\n",
         fsck_tolerated);
  printf("PASS exfat_repair (%ld checks)\n", checks);
  return 0;
}
