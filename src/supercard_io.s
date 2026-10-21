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

// gbaSD: GBASD_SC_READ_CRC, GBASD_SC_READ_UNROLL (path relative to this
// file: Butano passes no include path to assembly).
#include "../include/gbasd_config.h"

// Place the routines in IWRAM, they are perf critical for fast SD operations.
.section .iwram.text, "ax", %progbits
.align 4
.arm

// In general we use the 0x8-9 mapping, but A-B for data reads.
#define SC_MIRROR_BASE_8         0x08000000
#define SC_MIRROR_BASE_A         0x0A000000

// Used to perform writes (but also reads?) to the bus
#define SC_WRITE_REGISTER_8     (SC_MIRROR_BASE_8 + 0x01000000)
#define SC_WRITE_REGISTER_A     (SC_MIRROR_BASE_A + 0x01000000)
// Used to poll for reads (can be slow)
#define SC_READ_REGISTER_16     (SC_MIRROR_BASE_8 + 0x01100000)
// Used to read data from SD card.
#define SC_READ_REGISTER_8      (SC_MIRROR_BASE_8 + 0x01100000)
#define SC_READ_REGISTER_A      (SC_MIRROR_BASE_A + 0x01100000)
#define SCLITE_DATA_REGISTER32  (SC_MIRROR_BASE_8 + 0x01200000)

// Used to send/received commands.
#define SC_RDWR_COMMAND         (SC_MIRROR_BASE_8 + 0x01800000)
// ^^ CMD interface (bit 0 is an input pin, bit 7 is the output bit)

#define SD_DATA0                0x0100

.global sc_read_sectors_w0   // Version that uses 0x08000000-0x09FFFFFF addrs
.global sc_read_sectors_w1   // Version that uses 0x0A000000-0x0BFFFFFF addrs

// Second name of each routine with the CRC mode in it: the driver calls
// these, so C and assembly built with different GBASD_SC_READ_CRC do not link.
#define SC_RD_CAT2(a, b, c) a ## b ## c
#define SC_RD_CAT(a, b, c) SC_RD_CAT2(a, b, c)
#define SC_RD_MODE_SYM(w) SC_RD_CAT(sc_read_sectors_crc, GBASD_SC_READ_CRC, _ ## w)
.global SC_RD_MODE_SYM(w0)
.global SC_RD_MODE_SYM(w1)

#if !defined(SUPERCARD_LITE_IO) && !defined(SUPERCHIS_IO)

// Reads N 512 byte sectors mapped via ROM mappings into the provided buffer
// as fast as possible. Assumes that the read operation was issued and that
// it needs to wait for the block (by detecting the first zero bit).
// Took inspiration from SCFW and we use swp instructions to perform 2x32 bit
// accesses, with a rotated reg (unaligned addr) to avoid one rotr.

// gbaSD: the CRC16 of each sector (GBASD_SC_READ_CRC, see gbasd_config.h)
// is computed while it is read and compared with the 8 CRC bytes the card
// sends after it. The four data lines carry four CRC16-CCITT (one per line,
// bits interleaved in the nibbles); the 64 bit state of the four, interleaved
// the same way, is kept in r1 (high word) and lr (low word). SC_CRC_WORD adds
// a 32 bit little-endian data word to it (byte swap folded in, 8 cycles). The
// CRC bytes arrive like data bytes, so after a sector the state must equal
// them (byte swapped): SC_CRC_CHECK.
//  Check mode (2): on a mismatch the routine returns at once, with the
//  sector read in full (the CMD18 stays open), and the return value is the
//  buffer address after that sector. The driver stops or resumes the transfer.
//  Count mode (1): mismatches are counted (gbasd_sc_crc_stats.mismatches)
//  and the transfer goes on.
// The data lanes: the card sends the high nibble of each byte first; the
// latch places byte pairs in the top half of every second word.

// r0: output byte buffer (output)
// r1: number of blocks to read
// r2: number of timeout iterations when waiting for data.
// Returns 0 (done), 1 (timeout waiting for data) or, check mode only, the
// buffer address after the sector whose CRC did not match.

#if GBASD_SC_READ_UNROLL != 1 && GBASD_SC_READ_UNROLL != 2 && GBASD_SC_READ_UNROLL != 4 && \
    GBASD_SC_READ_UNROLL != 8 && GBASD_SC_READ_UNROLL != 16 && GBASD_SC_READ_UNROLL != 32
#error "GBASD_SC_READ_UNROLL: 1, 2, 4, 8, 16 or 32"
#endif

#if GBASD_SC_READ_CRC

