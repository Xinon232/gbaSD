/* Host tool of the exFAT test kit: reads and writes files on an exFAT test
 * card image with the app's FatFs (gbaSD's fatfs/ and the app's ffconf.h,
 * exFAT on; build.py builds it). mkfs.exfat formats the card and fsck.exfat
 * checks it afterwards, so the card itself is judged by an independent tool.
 * From gbamp3's tests/exfat/exfat_tool.c and the versions grown from it in
 * gbaplanner, gbawriter, gbavocab and gbareader, merged:
 *   exfat_tool IMG mkdir PATH              make a folder (parents too; an existing one is fine)
 *   exfat_tool IMG put HOSTFILE PATH...    write files (replaces them; HOSTFILE PATH pairs)
 *   exfat_tool IMG puttree HOSTDIR PATH    copy a folder tree into PATH
 *   exfat_tool IMG get PATH HOSTFILE       read a file ("-": to stdout)
 *   exfat_tool IMG cat PATH                a file to stdout
 *   exfat_tool IMG gettree PATH HOSTDIR    copy the folder tree PATH into HOSTDIR
 *   exfat_tool IMG ls PATH                 "D|F <size> <name>" per entry, or the file itself
 *   exfat_tool IMG rm PATH...              delete files or empty folders
 *   exfat_tool IMG mv PATH NEWPATH         rename / move
 *   exfat_tool IMG attr PATH               "ADSHR" letters ('-' when clear)
 *   exfat_tool IMG chmod PATH [+-][ahrs]...  set or clear attributes
 *   exfat_tool IMG attrib +r|-r PATH       set or clear read-only (gbareader's form)
 *   exfat_tool IMG free /                  free bytes on the card
 *   exfat_tool IMG fragment [MB|Nc] [FILE] two files grow in turn a cluster each over MB
 *                                          megabytes (Nc: N cluster pairs; 0 or none:
 *                                          all the free space but 64 clusters), then the
 *                                          second is deleted: the free space is every
 *                                          other cluster; FILE (default /FRAGMENT.BIN)
 *                                          stays
 *   exfat_tool IMG fragment-files N        N one-cluster files (holding text) at the start
 *                                          of the free space in the hidden folder
 *                                          /.fragment, every other one deleted: N/2
 *                                          one-cluster holes
 * Layout of what put / puttree write:
 *   puttree first makes every file of a folder empty, then fills them one by
 *   one: the folder's entries come before any data, so a large folder stays
 *   contiguous, like the files ("NoFatChain"). With EXFAT_FRAGMENT=1 it
 *   writes the files of a folder a cluster at a time in turn instead, so
 *   files and large folders get FAT chains.
 *   put writes a file in one piece, unless EXFAT_FRAGMENT=1 and
 *   EXFAT_FRAGMENT_MODE=put: then each cluster of the file is followed by
 *   one of a scratch file deleted afterwards (the file in a FAT chain, the
 *   card left with free space in holes), when the card has room for the file
 *   twice over.
 * Paths use '/', relative to the volume root. Exit 3 when a path does not
 * exist, 1 on any other error, 2 on bad arguments. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define DIR SYS_DIR   /* FatFs has its own DIR */
#include <dirent.h>
#undef DIR
#include "ff.h"
#include "diskio.h"

static int img_fd = -1;
static off_t img_size;
DSTATUS disk_initialize(BYTE d) { return d ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE d) { return d ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE d, BYTE *b, LBA_t s, UINT n) {
    if (d || ((off_t)s + n) * 512 > img_size) return RES_ERROR;
    return pread(img_fd, b, n * 512, (off_t)s * 512) == (ssize_t)n * 512 ? RES_OK : RES_ERROR;
}
DRESULT disk_write(BYTE d, const BYTE *b, LBA_t s, UINT n) {
    if (d || ((off_t)s + n) * 512 > img_size) return RES_ERROR;
    return pwrite(img_fd, b, n * 512, (off_t)s * 512) == (ssize_t)n * 512 ? RES_OK : RES_ERROR;
}
DRESULT disk_ioctl(BYTE d, BYTE c, void *b) {
    (void)d;
    if (c == CTRL_SYNC) return RES_OK;
    if (c == GET_SECTOR_COUNT) { *(LBA_t *)b = (LBA_t)(img_size / 512); return RES_OK; }
    if (c == GET_SECTOR_SIZE) { *(WORD *)b = 512; return RES_OK; }
    if (c == GET_BLOCK_SIZE) { *(DWORD *)b = 1; return RES_OK; }
    return RES_PARERR;
}
DWORD get_fattime(void) { return (DWORD)(46 << 25 | 1 << 21 | 1 << 16); }

