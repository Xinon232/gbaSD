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

#if !defined(SUPERCARD_LITE_IO) && !defined(SUPERCHIS_IO)

// Reads N 512 byte sectors mapped via ROM mappings into the provided buffer
// as fast as possible. Assumes that the read operation was issued and that
// it needs to wait for the block (by detecting the first zero bit).
// Took inspiration from SCFW and we use swp instructions to perform 2x32 bit
// accesses, with a rotated reg (unaligned addr) to avoid one rotr.

// r0: output byte buffer (output)
// r1: number of blocks to read
// r2: number of timeout iterations when waiting for data.

.type sc_read_sectors_w0,function
sc_read_sectors_w0:
  push {r2, r4-r12}

  ldr r5, =SC_READ_REGISTER_8
  b 6f

.type sc_read_sectors_w1,function
sc_read_sectors_w1:
  push {r2, r4-r12}

  ldr r5, =SC_READ_REGISTER_A

6:
  tst r0, $3                 // Check if the buffer address is aligned
  bne 5f                     // Branch to the slow routine.

  ldr r12, =0x0000FFFF

1:
  // Perform a wait on the data bus, until the bits are pulled zero once.
  mov r3, $(SC_READ_REGISTER_16)
  ldr r2, [sp]
  2:
    subs r2, r2, $1   // Decrement timeout
    moveq r0, $1      // Non zero retvalue on timeout error
    beq 3f            // Function return.
    ldrh r4, [r3]
    tst r4, $(SD_DATA0)
  bne 2b

  // Go ahead and read a 512 byte sector
  mov r4, $(512 / 16 / 8)    // Performs 4 iterations per 512 byte block

  2:
    .rept 16
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
    .endr

    subs r4, r4, $1
    bne 2b

  // Reads 64 bits (8 bytes of checksum) in one go.
  ldmia r5, {r2, r3, r6, r7, r8, r9, r10, r11}

  ldrh r2, [r5]       // Final clock: should go all high (0xF)

  subs r1, r1, $1
  bne 1b

  mov r0, $0            // Return value: success
3:
  pop {r2, r4-r12}
  bx lr

  // Unaligned code! It should be slower (but also smaller)
5:
  // Perform a wait on the data bus, until the bits are pulled zero once.
  mov r3, $(SC_READ_REGISTER_16)
  ldr r2, [sp]
  2:
    subs r2, r2, $1   // Decrement timeout.
    moveq r0, $1      // Non zero retvalue on timeout error
    beq 3f            // Function return.
    ldrh r4, [r3]
    tst r4, $(SD_DATA0)
  bne 2b

  // Go ahead and read a 512 byte sector
  mov r4, $(512 / 2)     // Performs two bytes per iteration
  ldr r5, =SC_READ_REGISTER_8

  2:
    ldmia r5, {r2, r3}   // Reads two bytes in one go.
    lsr r2, r2, #24      // Places in the top 8 bits of the register
    lsr r3, r3, #24
    strb r2, [r0], #1    // Stores the received byte
    strb r3, [r0], #1
    subs r4, r4, $1
    bne 2b

  // Reads 64 bits (8 bytes of checksum) in one go.
  ldmia r5, {r2, r3, r6, r7, r8, r9, r10, r11}

  ldrh r2, [r5]       // Final clock: should go all high (0xF)

  subs r1, r1, $1
  bne 5b

  mov r0, $0            // Return value: success
3:
  pop {r2, r4-r12}
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
.type sc_read_sectors_w1,function
sc_read_sectors_w1:

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