// CRC state (hi, lo) += 32 bit word x (little-endian bytes). t1, t2: scratch.
// u = hi ^ bswap(x); u ^= u >> 16; hi = lo ^ u >> 12 ^ u << 16; lo = u ^ u << 20
.macro SC_CRC_WORD x, hi, lo, t1, t2
  eor \t1, \x, \x, ror #16
  bic \t1, \t1, #0x00FF0000
  eor \t2, \hi, \x, ror #8
  eor \t2, \t2, \t1, lsr #8      // t2 = hi ^ bswap(x)
  eor \t2, \t2, \t2, lsr #16     // Propagate overlapping bits
  eor \hi, \lo, \t2, lsr #12
  eor \hi, \hi, \t2, lsl #16
  eor \lo, \t2, \t2, lsl #20
.endm

// Compares the state with the received CRC bytes (r7, r11, little-endian):
// Z set when both words match. Uses r2.
.macro SC_CRC_CHECK
  eor r2, r7, r7, ror #16
  bic r2, r2, #0x00FF0000
  eor r1, r1, r7, ror #8
  eor r1, r1, r2, lsr #8         // r1 ^= bswap(r7)
  eor r2, r11, r11, ror #16
  bic r2, r2, #0x00FF0000
  eor lr, lr, r11, ror #8
  eor lr, lr, r2, lsr #8         // lr ^= bswap(r11)
  orrs r1, r1, lr
.endm

// On a mismatch: check mode returns (r0: buffer after the sector), count
// mode adds one to gbasd_sc_crc_stats.mismatches (second word).
.macro SC_CRC_RESULT
#if GBASD_SC_READ_CRC >= 2
  bne 3f
#else
  ldrne r2, =(gbasd_sc_crc_stats + 4)
  ldrne r3, [r2]
  addne r3, r3, $1
  strne r3, [r2]
#endif
.endm

// Saved registers: r1 (sectors left) at [sp], r2 (timeout) at [sp, #4];
// lr holds half of the CRC state.
#define SC_RD_SAVE   {r1, r2, r4-r12, lr}

#else

#define SC_RD_SAVE   {r1, r2, r4-r12}

#endif
#define SC_RD_TIMEOUT [sp, #4]

.type sc_read_sectors_w0,function
sc_read_sectors_w0:
SC_RD_MODE_SYM(w0):
  push SC_RD_SAVE

  ldr r5, =SC_READ_REGISTER_8
  b 6f

.type sc_read_sectors_w1,function
sc_read_sectors_w1:
SC_RD_MODE_SYM(w1):
  push SC_RD_SAVE

  ldr r5, =SC_READ_REGISTER_A

6:
  ldr r12, =0x0000FFFF       // Half word mask (also for the CRC bytes)
  tst r0, $3                 // Check if the buffer address is aligned
  bne 5f                     // Branch to the slow routine.

1:
  // Perform a wait on the data bus, until the bits are pulled zero once.
  mov r3, $(SC_READ_REGISTER_16)
  ldr r2, SC_RD_TIMEOUT
  2:
    subs r2, r2, $1   // Decrement timeout
    moveq r0, $1      // Non zero retvalue on timeout error
    beq 3f            // Function return.
    ldrh r4, [r3]
    tst r4, $(SD_DATA0)
  bne 2b

  // Go ahead and read a 512 byte sector
  mov r4, $(512 / 8 / GBASD_SC_READ_UNROLL)    // Iterations per 512 byte block
#if GBASD_SC_READ_CRC
  mov r1, $0                 // CRC state
  mov lr, $0
#endif

  2:
    .rept GBASD_SC_READ_UNROLL
      // Reads 64 bits (even regs are mostly garbage)
      ldmia r5, {r2, r3, r6, r7, r8, r9, r10, r11}
      // Regs r2, r6, r8, r10 are garbage
      bic  r7, r12
      bic r11, r12
      // Merge half words (after clearing the LSB)
      orr  r7,  r7, r3, lsr #16
      orr r11, r11, r9, lsr #16
      // Write 8 bytes to the output buffer
      stmia r0!, {r7, r11}
#if GBASD_SC_READ_CRC
      SC_CRC_WORD  r7, r1, lr, r2, r6
      SC_CRC_WORD r11, r1, lr, r2, r6
#endif
    .endr

    subs r4, r4, $1
    bne 2b

  // Reads 64 bits (8 bytes of checksum) in one go.
  ldmia r5, {r2, r3, r6, r7, r8, r9, r10, r11}

  ldrh r2, [r5]       // Final clock: should go all high (0xF)

#if GBASD_SC_READ_CRC
  // The CRC bytes, merged like data
  bic  r7, r12
  bic r11, r12
  orr  r7,  r7, r3, lsr #16
  orr r11, r11, r9, lsr #16
  SC_CRC_CHECK
  SC_CRC_RESULT
#endif

  ldr r1, [sp]
  subs r1, r1, $1
  str r1, [sp]
  bne 1b

  mov r0, $0            // Return value: success
3:
  pop SC_RD_SAVE
  bx lr

  // Unaligned code! It should be slower (but also smaller)