static FATFS fs;
static void die(const char *what, const char *path, FRESULT r) {
    fprintf(stderr, "exfat_tool: %s %s: FatFs error %d\n", what, path, (int)r);
    exit(r == FR_NO_FILE || r == FR_NO_PATH ? 3 : 1);
}
static void vol(char *out, const char *p) { snprintf(out, 1024, "0:%s%s", p[0] == '/' ? "" : "/", p); }
static int env_is(const char *name, const char *value) { const char *v = getenv(name); return v && !strcmp(v, value); }

static void mkdirs(const char *path) {
    char v[1024], *s;
    vol(v, path);
    for (s = v + 3; ; s++) {
        if (*s == '/' || !*s) {
            char c = *s;
            *s = 0;
            if (strlen(v) > 3) {
                FILINFO fi;
                if (f_stat(v, &fi) != FR_OK) { FRESULT r = f_mkdir(v); if (r) die("mkdir", v, r); }
            }
            *s = c;
            if (!c) break;
        }
    }
}

static void put(const char *host, const char *path, int frag) {
    char v[1024];
    vol(v, path);
    FILE *in = fopen(host, "rb");
    if (!in) { perror(host); exit(1); }
    static FIL f, scratch;
    FRESULT r = f_open(&f, v, FA_WRITE | FA_CREATE_ALWAYS);
    if (r) die("create", v, r);
    const char *pad = "0:/exfat_tool.pad";
    if (frag) {   /* no room for the file twice over: it goes on in one piece */
        DWORD free_clusters;
        FATFS *fsp;
        struct stat st;
        if (fstat(fileno(in), &st) || (r = f_getfree("0:", &free_clusters, &fsp))) die("getfree", v, r ? r : FR_INT_ERR);
        if ((unsigned long long)st.st_size * 2 + 2ull * fs.csize * 512u > (unsigned long long)free_clusters * fs.csize * 512u) frag = 0;
    }
    if (frag && (r = f_open(&scratch, pad, FA_WRITE | FA_CREATE_ALWAYS))) die("create", pad, r);
    static char buf[1 << 20];
    const size_t step = frag ? fs.csize * 512u : sizeof buf;
    size_t n;
    while ((n = fread(buf, 1, step, in)) > 0) {
        UINT w;
        r = f_write(&f, buf, (UINT)n, &w);
        if (r || w != n) die("write", v, r ? r : FR_DENIED);
        if (frag && n == step) {   /* the next cluster of the card goes to the scratch file */
            r = f_write(&scratch, buf, (UINT)n, &w);
            if (r || w != n) die("write", pad, r ? r : FR_DENIED);
        }
    }
    fclose(in);
    r = f_close(&f);
    if (r) die("close", v, r);
    if (frag && ((r = f_close(&scratch)) || (r = f_unlink(pad)))) die("remove", pad, r);
}

/* Fragmented copy: up to 16 files of one folder open at once, written one
 * cluster in turn, so each one's clusters are interleaved with the others'. */
#define TURNS 16
static void put_turns(char hosts[][1024], char paths[][1024], int n) {
    static FIL f[TURNS];
    FILE *in[TURNS];
    UINT step = fs.csize * 512u;
    static char buf[1 << 20];
    for (int i = 0; i < n; i++) {
        char v[1024];
        vol(v, paths[i]);
        in[i] = fopen(hosts[i], "rb");
        if (!in[i]) { perror(hosts[i]); exit(1); }
        FRESULT r = f_open(&f[i], v, FA_WRITE | FA_CREATE_ALWAYS);
        if (r) die("create", v, r);
    }
    for (int live = n; live;) {
        live = 0;
        for (int i = 0; i < n; i++) {
            if (!in[i]) continue;
            size_t got = fread(buf, 1, step, in[i]);
            if (got) {
                UINT w;
                FRESULT r = f_write(&f[i], buf, (UINT)got, &w);
                if (r || w != got) die("write", paths[i], r ? r : FR_DENIED);
            }
            if (got < step) { fclose(in[i]); in[i] = 0; FRESULT r = f_close(&f[i]); if (r) die("close", paths[i], r); }
            else live = 1;
        }
    }
}

