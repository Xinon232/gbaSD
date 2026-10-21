/* crc_diag: SuperCard SD read CRC16 diagnostics ROM (see README.md).
 * Copyright (C) 2026 Halim Jarrar. Part of gbaSD, GPL-3.0-or-later (LICENSE).
 *
 * Starts the card with gbaSD (cart_sd_init), finds the FAT/exFAT volume and
 * reads, again and again, its first sectors (boot sector, FAT) and the first
 * DIAG_MB MiB of its data area (where the files are), in reads of 1 to 32
 * sectors into aligned and unaligned buffers (both read loops). It shows the
 * driver's read CRC16 counters (sdcard_crc_stats). Built twice:
 *   crc_diag.gba        count only (GBASD_SC_READ_CRC=1): every mismatch is
 *                       counted, nothing is retried. 0 mismatches over many
 *                       non-zero sectors proves the CRC byte layout on the cart.
 *                       A read with a mismatch is read again and compared:
 *                       same data -> the CRC (layout) is suspect, other
 *                       data -> a real transfer error.
 *   crc_diag_check.gba  check mode (the apps' default): retries, read errors,
 *                       safety switch.
 * Nothing is written to the card. */
#include <gba_base.h>
#include <gba_console.h>
#include <gba_input.h>
#include <gba_interrupt.h>
#include <gba_systemcalls.h>
#include <gba_video.h>
#include <stdio.h>
#include <string.h>
#include "cart_io.h"
#include "supercard_driver.h"
#include "gbasd_config.h"

#ifndef DIAG_MB
#define DIAG_MB 8           /* MiB of the data area per pass */
#endif
#define FIRST_SECTORS 64    /* boot sector and the start of the FAT */
#define MAX_CHUNK 32

#define REG_TM2CNT_L (*(volatile uint16_t *)0x04000108)
#define REG_TM2CNT_H (*(volatile uint16_t *)0x0400010A)
#define REG_TM3CNT_L (*(volatile uint16_t *)0x0400010C)
#define REG_TM3CNT_H (*(volatile uint16_t *)0x0400010E)

/* Read by the emulator test (tests/emu/crc_diag_test.py) through the ELF
 * symbol; keep the layout. */
struct diag_state {
  uint32_t magic;           /* 'CRCD' once the volume is found */
  uint32_t init_error;      /* cart_sd_init() */
  uint32_t cart;            /* cart_kind */
  uint32_t pass;            /* passes completed */
  uint32_t sectors;         /* sectors read (all passes) */
  uint32_t nonzero;         /* of these, not all zero */
  uint32_t read_errors;     /* cart_read() errors */
  uint32_t checksum1;       /* of the data read in pass 1 (read order) */
  uint32_t volume_start, data_start, data_sectors;
  uint32_t first_bad_lba;   /* first read with a mismatch (start sector) */
  uint32_t same_data;       /* count mode: mismatching reads that read back the same */
  uint32_t other_data;      /* ... that read back different data */
  uint32_t kib_per_s;
};
struct diag_state diag = { .first_bad_lba = 0xFFFFFFFF };

/* In EWRAM (.sbss), like the apps' buffers. */
static uint8_t buf[MAX_CHUNK * 512 + 4] __attribute__((aligned(4), section(".sbss")));
static uint8_t buf2[MAX_CHUNK * 512] __attribute__((aligned(4), section(".sbss")));
static uint32_t sum_a = 1, sum_b;   /* Adler-style, mod 2^32 */

static void timer_start(void) {
  REG_TM2CNT_H = 0; REG_TM3CNT_H = 0; REG_TM2CNT_L = 0; REG_TM3CNT_L = 0;
  REG_TM3CNT_H = 0x84; REG_TM2CNT_H = 0x83;   /* TM2: 16384 Hz, TM3 cascades */
}
static uint32_t timer_now(void) { return (uint32_t)REG_TM3CNT_L << 16 | REG_TM2CNT_L; }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }

static const char *mode_name(unsigned m) {
  return m == 1 ? "count only" : m == 2 ? "check+retry" : "off";
}

static void show(const char *phase) {
  const t_sc_crc_stats *st = sdcard_crc_stats();
  iprintf("\x1b[4;0H");
  iprintf("Phase     %-19s\n", phase);
  iprintf("Pass      %lu       \n", (unsigned long)diag.pass + 1);
  iprintf("Sectors   %lu       \n", (unsigned long)diag.sectors);
  iprintf("Non-zero  %lu       \n", (unsigned long)diag.nonzero);
  iprintf("Speed     %lu KiB/s    \n", (unsigned long)diag.kib_per_s);
  iprintf("Read errs %lu       \n\n", (unsigned long)diag.read_errors);
  iprintf("CRC checked %lu      \n", (unsigned long)st->sectors);
  iprintf("Mismatches  %lu      \n", (unsigned long)st->mismatches);
  if (st->mode == 1) {
    iprintf(" reread same  %lu    \n", (unsigned long)diag.same_data);
    iprintf(" reread other %lu    \n", (unsigned long)diag.other_data);
  } else {
    iprintf("Retries %lu Failed %lu  \n", (unsigned long)st->retries, (unsigned long)st->failures);
    iprintf("Suspect %lu Trusted %s   \n", (unsigned long)st->suspects, st->trusted ? "yes" : "no");
  }
  iprintf("Check disabled  %s  \n", st->disabled ? "YES" : "no");
  if (diag.first_bad_lba != 0xFFFFFFFF)
    iprintf("First bad LBA   %lu   \n", (unsigned long)diag.first_bad_lba);
  else
    iprintf("%29s\n", "");
  /* 29 columns at most: a full 30-column line wraps. */
  if (st->disabled)
    iprintf("RESULT: check OFF (switch)   ");
  else if (st->mode == 2 && st->failures)
    iprintf("RESULT: read errors          ");
  else if (st->mode == 2 && st->mismatches)
    iprintf("RESULT: retried, data OK     ");
  else if (st->mismatches)
    iprintf("RESULT: MISMATCH, see README ");
  else if (diag.nonzero >= 2048)
    iprintf("RESULT: 0 mismatches, CRC OK ");
  else
    iprintf("RESULT: reading...           ");
}

