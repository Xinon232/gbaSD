/*
 * Copyright (C) 2024 David Guillen Fandos <david@davidgf.net>
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef _SUPERCARD_DRIVER_H_
#define _SUPERCARD_DRIVER_H_

#include <stdint.h>
#include <stdbool.h>


typedef struct {
  uint32_t block_cnt;        // Size (in 512byte blocks)
  bool sdhc;                 // Is SDHC (or SDSC otherwise)
  uint8_t manufacturer;      // 8 bit ID
  uint16_t oemid;            // Product ID.
} t_card_info;

#define MAPPED_FIRMWARE      0
#define MAPPED_SDRAM         1

// Generic supercard function to change mapping mode
void write_supercard_mode(uint16_t modebits);
void set_supercard_mode(unsigned mapped_area, bool write_access, bool sdcard_interface);

// Full SD card init
unsigned sdcard_init(t_card_info *info);

bool sc_issdhc();
uint16_t sc_rca();

unsigned sdcard_read_blocks(uint8_t *buffer, uint32_t blocknum, unsigned blkcnt);
unsigned sdcard_write_blocks(const uint8_t *buffer, uint32_t blocknum, unsigned blkcnt);

// gbaSD: read CRC16 check counters (GBASD_SC_READ_CRC), since sdcard_init().
// Diagnostics only; the driver keeps them up to date.
typedef struct {
  uint32_t sectors;      // sectors read with the check on (each attempt)
  uint32_t mismatches;   // of these: CRC did not match (supercard_io.s
                         // count mode adds to this word: keep it second)
  uint32_t retries;      // re-reads after a mismatch (check mode)
  uint32_t failures;     // reads failed after all retries (SD_ERR_BADREAD)
  uint32_t suspects;     // mismatching sectors returned as read before the
                         // CRC layout was trusted (safety switch)
  uint8_t mode;          // GBASD_SC_READ_CRC: 0 off, 1 count only, 2 check
  uint8_t trusted;       // GBASD_SC_CRC_TRUST sectors matched
  uint8_t disabled;      // safety switch: check off until the next init
} t_sc_crc_stats;

const t_sc_crc_stats *sdcard_crc_stats(void);

#define SD_ERR_NO_STARTUP       1
#define SD_ERR_BAD_IDENT        2
#define SD_ERR_BAD_INIT         3
#define SD_ERR_BAD_CAP          4
#define SD_ERR_BAD_MODEXCH      5
#define SD_ERR_BAD_BUSSEL       6

#define SD_ERR_BADREAD          8
#define SD_ERR_BADWRITE         9
#define SD_ERR_READTIMEOUT     10
#define SD_ERR_WRITETIMEOUT    11

#endif