static int by_name(const struct dirent **a, const struct dirent **b) { return strcmp((*a)->d_name, (*b)->d_name); }
static void puttree(const char *host, const char *path, int frag) {
    mkdirs(path);
    struct dirent **e;
    int n = scandir(host, &e, 0, by_name);
    if (n < 0) { perror(host); exit(1); }
    char (*hosts)[1024] = malloc(TURNS * 1024), (*paths)[1024] = malloc(TURNS * 1024);   /* per level: the recursion queues its own */
    int queued = 0;
    for (int i = 0; i < n && !frag; i++) {         /* the entries first */
        char h[1024], v[1024];
        struct stat st;
        snprintf(h, sizeof h, "%s/%s", host, e[i]->d_name);
        snprintf(v, sizeof v, "0:%s/%s", strcmp(path, "/") ? path : "", e[i]->d_name);
        if (stat(h, &st) || !S_ISREG(st.st_mode)) continue;
        FIL f;
        FRESULT r = f_open(&f, v, FA_WRITE | FA_CREATE_ALWAYS);
        if (r || (r = f_close(&f))) die("create", v, r);
    }
    for (int i = 0; i < n; i++) {
        const char *name = e[i]->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char h[1024], p[1024];
        snprintf(h, sizeof h, "%s/%s", host, name);
        snprintf(p, sizeof p, "%s/%s", strcmp(path, "/") ? path : "", name);
        struct stat st;
        if (stat(h, &st)) { perror(h); exit(1); }
        if (S_ISDIR(st.st_mode)) puttree(h, p, frag);
        else if (!frag) put(h, p, 0);
        else {
            strcpy(hosts[queued], h);
            strcpy(paths[queued], p);
            if (++queued == TURNS) { put_turns(hosts, paths, queued); queued = 0; }
        }
    }
    if (queued) put_turns(hosts, paths, queued);
    for (int i = 0; i < n; i++) free(e[i]);
    free(e);
    free(hosts);
    free(paths);
}

static void get(const char *path, const char *host) {
    char v[1024];
    vol(v, path);
    FIL f;
    FRESULT r = f_open(&f, v, FA_READ);
    if (r) die("open", v, r);
    FILE *out = strcmp(host, "-") ? fopen(host, "wb") : stdout;
    if (!out) { perror(host); exit(1); }
    static char buf[65536];
    for (;;) {
        UINT got;
        r = f_read(&f, buf, sizeof buf, &got);
        if (r) die("read", v, r);
        if (!got) break;
        if (fwrite(buf, 1, got, out) != got) { perror(host); exit(1); }
    }
    f_close(&f);
    if (out == stdout ? fflush(out) : fclose(out)) { perror(host); exit(1); }
}

/* Like mcopy -s ::PATH HOSTDIR: files and folders of PATH (recursively) into HOSTDIR. */
static void gettree(const char *path, const char *host) {
    char v[1024];
    vol(v, path);
    if (mkdir(host, 0777) && access(host, F_OK)) { perror(host); exit(1); }
    DIR d;
    FILINFO fi;
    FRESULT r = f_opendir(&d, v);
    if (r) die("opendir", v, r);
    while ((r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0]) {
        char p[1024], h[1024];
        snprintf(p, sizeof p, "%s/%s", strcmp(path, "/") ? path : "", fi.fname);
        snprintf(h, sizeof h, "%s/%s", host, fi.fname);
        if (fi.fattrib & AM_DIR) gettree(p, h);
        else get(p, h);
    }
    if (r) die("readdir", v, r);
    f_closedir(&d);
}

static void ls(const char *path) {
    char v[1024];
    vol(v, path);
    FILINFO fi;
    if (strlen(v) > 3) {
        FRESULT r = f_stat(v, &fi);
        if (r) die("stat", v, r);
        if (!(fi.fattrib & AM_DIR)) { printf("F %llu %s\n", (unsigned long long)fi.fsize, fi.fname); return; }
    }
    DIR d;
    FRESULT r = f_opendir(&d, v);
    if (r) die("opendir", v, r);
    while ((r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0])
        printf("%c %llu %s\n", fi.fattrib & AM_DIR ? 'D' : 'F', fi.fattrib & AM_DIR ? 0ull : (unsigned long long)fi.fsize, fi.fname);
    if (r) die("readdir", v, r);
    f_closedir(&d);
}

