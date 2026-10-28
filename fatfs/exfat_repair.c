/* exfat_repair: see exfat_repair.h (what it fixes, when to call it, cost). */
#include "exfat_repair.h"

#if FF_FS_EXFAT

#include "diskio.h"

#if FF_MAX_SS == FF_MIN_SS
#define XR_SS(fs) ((UINT)FF_MAX_SS)
#else
#define XR_SS(fs) ((UINT)(fs)->ssize)
#endif
#define XR_NOWIN ((LBA_t)0 - 1)

/* fs->win holds sector fs->winsect; fs->wflag: changed, not written yet. */
static int xr_flush(FATFS* fs) {
  if (!fs->wflag) return 0;
  fs->wflag = 0;
  if (disk_write(fs->pdrv, fs->win, fs->winsect, 1) != RES_OK) {
    fs->winsect = XR_NOWIN;
    return -1;
  }
  if (fs->winsect - fs->fatbase < fs->fsize && fs->n_fats == 2) /* as FatFs' sync_window() */
    disk_write(fs->pdrv, fs->win, fs->winsect + fs->fsize, 1);
  return 0;
}

static int xr_load(FATFS* fs, LBA_t sect) {
  if (fs->winsect == sect) return 0;
  if (xr_flush(fs)) return -1;
  if (disk_read(fs->pdrv, fs->win, sect, 1) != RES_OK) {
    fs->winsect = XR_NOWIN;
    return -1;
  }
  fs->winsect = sect;
  return 0;
}

static WORD xr_ld16(const BYTE* p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD xr_ld32(const BYTE* p) { return (DWORD)p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }

static WORD xr_sum(WORD sum, const BYTE* e, int primary) {
  int i;
  for (i = 0; i < 32; ++i)
    if (!primary || (i != 2 && i != 3)) sum = (WORD)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + e[i]);
  return sum;
}

/* The entry set being read: entries are consecutive in the folder, so entry
   k is at byte o0 + 32 k of the sectors sec[0], sec[1], sec[2] (19 entries:
   at most 3 sectors). */
typedef struct {
  LBA_t sec[3];
  UINT o0;
  BYTE nsec, have, want;
  WORD sum, stored;
} xr_set;

static BYTE* xr_entry(FATFS* fs, const xr_set* s, UINT k) {
  const UINT b = s->o0 + 32 * k;
  return xr_load(fs, s->sec[b / XR_SS(fs)]) ? 0 : fs->win + b % XR_SS(fs);
}

/* Frees entries 0..have-1 of the set; leaves fs->win on any sector. */
static int xr_free_set(FATFS* fs, xr_set* s) {
  UINT k;
  for (k = 0; k < s->have; ++k) {
    BYTE* e = xr_entry(fs, s, k);
    if (!e) return -1;
    e[0] &= 0x7F;
    fs->wflag = 1;
  }
  s->want = s->have = 0;
  return 0;
}