5:
  // Perform a wait on the data bus, until the bits are pulled zero once.
  mov r3, $(SC_READ_REGISTER_16)
  ldr r2, SC_RD_TIMEOUT
  2:
    subs r2, r2, $1   // Decrement timeout.
    moveq r0, $1      // Non zero retvalue on timeout error
    beq 3f            // Function return.
    ldrh r4, [r3]
    tst r4, $(SD_DATA0)
  bne 2b

  // Go ahead and read a 512 byte sector
  mov r4, $(512 / 4)     // Performs four bytes per iteration
  ldr r5, =SC_READ_REGISTER_8
#if GBASD_SC_READ_CRC
  mov r1, $0             // CRC state
  mov lr, $0
#endif

  2:
    ldmia r5, {r2, r3, r6, r7}   // Reads four bytes in one go.
    lsr r2, r2, #24      // Places in the top 8 bits of the register
    lsr r3, r3, #24
    lsr r6, r6, #24
    lsr r7, r7, #24
    strb r2, [r0], #1    // Stores the received bytes
    strb r3, [r0], #1
    strb r6, [r0], #1
    strb r7, [r0], #1
#if GBASD_SC_READ_CRC
    // Big-endian word of the four bytes (no byte swap needed), then the
    // second half of SC_CRC_WORD.
    orr r7, r7, r6, lsl #8
    orr r7, r7, r3, lsl #16
    orr r7, r7, r2, lsl #24
    eor r2, r1, r7
    eor r2, r2, r2, lsr #16
    eor r1, lr, r2, lsr #12
    eor r1, r1, r2, lsl #16
    eor lr, r2, r2, lsl #20
#endif
    subs r4, r4, $1
    bne 2b

  // Reads 64 bits (8 bytes of checksum) in one go.
  ldmia r5, {r2, r3, r6, r7, r8, r9, r10, r11}

  ldrh r2, [r5]       // Final clock: should go all high (0xF)

#if GBASD_SC_READ_CRC
  // The CRC bytes, merged like aligned data
  bic  r7, r12
  bic r11, r12
  orr  r7,  r7, r3, lsr #16
  orr r11, r11, r9, lsr #16
  SC_CRC_CHECK
  SC_CRC_RESULT
#endif

  ldr r1, [sp]
  subs r1, r1, $1
  str r1, [sp]
  bne 5b

  mov r0, $0            // Return value: success
3:
  pop SC_RD_SAVE
  bx lr

#else

// This is the same as above, but using the Supercard Lite faster I/O interface.
// The cart clocks data for us and we can read it in 32 bit blocks (we assume
// that it clocks the SD card 8 times under the hood). We can't use the faster
// waitstates otherwise it doesn't have enough time to do so.

// r0: output byte buffer (output)
// r1: number of blocks to read
// r2: number of timeout iterations when waiting for data.

.type sc_read_sectors_w0,function
sc_read_sectors_w0:
SC_RD_MODE_SYM(w0):
.type sc_read_sectors_w1,function
sc_read_sectors_w1:
SC_RD_MODE_SYM(w1):

  push {r2, r4-r11}
  ldr r5, =SCLITE_DATA_REGISTER32

6:
  tst r0, $3                 // Check if the buffer address is aligned
  bne 5f                     // Branch to the slow routine.

1:
  // Perform a wait on the data bus, until the bits are pulled zero once.
  mov r4, $(SC_READ_REGISTER_16)
  ldr r2, [sp]
  2:
    subs r2, r2, $1   // Decrement timeout iteration
    moveq r0, $1      // Non zero retvalue on timeout error
    beq 3f            // Function return.
    ldrh r3, [r4]
    tst r3, $(SD_DATA0)
  bne 2b

  ldrh r2, [r5]       // Discard the first 32 bits

  // Go ahead and read a 512 byte sector
  // Repeat 16 * 32 bytes = 512 bytes.
  .rept 16
    // Reads 256 bits
    ldmia r5, {r2, r3, r6, r7, r8, r9, r10, r11}
    // Write the data back to the buffer
    stmia r0!, {r2, r3, r6, r7, r8, r9, r10, r11}
  .endr

  // Reads 64 bits (8 bytes of checksum) in one go.
  ldmia r5, {r2, r3}

  ldrh r2, [r4]       // Final clock: should go all high (0xF)

  subs r1, r1, $1
  bne 1b

  mov r0, $0            // Return value: success
3:
  pop {r2, r4-r11}
  bx lr

  // Unaligned code! It should be slower (but also smaller)