static const struct { char letter; BYTE bit; } ATTRS[] = {{'A', AM_ARC}, {'D', AM_DIR}, {'S', AM_SYS}, {'H', AM_HID}, {'R', AM_RDO}};
static void attr(const char *path) {
    char v[1024];
    vol(v, path);
    FILINFO fi;
    FRESULT r = f_stat(v, &fi);
    if (r) die("stat", v, r);
    for (unsigned i = 0; i < sizeof ATTRS / sizeof *ATTRS; i++) putchar(fi.fattrib & ATTRS[i].bit ? ATTRS[i].letter : '-');
    putchar('\n');
}
static void chmod_(const char *path, int argc, char **argv) {
    char v[1024];
    vol(v, path);
    BYTE set = 0, mask = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if ((a[0] != '+' && a[0] != '-') || !a[1]) { fprintf(stderr, "exfat_tool: bad attribute %s\n", a); exit(2); }
        for (const char *c = a + 1; *c; c++) {
            BYTE bit = 0;
            for (unsigned k = 0; k < sizeof ATTRS / sizeof *ATTRS; k++)
                if ((*c & ~0x20) == ATTRS[k].letter && ATTRS[k].bit != AM_DIR) bit = ATTRS[k].bit;
            if (!bit) { fprintf(stderr, "exfat_tool: bad attribute %s\n", a); exit(2); }
            mask |= bit;
            if (a[0] == '+') set |= bit; else set &= (BYTE)~bit;
        }
    }
    FRESULT r = f_chmod(v, set, mask);
    if (r) die("chmod", v, r);
}

/* Two files written a cluster in turn, then one of them deleted: the free
 * space left is single clusters between the other one's. */
static void fragment(const char *amount, const char *keep) {
    FIL a, b;
    FRESULT r;
    UINT step = fs.csize * 512u, w;
    static char buf[1 << 16];
    char kv[1024];
    if (step > sizeof buf) die("fragment", "cluster too large", FR_INVALID_PARAMETER);
    memset(buf, 0xA5, sizeof buf);
    vol(kv, keep);
    if ((r = f_open(&a, kv, FA_WRITE | FA_CREATE_ALWAYS))) die("create", kv, r);
    if ((r = f_open(&b, "0:/FRAGMENT.TMP", FA_WRITE | FA_CREATE_ALWAYS))) die("create", "FRAGMENT.TMP", r);
    DWORD nfree = 0;
    FATFS *pfs;
    if ((r = f_getfree("0:", &nfree, &pfs))) die("getfree", "0:", r);
    long n = atol(amount), pairs;
    if (n && amount[strlen(amount) - 1] == 'c') pairs = n;
    else pairs = n ? n * 1024 * 1024 / (2 * (long)step) : (long)nfree / 2 - 64;
    if (pairs > (long)nfree / 2 - 2) pairs = (long)nfree / 2 - 2;
    for (long k = 0; k < pairs; k++) {
        if ((r = f_write(&a, buf, step, &w)) || w != step) die("write", kv, r ? r : FR_DENIED);
        if ((r = f_write(&b, buf, step, &w)) || w != step) die("write", "FRAGMENT.TMP", r ? r : FR_DENIED);
    }
    if ((r = f_close(&a)) || (r = f_close(&b))) die("close", "FRAGMENT", r);
    if ((r = f_unlink("0:/FRAGMENT.TMP"))) die("rm", "FRAGMENT.TMP", r);
}

