/* SuperCard read CRC16 host test.
 * 1. The CRC: a bit-by-bit reference (four CRC16-CCITT, one per data line,
 *    packed as the card sends them) against crc.c and against a C copy of
 *    the read loop's word step and final compare (supercard_io.s
 *    SC_CRC_WORD / SC_CRC_CHECK); every single bit flip (all four lanes,
 *    every position) and a flipped CRC byte must be detected.
 * 2. The driver's retry and safety switch (sdcard_read_recover) against a
 *    scripted card whose read routine returns like supercard_io.s: clean
 *    reads, a transient error (re-read from the failing sector), a permanent
 *    one (read error), the same before the layout is trusted (kept, suspect),
 *    every sector wrong (check switched off), count-only mode.
 * gbaSD, GPL-3.0-or-later. */
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "supercard_driver.h"
#include "crc.h"
#include "gbasd_config.h"
#define SC_RD_CAT2(a,b,c) a##b##c
#define SC_RD_CAT(a,b,c) SC_RD_CAT2(a,b,c)
#define SC_RD_MODE_SYM(w) SC_RD_CAT(sc_read_sectors_crc,GBASD_SC_READ_CRC,_##w)

/* ---- 1. CRC ---- */

/* Reference: de-interleave the lanes (high nibble first, nibble bit l = line
 * l), CRC16-CCITT (0x1021, init 0) per lane, pack 16 nibbles MSB first. */
static void ref_crc(const uint8_t *d, uint8_t out[8]) {
  unsigned crc[4] = {0};
  for (unsigned i = 0; i < 512; i++)
    for (int sh = 4; sh >= 0; sh -= 4)
      for (unsigned l = 0; l < 4; l++) {
        unsigned fb = ((d[i] >> (sh + l)) & 1) ^ ((crc[l] >> 15) & 1);
        crc[l] = ((crc[l] << 1) & 0xffff) ^ (fb ? 0x1021 : 0);
      }
  for (unsigned i = 0; i < 8; i++) {
    unsigned v = 0;
    for (unsigned h = 0; h < 2; h++)
      for (unsigned l = 0; l < 4; l++)
        v |= ((crc[l] >> (15 - 2 * i - h)) & 1) << (4 * (1 - h) + l);
    out[i] = v;
  }
}