/* One read of n sectors at lba into buf + off; counts and checks it. */
static void read_chunk(uint32_t lba, unsigned n, unsigned off, bool first_pass) {
  const t_sc_crc_stats *st = sdcard_crc_stats();
  uint32_t before = st->mismatches;
  uint8_t *p = buf + off;
  unsigned err = cart_read(p, lba, n);
  diag.sectors += n;
  if (err) {
    diag.read_errors++;
    return;
  }
  if (st->mismatches != before) {
    if (diag.first_bad_lba == 0xFFFFFFFF)
      diag.first_bad_lba = lba;
    if (st->mode == 1) {       /* read again: is the data the same? */
      memcpy(buf2, p, n * 512);
      if (cart_read(p, lba, n) == 0) {
        if (memcmp(buf2, p, n * 512))
          diag.other_data++;
        else
          diag.same_data++;
      }
    }
  }
  for (unsigned s = 0; s < n; s++) {
    const uint8_t *q = p + 512 * s;
    unsigned nz = 0;
    for (unsigned i = 0; i < 512; i++) {
      nz |= q[i];
      if (first_pass) {
        sum_a += q[i];
        sum_b += sum_a;
      }
    }
    if (nz)
      diag.nonzero++;
  }
}

static void read_range(uint32_t lba, uint32_t count, bool first_pass, const char *phase) {
  static const uint8_t sizes[] = { 8, 1, 32, 16, 3, 32 };
  static unsigned k;
  uint32_t t0 = timer_now(), s0 = diag.sectors;
  while (count) {
    unsigned n = sizes[k % sizeof sizes];
    unsigned off = (k / sizeof sizes) % 3 == 2 ? 1 + (k & 2) : 0;   /* a third of the reads unaligned */
    k++;
    if (n > count)
      n = count;
    read_chunk(lba, n, off, first_pass);
    lba += n;
    count -= n;
    uint32_t t = timer_now() - t0;
    if (t > 8192 || !count) {          /* about twice a second */
      diag.kib_per_s = t ? (uint32_t)((uint64_t)(diag.sectors - s0) * 512 * 16384 / 1024 / t) : 0;
      show(phase);
      t0 = timer_now();
      s0 = diag.sectors;
    }
  }
}

int main(void) {
  irqInit();
  irqEnable(IRQ_VBLANK);
  consoleDemoInit();
  timer_start();
  iprintf("gbaSD SuperCard read CRC diag\n");
  iprintf("Mode: %s, unroll %d\n", mode_name(GBASD_SC_READ_CRC), GBASD_SC_READ_UNROLL);

  diag.init_error = cart_sd_init();
  diag.cart = cart_kind;
  if (cart_kind != CART_SUPERCARD) {
    iprintf("\nNot a SuperCard (cart %lu).\nThis ROM tests the SuperCard\ndriver only.\n", (unsigned long)cart_kind);
    for (;;) VBlankIntrWait();
  }
  if (diag.init_error) {
    iprintf("\nSD card start error %lu\n", (unsigned long)diag.init_error);
    for (;;) VBlankIntrWait();
  }

  /* The volume: MBR partition 1, or a volume boot sector at 0. */
  uint32_t vol = 0;
  if (cart_read(buf, 0, 1)) {
    iprintf("\nCannot read sector 0\n");
    for (;;) VBlankIntrWait();
  }
  bool vbr = !memcmp(buf + 3, "EXFAT", 5) || !memcmp(buf + 54, "FAT", 3) || !memcmp(buf + 82, "FAT32", 5);
  if (!vbr && buf[510] == 0x55 && buf[511] == 0xAA && buf[446 + 4])
    vol = rd32(buf + 446 + 8);
  uint32_t data = vol, total = 0;
  if (vol && cart_read(buf, vol, 1) == 0) vbr = true;
  if (vbr) {
    if (!memcmp(buf + 3, "EXFAT", 5)) {
      data = vol + rd32(buf + 88);                     /* ClusterHeapOffset */
      total = rd32(buf + 92) << buf[109];              /* ClusterCount << SectorsPerClusterShift */
    } else {
      uint32_t fatsz = rd16(buf + 22) ? rd16(buf + 22) : rd32(buf + 36);
      uint32_t tot = rd16(buf + 19) ? rd16(buf + 19) : rd32(buf + 32);
      uint32_t meta = rd16(buf + 14) + buf[16] * fatsz + (rd16(buf + 17) * 32 + 511) / 512;
      data = vol + meta;
      total = tot > meta ? tot - meta : 0;
    }
  }
  if (!total || total > DIAG_MB * 2048u)
    total = DIAG_MB * 2048u;
  diag.volume_start = vol;
  diag.data_start = data;
  diag.data_sectors = total;
  diag.magic = 0x44435243;   /* 'CRCD' */
  iprintf("Volume %lu, data %lu\n", (unsigned long)vol, (unsigned long)data);

  for (;;) {
    bool first = diag.pass == 0;
    read_range(vol, FIRST_SECTORS, first, "boot+FAT");
    read_range(data, total, first, "data area");
    if (first)
      diag.checksum1 = sum_b ^ (sum_a << 16);
    diag.pass++;
  }
}
