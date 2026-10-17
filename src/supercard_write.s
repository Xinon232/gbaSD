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

// SuperCard SD sector writer (SuperFW). gbaSD: the 8-byte stack alignment at
// the C CRC calls is kept (r3 pushed as padding); IWRAM by default,
// GBASD_SC_WRITE_IN_EWRAM puts it in EWRAM (gbamp3).
#ifdef GBASD_SC_WRITE_IN_EWRAM
.section .ewram, "ax", %progbits
#else
.section .iwram.text, "ax", %progbits
#endif
.align 4
.arm
#define SC_WRITE_REGISTER_8 0x09000000
#define SC_WRITE_REGISTER_A 0x0B000000
#define SD_DATA0 0x0100
.global sc_write_sectors_w0
.global sc_write_sectors_w1
.type sc_write_sectors_w0,function
sc_write_sectors_w0:
  push {r3-r7, lr}
  mov r6, $(SC_WRITE_REGISTER_8)
  b 9f

.type sc_write_sectors_w1,function
sc_write_sectors_w1:
  push {r3-r7, lr}
  mov r6, $(SC_WRITE_REGISTER_A)

9:
  sub sp, $8    // Some space for the CRC buffer
  mov r4, r0    // Input buffer
  mov r5, r1    // Block count
  mov r7, r2    // Save the timeout count.

  // Calculate crc for the first block.
  mov r1, sp
  bl crc16_nibble_512_nolut

  1: // Loop r5 times. Try to perform checksum while waiting for write to finish.

    // Go ahead send the data! Send the first byte token (0xFE? should be 0xFC!)
    mov r1, $0
    mov r2, $0xFFFFFFFF
    str r2, [r6]
    str r2, [r6]
    str r2, [r6]
    strh r2, [r6]
    strh r1, [r6]

    mov r1, $(512 / 2 / 8)   // Pushing 512 bytes in halfwords (unrolled x8)
    2:
      .rept 16
        ldrb r2, [r4], #1    // Load byte from buffer
        str r2, [r6]
      .endr
      subs r1, r1, $1
      bne 2b

    .set i, 0
    .rept 8                  // Send 16x4 bits of checksum
      ldrb r2, [sp, #(i)]
      str r2, [r6]
      .set i, i+1
    .endr

    // Response token is 8 bits, only transmitted via DAT0.
    // Write one to the bus, read a couple more times (ignore 3 MSB)
    mov  r2, $(~0)            // All data lines high after last bit
    strh r2, [r6]
    strh r2, [r6]
    strh r2, [r6]
    strh r2, [r6]

    // Get status code (3 bits) + dummy bit
    mov r0, $0
    .rept 4
      lsl r0, $1
      ldrh r2, [r6]
      tst r2, $SD_DATA0
      orrne r0, $0x1
    .endr

    // Pipe one byte, some cards do not start actually writing (ie. go busy?)
    // until we pipe an extra clock? Perhaps not, but does no harm.
    .rept 4
      ldr r2, [r6]
    .endr

    // Check status code, bail on error
    cmp r0, $0x5              // 0.010.1 means "data accepted" (0.status.1)
    bne 3f                    // Wait and exit with error

    // Break out if we are on the last block!
    subs r5, $1
    beq 5f

    // Calculate the block checksum for the next block to send.
    // (helps hiding busy latency!)
    mov r0, r4
    mov r1, sp
    bl crc16_nibble_512_nolut   // Must be in IWRAM as well!

    // Perform a wait on the data bus, DAT0 goes high when ready.
    mov r1, r7
    2:
      subs r1, r1, $1
      beq 4f
      ldrh r2, [r6]
      tst r2, $(SD_DATA0)
    beq 2b

    b 1b  // Continue loop on the next block.

5:
  // Perform a final wait, ensure we leave with no pending operations.
  2:
    subs r7, r7, $1
    beq 4f
    ldrh r2, [r6]
    tst r2, $(SD_DATA0)
  beq 2b

  add sp, $8
  pop {r3-r7, lr}
  mov r0, $0            // Return OK
  bx lr

3:  // Wait-and-exit on error
  2:
    subs r7, r7, $1
    beq 4f
    ldrh r2, [r6]
    tst r2, $(SD_DATA0)
  beq 2b

4:  // Return non zero on timeout/response error
  add sp, $8
  pop {r3-r7, lr}
  mov r0, $1
  bx lr
.pool