5:
  // Perform a wait on the data bus, until the bits are pulled zero once.
  mov r4, $(SC_READ_REGISTER_16)
  ldr r2, [sp]
  2:
    subs r2, r2, $1   // Decrement timeout iteration
    moveq r0, $1      // Non zero retvalue on timeout error
    beq 3f            // Function return.
    ldrh r3, [r4]
    tst r3, $(SD_DATA0)
  bne 2b

  ldrh r2, [r5]       // Discard the first 32 bits

  // Go ahead and read a 512 byte sector
  mov r6, $(512 / 8)     // Performs eight bytes per iteration

  2:
    ldmia r5, {r2, r3}   // Reads 8 bytes in one go.
    strb r2, [r0], #1    // Stores the received byte
    lsr r2, #8
    strb r2, [r0], #1
    lsr r2, #8
    strb r2, [r0], #1
    lsr r2, #8
    strb r2, [r0], #1
    strb r3, [r0], #1
    lsr r3, #8
    strb r3, [r0], #1
    lsr r3, #8
    strb r3, [r0], #1
    lsr r3, #8
    strb r3, [r0], #1

    subs r6, r6, $1
    bne 2b

  // Reads 64 bits (8 bytes of checksum) in one go.
  ldmia r5, {r2, r3}

  ldrh r2, [r4]       // Final clock: should go all high (0xF)

  subs r1, r1, $1
  bne 5b

  mov r0, $0            // Return value: success
3:
  pop {r2, r4-r11}
  bx lr

#endif



.pool

// Command-related routines
// These are placed here so they can be reimplemented on other modes.

// gbaSD: ROM (Thumb) by default; GBASD_SC_CMD_IN_IWRAM puts them in IWRAM.
#ifdef GBASD_SC_CMD_IN_IWRAM
.section .iwram.text, "ax", %progbits
#else
.text
#endif
.align 4
.thumb

.global send_empty_clocks
.thumb_func
// Sends empty clocks (just read the CMD register to force a clock)
// r0: number of clocks to send
send_empty_clocks:
  ldr r1, =SC_RDWR_COMMAND
  1:
    ldrh r2, [r1]
    sub r0, $1
    bne 1b
  bx lr

.global wait_sdcard_idle
.thumb_func
// Waits until the CMD line (bit0) goes high, indicating IDLE.
// r0: number of iterations before giving up
wait_sdcard_idle:
  ldr r1, =SC_RDWR_COMMAND
  mov r3, r0
  mov r0, $1
  1:
    ldrh r2, [r1]
    lsl r2, #31        // Move LSB to MSB
    bne 2f             // If LSB is set, return true (Idle reached)
    sub r3, $1         // Decrement timeout.
    bne 1b

  mov r0, $0
2:bx lr

.global wait_dat0_idle
.thumb_func
// Waits until the DAT0 line goes high, indicating not busy.
// r0: number of iterations before giving up
wait_dat0_idle:
  ldr r1, =SC_WRITE_REGISTER_8
  ldr r3, =SD_DATA0
  1:
    ldrh r2, [r1]
    tst r2, r3         // Check the right bit (SD_DATA0)
    bne 2f             // If bit set, it is idlea again, return true
    sub r0, $1         // Decrement timeout.
    bne 1b

  mov r0, $0
  bx lr
2:
  mov r0, $1
  bx lr


.global receive_sdcard_response
.thumb_func
// Waits for a response (or returns error) and receives a response to a buffer.
// r0: buffer pointer where we write the response (can be null!)
// r1: response length in bytes
// r2: number of iterations before giving up
receive_sdcard_response:
  push {r4-r6}
  ldr r4, =SC_RDWR_COMMAND

  mov r6, $7           // First bit belongs to first byte (always zero)
  mov r5, $0

  1:
    ldrh r3, [r4]
    lsr r3, #1         // Extract LSB to Carry Flag
    bcc 4f             // If LSB is clear, we can receive the next bits!
    sub r2, $1         // Decrement timeout.
    bne 1b

  mov r0, $0           // Timeout, return false
  pop {r4-r6}
  bx lr

  3:

    // Read 8 bits (MSB to LSB)
    mov r6, $8
    4:
      ldrh r3, [r4]
      lsr r3, #1       // Extract LSB into Carry Flag
      adc r5, r5       // Shift register left by 1, add CF to LSB
      sub r6, $1
      bne 4b

    cmp r0, $0
    beq 5f

    strb r5, [r0]      // Store it in the buffer, if ptr!=null
    add r0, $1

    5:
    sub r1, $1
    bne 3b

  mov r0, $1           // Return true
  pop {r4-r6}
  bx lr


.global send_sdcard_commandbuf
.thumb_func
// Waits for a response (or returns error) and receives a response to a buffer.
// r0: buffer pointer to send
// r1: command length in bytes
send_sdcard_commandbuf:
  ldr r3, =SC_RDWR_COMMAND

  1:
    ldrb r2, [r0]
    add r0, $1
    .rept 8
      strh r2, [r3]
      lsl r2, #1
    .endr

    sub r1, $1
    bne 1b

  bx lr

.pool

