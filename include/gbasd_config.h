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
