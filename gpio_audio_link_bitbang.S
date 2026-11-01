.syntax unified
.cpu cortex-m33
.thumb

/*
 * Cycle-controlled, software-only, full-duplex GPIO transfer routines.
 *
 * ABI arguments for both functions:
 *   r0 = gpio_bitbang_context_t *
 *   r1 = transmit bytes
 *   r2 = receive bytes
 *   r3 = byte count
 *
 * gpio_bitbang_context_t offsets:
 *   0  gpio_in register address
 *   4  gpio_set register address
 *   8  gpio_clr register address
 *   12 CS mask
 *   16 SCK mask
 *   20 MOSI mask
 *   24 MISO mask
 *
 * The master emits CPOL=0 / CPHA=0. MOSI changes while SCK is low,
 * the slave samples MOSI on rising SCK, and the master samples MISO
 * during the high phase. The slave changes MISO after each falling edge.
 *
 * These routines run from SRAM in .time_critical.* and preserve r4-r11.
 * Interrupts are disabled by the caller for the duration of one frame.
 */

/* Master: eight unrolled bits per byte. */
.macro MASTER_BIT
    lsls    r10, r10, #1          /* old bit 7 -> carry */
    it      cs
    strcs   r7, [r4]              /* MOSI high */
    it      cc
    strcc   r7, [r5]              /* MOSI low */
    nop
    nop
    str     r9, [r4]              /* SCK rising edge */
    .rept 10
      nop
    .endr
    ldr     r12, [r6]              /* synchronized GPIO input sample */
    lsls    r0, r0, #1
    tst     r12, r8
    it      ne
    orrne   r0, r0, #1            /* append MISO bit */
    str     r9, [r5]              /* SCK falling edge */
    .rept 10
      nop
    .endr
.endm

.section .time_critical.gpio_bitbang_master_transfer_asm,"ax",%progbits
.align 2
.global gpio_bitbang_master_transfer_asm
.type gpio_bitbang_master_transfer_asm, %function
.thumb_func
gpio_bitbang_master_transfer_asm:
    push    {r4-r11, lr}
    ldr     r6, [r0, #0]          /* GPIO_IN */
    ldr     r4, [r0, #4]          /* GPIO_SET */
    ldr     r5, [r0, #8]          /* GPIO_CLR */
    ldr     r7, [r0, #20]         /* MOSI mask */
    ldr     r8, [r0, #24]         /* MISO mask */
    ldr     r9, [r0, #16]         /* SCK mask */

.Lmaster_byte:
    ldrb    r10, [r1], #1
    movs    r0, #0                /* received byte */
    .rept 8
      MASTER_BIT
    .endr
    strb    r0, [r2], #1
    subs    r3, r3, #1
    bne     .Lmaster_byte

    movs    r0, #0
    pop     {r4-r11, pc}
.size gpio_bitbang_master_transfer_asm, .-gpio_bitbang_master_transfer_asm

/* Slave edge primitive. r0 accumulates the received byte. */
.macro SLAVE_SAMPLE_EDGE
.Lslave_wait_rise\@:
    ldr     r12, [r4]
    tst     r12, r7
    bne     .Lslave_abort
    tst     r12, r8
    beq     .Lslave_wait_rise\@
    lsls    r0, r0, #1
    tst     r12, r9
    it      ne
    orrne   r0, r0, #1
.Lslave_wait_fall\@:
    ldr     r12, [r4]
    tst     r12, r7
    bne     .Lslave_abort
    tst     r12, r8
    bne     .Lslave_wait_fall\@
.endm

/* After a falling edge, advance current TX byte and present its next bit. */
.macro SLAVE_ADVANCE_MISO
    lsls    r11, r11, #1
    tst     r11, #0x80
    it      ne
    strne   r10, [r5]
    it      eq
    streq   r10, [r6]
.endm

/* Slave: first seven bits advance within the current transmit byte. */
.section .time_critical.gpio_bitbang_slave_transfer_asm,"ax",%progbits
.align 2
.global gpio_bitbang_slave_transfer_asm
.type gpio_bitbang_slave_transfer_asm, %function
.thumb_func
gpio_bitbang_slave_transfer_asm:
    push    {r4-r11, lr}
    ldr     r4, [r0, #0]          /* GPIO_IN */
    ldr     r5, [r0, #4]          /* GPIO_SET */
    ldr     r6, [r0, #8]          /* GPIO_CLR */
    ldr     r7, [r0, #12]         /* CS mask */
    ldr     r8, [r0, #16]         /* SCK mask */
    ldr     r9, [r0, #20]         /* MOSI mask */
    ldr     r10, [r0, #24]        /* MISO mask */
    ldrb    r11, [r1], #1         /* current TX byte */

    /* Slave must present bit 7 before the master's first rising edge. */
    tst     r11, #0x80
    it      ne
    strne   r10, [r5]
    it      eq
    streq   r10, [r6]

.Lslave_byte:
    movs    r0, #0

    /* Bits 7 through 1: shift current TX byte after each falling edge. */
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO
    SLAVE_SAMPLE_EDGE
    SLAVE_ADVANCE_MISO

    /*
     * Bit 0 is already being driven. Prefetch the next byte BEFORE its
     * rising edge, then use that byte's MSB immediately after this bit's
     * falling edge. This avoids a C/loop byte-boundary gap on MISO.
     */
    subs    r3, r3, #1            /* number of bytes after current */
    beq     .Lslave_no_next_byte
    ldrb    r11, [r1], #1         /* preload next byte */
    b       .Lslave_final_bit
.Lslave_no_next_byte:
    movs    r11, #0               /* drive idle-low after final bit */

.Lslave_final_bit:
    SLAVE_SAMPLE_EDGE
    tst     r11, #0x80            /* first bit of next byte, or idle-low */
    it      ne
    strne   r10, [r5]
    it      eq
    streq   r10, [r6]
    strb    r0, [r2], #1
    cmp     r3, #0
    bne     .Lslave_byte

    movs    r0, #1                /* complete frame */
    b       .Lslave_exit

.Lslave_abort:
    movs    r0, #0                /* CS rose before all bits arrived */
.Lslave_exit:
    str     r10, [r6]             /* release MISO low */
    pop     {r4-r11, pc}
.size gpio_bitbang_slave_transfer_asm, .-gpio_bitbang_slave_transfer_asm