static uint32_t ror(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* supercard_io.s SC_CRC_WORD, instruction by instruction. */
static void asm_word(uint32_t x, uint32_t *hi, uint32_t *lo) {
  uint32_t t1 = x ^ ror(x, 16);
  t1 &= ~0x00FF0000u;
  uint32_t t2 = *hi ^ ror(x, 8);
  t2 ^= t1 >> 8;
  t2 ^= t2 >> 16;
  *hi = *lo ^ (t2 >> 12);
  *hi ^= t2 << 16;
  *lo = t2 ^ (t2 << 20);
}
/* The unaligned path: big-endian word of four bytes, second half of the step. */
static void asm_word_be(const uint8_t *p, uint32_t *hi, uint32_t *lo) {
  uint32_t w = p[3] | p[2] << 8 | p[1] << 16 | (uint32_t)p[0] << 24;
  uint32_t t = *hi ^ w;
  t ^= t >> 16;
  *hi = *lo ^ (t >> 12);
  *hi ^= t << 16;
  *lo = t ^ (t << 20);
}
/* SC_CRC_CHECK: zero when the CRC bytes (two little-endian words) match. */
static uint32_t asm_check(uint32_t hi, uint32_t lo, const uint8_t crc[8]) {
  uint32_t r7 = le32(crc), r11 = le32(crc + 4);
  uint32_t r2 = (r7 ^ ror(r7, 16)) & ~0x00FF0000u;
  hi ^= ror(r7, 8) ^ (r2 >> 8);
  r2 = (r11 ^ ror(r11, 16)) & ~0x00FF0000u;
  lo ^= ror(r11, 8) ^ (r2 >> 8);
  return hi | lo;
}
static uint32_t loop_check(const uint8_t *d, const uint8_t crc[8], bool aligned) {
  uint32_t hi = 0, lo = 0;
  for (unsigned i = 0; i < 512; i += aligned ? 8 : 4) {
    if (aligned) {
      asm_word(le32(d + i), &hi, &lo);       /* r7 */
      asm_word(le32(d + i + 4), &hi, &lo);   /* r11 */
    } else {
      asm_word_be(d + i, &hi, &lo);
    }
  }
  return asm_check(hi, lo, crc);
}

static void test_crc(void) {
  uint8_t d[512], c[8], c2[8];
  srand(1234);
  for (unsigned v = 0; v < 64; v++) {
    for (unsigned i = 0; i < 512; i++) d[i] = v == 0 ? 0 : v == 1 ? 0xff : rand() & 0xff;
    ref_crc(d, c);
    crc16_nibble_512_nolut(d, c2);
    assert(!memcmp(c, c2, 8));                     /* crc.c (writes) agrees */
    if (v == 0) for (unsigned i = 0; i < 8; i++) assert(c[i] == 0);
    assert(loop_check(d, c, true) == 0 && loop_check(d, c, false) == 0);
    if (v > 3) continue;
    for (unsigned bit = 0; bit < 4096; bit++) {    /* every position, so all four lanes */
      d[bit / 8] ^= 1 << (bit % 8);
      assert(loop_check(d, c, true) != 0 && loop_check(d, c, false) != 0);
      d[bit / 8] ^= 1 << (bit % 8);
    }
    for (unsigned bit = 0; bit < 64; bit++) {      /* a wrong CRC bit */
      c[bit / 8] ^= 1 << (bit % 8);
      assert(loop_check(d, c, true) != 0 && loop_check(d, c, false) != 0);
      c[bit / 8] ^= 1 << (bit % 8);
    }
  }
  /* The old model's constant CRC fails everywhere, also on zero data. */
  memset(d, 0, 512); memset(c, 0xa5, 8);
  assert(loop_check(d, c, true) != 0);
  puts("CRC16: reference = crc.c = read loop steps; 4096 data and 64 CRC bit flips detected PASS");
}

/* ---- 2. Driver ---- */

bool isgba = true, slowsd = false;
static unsigned cmd, arg, stops, reads17, reads18, lastread;
static uint32_t cur;                 /* next sector of the open transfer */
static bool open_rd;
static unsigned fails[64];           /* per sector: mismatches still to give */
static bool all_bad;                 /* every sector mismatches (wrong layout) */

void send_empty_clocks(unsigned n) { assert(n); if (n == 8) open_rd = false; }
bool wait_sdcard_idle(unsigned t) { assert(t); return true; }
bool wait_dat0_idle(unsigned t) { (void)t; return true; }
void send_sdcard_commandbuf(const uint8_t *p, unsigned n) {
  assert(n == 6 && p[5] == crc7(p, 5));
  cmd = p[0] & 63; arg = (unsigned)p[1] << 24 | (unsigned)p[2] << 16 | p[3] << 8 | p[4];
  if (cmd == 17 || cmd == 18) { assert(!open_rd); open_rd = true; cur = lastread = arg; cmd == 17 ? reads17++ : reads18++; }
  if (cmd == 12) { assert(open_rd); open_rd = false; stops++; }
}
bool receive_sdcard_response(uint8_t *p, unsigned n, unsigned t) {
  assert(t); uint8_t r[20] = {0}; r[0] = cmd; r[3] = 9;
  switch (cmd) {
  case 8: r[3] = 1; r[4] = 0xaa; break;
  case 41: r[0] = 63; r[1] = 128 | 64; break;
  case 3: r[1] = 0x12; r[2] = 0x34; r[3] = 6; break;
  case 9: memset(r, 0, 20); r[0] = 63; r[1] = 64; r[9] = 31; r[10] = 255; break;
  }
  if (p) memcpy(p, r, n);
  return true;
}
static uint8_t good(uint32_t lba, unsigned i) { return (uint8_t)(lba * 7 + i * 13 + 1); }
/* Like supercard_io.s: sectors in full; check mode returns at a mismatch
 * (buffer after it), count mode counts it. A mismatching sector has one bit
 * wrong in its data. */
uintptr_t SC_RD_MODE_SYM(w0)(uint8_t *b, unsigned n, unsigned t) {
  assert(open_rd && n && t);
  for (unsigned s = 0; s < n; s++, cur++) {
    for (unsigned i = 0; i < 512; i++) b[512 * s + i] = good(cur, i);
    bool bad = all_bad || (cur < 64 && fails[cur] && fails[cur]--);
    if (!bad) continue;
    b[512 * s + 100] ^= 0x10;
#if GBASD_SC_READ_CRC >= 2
    cur++;
    return (uintptr_t)(b + 512 * (s + 1));
#elif GBASD_SC_READ_CRC == 1
    ((volatile uint32_t *)sdcard_crc_stats())[1]++;   /* the asm adds to word 1 */
#endif
  }
  return 0;
}
uintptr_t SC_RD_MODE_SYM(w1)(uint8_t *b, unsigned n, unsigned t) { return SC_RD_MODE_SYM(w0)(b, n, t); }
int sc_write_sectors_w0(const uint8_t *b, unsigned n, unsigned t) { (void)b; (void)n; (void)t; assert(0); return 1; }
int sc_write_sectors_w1(const uint8_t *b, unsigned n, unsigned t) { return sc_write_sectors_w0(b, n, t); }

static uint8_t buf[16 * 512];
static bool data_ok(uint32_t lba, unsigned n) {
  for (unsigned s = 0; s < n; s++) for (unsigned i = 0; i < 512; i++) if (buf[512 * s + i] != good(lba + s, i)) return false;
  return true;
}
static void init(void) { t_card_info info; memset(fails, 0, sizeof fails); all_bad = false; assert(sdcard_init(&info) == 0); }

int main(void) {
  test_crc();
  const t_sc_crc_stats *st = sdcard_crc_stats();
  assert(offsetof(t_sc_crc_stats, mismatches) == 4);   /* supercard_io.s count mode */
  init();
  assert(st->mode == GBASD_SC_READ_CRC);
#if GBASD_SC_READ_CRC >= 2
  /* Clean reads; then trusted after GBASD_SC_CRC_TRUST matching sectors. */
  assert(sdcard_read_blocks(buf, 0, 16) == 0 && data_ok(0, 16) && st->sectors == 16 && !st->mismatches);
  assert(sdcard_read_blocks(buf, 5, 1) == 0 && data_ok(5, 1) && !open_rd);
  while (st->sectors < GBASD_SC_CRC_TRUST) assert(sdcard_read_blocks(buf, 16, 16) == 0);
  assert(sdcard_crc_stats()->trusted);

  /* Transient: sector 21 mismatches once; re-read from 21, data correct. */
  unsigned s18 = reads18, stp = stops;
  fails[21] = 1;
  assert(sdcard_read_blocks(buf, 16, 16) == 0 && data_ok(16, 16) && !open_rd);
  assert(st->retries == 1 && st->mismatches == 1 && !st->failures && reads18 == s18 + 2 && lastread == 21 && stops == stp + 2);
  /* Twice (all retries used), on the last sector: re-read as CMD17. */
  unsigned s17 = reads17;
  fails[31] = GBASD_SC_READ_RETRIES;
  assert(sdcard_read_blocks(buf, 16, 16) == 0 && data_ok(16, 16) && reads17 == s17 + GBASD_SC_READ_RETRIES && lastread == 31);
  /* Two failing sectors in one read: each gets its retries. */
  fails[17] = GBASD_SC_READ_RETRIES; fails[30] = GBASD_SC_READ_RETRIES;
  assert(sdcard_read_blocks(buf, 16, 16) == 0 && data_ok(16, 16));
  /* Single-sector read. */
  fails[3] = 1;
  assert(sdcard_read_blocks(buf, 3, 1) == 0 && data_ok(3, 1) && !open_rd);
  /* Permanent: a read error, transfer stopped, no wrong data claimed good. */
  unsigned r0 = st->retries;
  fails[20] = 100;
  assert(sdcard_read_blocks(buf, 16, 16) == SD_ERR_BADREAD && !open_rd);
  assert(st->failures == 1 && st->retries == r0 + GBASD_SC_READ_RETRIES && !st->suspects && !st->disabled);
  fails[3] = 100;
  assert(sdcard_read_blocks(buf, 3, 1) == SD_ERR_BADREAD && !open_rd && st->failures == 2);
  fails[20] = fails[3] = 0;
  assert(sdcard_read_blocks(buf, 16, 16) == 0 && data_ok(16, 16));

  /* Before trust: a persistent mismatch is kept as read (suspect). */
  init();
  fails[2] = 100;
  assert(sdcard_read_blocks(buf, 0, 8) == 0 && !open_rd && st->suspects == 1 && !st->disabled && !st->failures);
  assert(buf[512 * 2 + 100] == (good(2, 100) ^ 0x10) && data_ok(3, 0));
  fails[2] = 0;
  for (unsigned i = 0; i < 8; i++) assert(sdcard_read_blocks(buf, 0, 8) == 0);
  assert(sdcard_crc_stats()->trusted && !st->disabled);

  /* Wrong layout (every sector mismatches): switched off after
   * GBASD_SC_CRC_SUSPECTS sectors, reads go on with the data as read. */
  init();
  all_bad = true;
  assert(sdcard_read_blocks(buf, 0, 16) == 0 && !open_rd && st->disabled && st->suspects == GBASD_SC_CRC_SUSPECTS);
  assert(st->retries == GBASD_SC_CRC_SUSPECTS * GBASD_SC_READ_RETRIES && st->sectors == 16 + st->retries);
  unsigned rr = st->retries;
  assert(sdcard_read_blocks(buf, 40, 16) == 0 && sdcard_read_blocks(buf, 7, 1) == 0 && st->retries == rr && !st->failures);
  init();
  assert(!st->disabled && !st->suspects && !st->sectors);   /* a new session */
  puts("Driver: retry from the failing sector, read error when permanent, safety switch PASS");
#elif GBASD_SC_READ_CRC == 1
  fails[3] = 1; fails[9] = 5;
  assert(sdcard_read_blocks(buf, 0, 16) == 0 && st->sectors == 16 && st->mismatches == 2 && !st->retries);
  assert(sdcard_read_blocks(buf, 9, 1) == 0 && st->mismatches == 3 && !open_rd);
  puts("Driver: count-only mode counts, no retry PASS");
#else
  assert(sdcard_read_blocks(buf, 0, 16) == 0 && data_ok(0, 16) && !st->sectors);
  puts("Driver: CRC off PASS");
#endif
  return 0;
}