static int xr_one(const TCHAR* path) {
  DIR d;
  FATFS* fs;
  DWORD clst, left, hops = 0;
  UINT in_clst = 0, i;
  BYTE contig, first = 1, end = 0, beyond = 0, sub;
  LBA_t sect;
  xr_set s;
  UINT skip = 0; /* secondary entries of a set FatFs did not write: left alone */
  int fixes = 0;
  FRESULT r = f_opendir(&d, path);

  if (r == FR_NO_PATH || r == FR_NO_FILE) return 0;
  if (r != FR_OK) return -(int)r;
  fs = d.obj.fs;
  clst = d.obj.sclust;
  sub = (BYTE)(clst != 0);
  if (fs->fs_type != FS_EXFAT) {
    f_closedir(&d);
    return 0;
  }
  if (clst == 0) { /* the root: a FAT chain, its end is the end */
    clst = fs->dirbase;
    contig = 0;
    left = 0xFFFFFFFF;
  } else {
    contig = (BYTE)((d.obj.stat & 2) != 0);
    left = (DWORD)((d.obj.objsize + XR_SS(fs) - 1) / XR_SS(fs));
  }
  f_closedir(&d);
  s.want = s.have = 0;
  if (xr_flush(fs)) return -(int)FR_DISK_ERR; /* FatFs' pending window write */
  for (;;) {
    if (!left) {
      /* The data length ends inside a set. FatFs R0.15 can leave a whole
         set past it (dir_register counts one new cluster when a set took
         two: 512-byte clusters, long names), and an unmodified FatFs reads
         a folder in a FAT chain to the end of its chain: so the rest of the
         set is looked at there; written (complete) the set is left alone,
         never written it is freed. (A contiguous folder ends at its data
         length for FatFs too: the set is freed.) */
      if (!s.want || !sub || contig || beyond) break;
      beyond = 1;
      left = 2; /* 18 entries at most */
    }
    if (end) break;
    if (!first && ++in_clst == fs->csize) { /* the next cluster */
      in_clst = 0;
      if (contig) {
        ++clst;
      } else {
        const UINT per = XR_SS(fs) / 4;
        if (xr_load(fs, fs->fatbase + clst / per)) goto io;
        clst = xr_ld32(fs->win + clst % per * 4);
      }
      if (++hops >= fs->n_fatent) break; /* a looping chain */
    }
    first = 0;
    if (clst < 2 || clst >= fs->n_fatent) break;
    sect = fs->database + (LBA_t)(clst - 2) * fs->csize + in_clst;
    --left;
    if (xr_load(fs, sect)) goto io;
    for (i = 0; i < XR_SS(fs) && !end; i += 32) {
      const BYTE* e = fs->win + i;
      const BYTE t = e[0];
      if (skip && (t & 0xC0) == 0xC0) {
        --skip;
        continue;
      }
      skip = 0;
      if (s.want) {
        if (!(t & 0x80)) { /* never written: an unfinished create / rename */
          if (xr_free_set(fs, &s) || xr_load(fs, sect)) goto io;
          ++fixes;
          if (beyond) { /* past the data length: nothing else is looked at */
            end = 1;
            continue;
          }
        } else if (t != (s.have == 1 ? 0xC0 : 0xC1) || (s.have == 1 && (e[3] + 44u) / 15 > s.want)) {
          skip = (UINT)(s.want - s.have - 1); /* not what FatFs writes: left alone */
          s.want = s.have = 0;
          end = beyond;
          continue;
        } else {
          const UINT idx = (s.o0 + 32u * s.have) / XR_SS(fs);
          if (idx == s.nsec) s.sec[s.nsec++] = sect;
          s.sum = xr_sum(s.sum, e, 0);
          if (++s.have == s.want) {
            if (s.sum != s.stored && !beyond) { /* an unfinished update of the stream entry */
              BYTE* f = xr_entry(fs, &s, 0);
              if (!f) goto io;
              f[2] = (BYTE)s.sum;
              f[3] = (BYTE)(s.sum >> 8);
              fs->wflag = 1;
              if (xr_load(fs, sect)) goto io;
              ++fixes;
            }
            s.want = s.have = 0;
            end = beyond;
          }
          continue;
        }
      }
      if (t == 0x00) {
        end = 1;
      } else if (t == 0x85 && e[1] >= 2 && e[1] <= 18) {
        s.want = (BYTE)(e[1] + 1);
        s.have = 1;
        s.stored = xr_ld16(e + 2);
        s.sum = xr_sum(0, e, 1);
        s.o0 = i;
        s.sec[0] = sect;
        s.nsec = 1;
      } else if (t == 0xC0 || t == 0xC1) { /* left by an unfinished delete */
        fs->win[i] = (BYTE)(t & 0x7F);
        fs->wflag = 1;
        ++fixes;
      } else if ((t & 0xC0) == 0x80) { /* another primary entry and its secondaries */
        skip = (t == 0x85 || t >= 0xA0) ? e[1] : 0;
      }
    }
  }
  if (s.want) { /* the folder ends inside the set */
    if (xr_free_set(fs, &s)) goto io;
    ++fixes;
  }
  if (xr_flush(fs)) goto io;
  fs->winsect = XR_NOWIN;
  if (fixes && disk_ioctl(fs->pdrv, CTRL_SYNC, 0) != RES_OK) return -(int)FR_DISK_ERR;
  return fixes;
io:
  fs->wflag = 0;
  fs->winsect = XR_NOWIN;
  return -(int)FR_DISK_ERR;
}

static int xr_sep(TCHAR c) { return c == '/' || c == '\\'; }

int exfat_repair_dir(FATFS* fs, const TCHAR* path, int with_parents) {
  TCHAR p[EXFAT_REPAIR_PATH_MAX];
  UINT n, i, root;
  int total = 0, r;

  if (fs && fs->fs_type && fs->fs_type != FS_EXFAT) return 0;
  if (!with_parents) return xr_one(path);
  for (n = 0; path[n]; ++n) {
    if (n + 1 >= EXFAT_REPAIR_PATH_MAX) return -(int)FR_INVALID_NAME;
    p[n] = path[n];
  }
  p[n] = 0;
  /* The root: a drive prefix ("0:") and the separators after it. */
  root = 0;
  for (i = 0; i < n && !xr_sep(p[i]); ++i)
    if (p[i] == ':') {
      root = i + 1;
      break;
    }
  while (root < n && xr_sep(p[root])) ++root;
  /* The root, then each folder on the way, then the folder itself. */
  for (i = root; i <= n; ++i) {
    if (i == root || ((i == n || xr_sep(p[i])) && !xr_sep(p[i - 1]))) {
      const TCHAR c = p[i];
      p[i] = 0;
      r = xr_one(p);
      p[i] = c;
      if (r < 0) return r;
      total += r;
    }
  }
  return total;
}

#else /* FAT only: nothing to repair */

int exfat_repair_dir(FATFS* fs, const TCHAR* path, int with_parents) {
  (void)fs;
  (void)path;
  (void)with_parents;
  return 0;
}

#endif