/* N one-cluster files in /.fragment (64 per sub-folder), every other one deleted. */
static void fragment_files(long n) {
    FRESULT r = f_mkdir("0:/.fragment");
    if (r) die("mkdir", "/.fragment", r);
    if ((r = f_chmod("0:/.fragment", AM_HID | AM_SYS, AM_HID | AM_SYS))) die("chmod", "/.fragment", r);
    static char buf[1 << 16];
    UINT cluster = fs.csize * 512u;
    if (cluster > sizeof buf) { fprintf(stderr, "exfat_tool: fragment-files: cluster too large\n"); exit(1); }
    /* Text, like a deleted note: what a freed cluster on a used card holds. In a
     * folder its bytes look like free entries (bit 7 clear), not like end-of-folder. */
    static const char text[] = "Old words from a deleted note. ";
    for (UINT i = 0; i < cluster; i++) buf[i] = text[i % (sizeof text - 1)];
    for (long i = 0; i < n; i++) {
        char v[64];
        if (i % 64 == 0) {
            snprintf(v, sizeof v, "0:/.fragment/%03ld", i / 64);
            if ((r = f_mkdir(v))) die("mkdir", v, r);
        }
        snprintf(v, sizeof v, "0:/.fragment/%03ld/%02ld", i / 64, i % 64);
        FIL f;
        UINT w;
        if ((r = f_open(&f, v, FA_WRITE | FA_CREATE_ALWAYS)) || (r = f_write(&f, buf, cluster, &w)) || (r = f_close(&f))) die("fill", v, r);
    }
    for (long i = 0; i < n; i += 2) {
        char v[64];
        snprintf(v, sizeof v, "0:/.fragment/%03ld/%02ld", i / 64, i % 64);
        if ((r = f_unlink(v))) die("rm", v, r);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: exfat_tool IMG mkdir|put|puttree|get|cat|gettree|ls|rm|mv|attr|chmod|attrib|free|fragment|fragment-files ARGS\n");
        return 2;
    }
    img_fd = open(argv[1], O_RDWR);
    if (img_fd < 0) { perror(argv[1]); return 1; }
    img_size = lseek(img_fd, 0, SEEK_END);
    FRESULT r = f_mount(&fs, "0:", 1);
    if (r) die("mount", argv[1], r);
    if (fs.fs_type != FS_EXFAT) { fprintf(stderr, "exfat_tool: %s is not exFAT\n", argv[1]); return 1; }
    const char *c = argv[2];
    int frag = env_is("EXFAT_FRAGMENT", "1");
    char v[1024], w[1024];
    vol(v, argc > 3 ? argv[3] : "/");
    if (!strcmp(c, "mkdir") && argc == 4) mkdirs(argv[3]);
    else if (!strcmp(c, "put") && argc >= 5 && argc % 2 == 1)
        for (int i = 3; i < argc; i += 2) put(argv[i], argv[i + 1], frag && env_is("EXFAT_FRAGMENT_MODE", "put"));
    else if (!strcmp(c, "puttree") && argc == 5) puttree(argv[3], argv[4], frag);
    else if (!strcmp(c, "get") && argc == 5) get(argv[3], argv[4]);
    else if (!strcmp(c, "cat") && argc == 4) get(argv[3], "-");
    else if (!strcmp(c, "gettree") && argc == 5) gettree(argv[3], argv[4]);
    else if (!strcmp(c, "ls") && argc == 4) ls(argv[3]);
    else if (!strcmp(c, "rm") && argc >= 4) for (int i = 3; i < argc; i++) { vol(v, argv[i]); if ((r = f_unlink(v))) die("rm", v, r); }
    else if (!strcmp(c, "mv") && argc == 5) { vol(w, argv[4]); if ((r = f_rename(v, w))) die("rename", v, r); }
    else if (!strcmp(c, "attr") && argc == 4) attr(argv[3]);
    else if (!strcmp(c, "chmod") && argc >= 5) chmod_(argv[3], argc - 4, argv + 4);
    else if (!strcmp(c, "attrib") && argc == 5 && (!strcmp(argv[3], "+r") || !strcmp(argv[3], "-r"))) {
        vol(w, argv[4]);
        if ((r = f_chmod(w, argv[3][0] == '+' ? AM_RDO : 0, AM_RDO))) die("attrib", w, r);
    }
    else if (!strcmp(c, "free")) {
        DWORD n;
        FATFS *f;
        if ((r = f_getfree("0:", &n, &f))) die("getfree", argv[1], r);
        printf("%llu\n", (unsigned long long)n * f->csize * 512u);
    }
    else if (!strcmp(c, "fragment") && argc <= 5) fragment(argc > 3 ? argv[3] : "0", argc > 4 ? argv[4] : "/FRAGMENT.BIN");
    else if (!strcmp(c, "fragment-files") && argc == 4) fragment_files(atol(argv[3]));
    else { fprintf(stderr, "exfat_tool: bad command or arguments: %s\n", c); return 2; }
    r = f_unmount("0:");
    if (r) die("unmount", argv[1], r);
    if (close(img_fd)) { perror(argv[1]); return 1; }
    return 0;
}
