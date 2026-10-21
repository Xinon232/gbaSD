/* gbaSD build settings. Every setting has a default; an app overrides one by
 * defining it on the compiler command line (-DNAME=value).
 * Part of gbaSD. GPL-3.0-or-later, see LICENSE. */
#ifndef GBASD_CONFIG_H
#define GBASD_CONFIG_H

/* ---- SuperCard SD ---- */

/* Polls to wait for the card to be ready for a command / for read data.
 * The defaults suit apps that can wait (about 1 s for data); a music player
 * that must not stall uses shorter ones (gbamp3: 0x8000 / 0x4000). */
#ifndef GBASD_SC_IDLE_TIMEOUT
#define GBASD_SC_IDLE_TIMEOUT 0x800000
#endif
#ifndef GBASD_SC_DATA_TIMEOUT
#define GBASD_SC_DATA_TIMEOUT 0x400000
#endif

/* Extra attempts after a failed multi-block write (the same data to the same
 * sectors). 0: no retry, the caller decides (gbamp3). */
#ifndef GBASD_SC_WRITE_RETRIES
#define GBASD_SC_WRITE_RETRIES 2
#endif

/* Largest write accepted in one call, in sectors. 0: no limit. */
#ifndef GBASD_SC_MAX_WRITE_BLOCKS
#define GBASD_SC_MAX_WRITE_BLOCKS 0
#endif

/* Read CRC16 check (supercard_io.s computes the CRC of every sector while
 * reading it and compares it with the 8 CRC bytes the card sends after it):
 *   2  check: a sector whose CRC does not match is read again (the transfer
 *      is stopped and restarted at that sector), up to GBASD_SC_READ_RETRIES
 *      more times; still wrong: the read fails (SD_ERR_BADREAD). Default.
 *   1  count only: mismatches are counted (sdcard_crc_stats()), the data is
 *      returned as read (diagnostics: tools/crc_diag).
 *   0  off: no CRC (the loop as before, without the CRC instructions).
 * A plain number: the assembly pastes it into a symbol name, so C and
 * assembly built with different values do not link. Butano apps: Butano does
 * not pass USERFLAGS to assembly, so set it in both USERFLAGS and
 * USERASFLAGS (the same holds for GBASD_SC_READ_UNROLL). */
#ifndef GBASD_SC_READ_CRC
#define GBASD_SC_READ_CRC 2
#endif

/* 8-byte reads per loop iteration in the aligned read loop (1, 2, 4, 8, 16
 * or 32). More is a little faster and larger, in IWRAM. With the CRC, at
 * WAITCNT 0x40c0 into EWRAM (emulator): 2 = 5413 cycles per sector, 572 B of
 * code; 4 = 5349, 748 B; 16 = 5301, 1804 B (no CRC: 4256 cycles, 588 B).
 * Default 4; gbamp3 (IWRAM full) uses 2. */
#ifndef GBASD_SC_READ_UNROLL
#if GBASD_SC_READ_CRC
#define GBASD_SC_READ_UNROLL 4
#else
#define GBASD_SC_READ_UNROLL 16
#endif
#endif

/* Check mode: extra reads of a sector whose CRC did not match. */
#ifndef GBASD_SC_READ_RETRIES
#define GBASD_SC_READ_RETRIES 2
#endif

/* Safety switch (check mode). That the CRC bytes arrive through the SuperCard
 * latch like data bytes is inferred, not yet proven on every cart. If it is
 * wrong for a cart, nearly every sector mismatches, so: until
 * GBASD_SC_CRC_TRUST sectors have matched since sdcard_init(), a sector that
 * still mismatches after its retries is returned as read (as without the
 * check) and counted as suspect; GBASD_SC_CRC_SUSPECTS suspect sectors
 * (different reads) in that time switch the check off until the next
 * sdcard_init(). After GBASD_SC_CRC_TRUST matches the layout is proven for
 * this cart and a persistent mismatch is a read error. A count, not "zero
 * matches": an all-zero sector has CRC 0, which a wrong layout may also
 * read as 0. */
#ifndef GBASD_SC_CRC_TRUST
#define GBASD_SC_CRC_TRUST 64
#endif
#ifndef GBASD_SC_CRC_SUSPECTS
#define GBASD_SC_CRC_SUSPECTS 3
#endif

/* Placement of the speed-critical C functions (command send, block read)
 * and of the CRC routines. Empty: wherever the build puts code (ROM).
 * gbamp3 puts them in IWRAM / EWRAM sections that its linker script maps. */
#ifndef GBASD_SC_FAST_FN
#define GBASD_SC_FAST_FN
#endif
#ifndef GBASD_SC_CRC7_FN
#define GBASD_SC_CRC7_FN
#endif
#ifndef GBASD_SC_CRC16_FN
#define GBASD_SC_CRC16_FN
#endif

/* Assembly placement (supercard_io.s / supercard_write.s):
 *   GBASD_SC_CMD_IN_IWRAM   command routines in IWRAM (default: ROM, Thumb)
 *   GBASD_SC_WRITE_IN_EWRAM sector writer in EWRAM (default: IWRAM) */

#endif
