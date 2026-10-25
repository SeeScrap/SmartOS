/* ---------------------------------------------------------------------------
 * Stage 2.2 - core 1's own Xtensa vector table.
 *
 * WHY THIS FILE HAS TO EXIST
 *
 * IDF's vector table cannot be shared with core 1 in a unicore build. Its
 * exception dispatch indexes a table as
 *
 *     _xt_exception_table[cause * portNUM_PROCESSORS + core_id]
 *
 * and CONFIG_FREERTOS_UNICORE sets portNUM_PROCESSORS to 1, so the table is
 * only one core wide. A fault taken on core 1 therefore indexes one slot past
 * the intended entry and calls whatever handler happens to live there. That is
 * the silent lockup this stage removes, and it is arithmetic, not a guess.
 *
 * WHAT THIS TABLE DOES
 *
 * The six window vectors are the register-window spill/fill handlers that every
 * Xtensa function call depends on. They are pure ISA plumbing with no policy in
 * them, so these are the canonical implementations, reproduced here to keep the
 * table self-contained. Jumping into IDF's copy instead would also risk falling
 * outside the +/-128 KB range of a j instruction once IRAM fills up.
 *
 * Every fault vector records machine state into k_fault_frame and then spins.
 * It deliberately does NOT print: printing means calling a C function, and
 * calling C from exception context means fixing up PS, WINDOWBASE and
 * WINDOWSTART by hand first. Core 0 polls the frame and reports from a context
 * where printf is actually safe.
 * ------------------------------------------------------------------------- */

/* Offsets into k_fault_frame - must match k_fault_frame_t in kfault.h */
#define FF_A0        0x00
#define FF_SP        0x04
#define FF_PC        0x08
#define FF_EXCCAUSE  0x0C
#define FF_EXCVADDR  0x10
#define FF_PS        0x14
#define FF_VECTOR    0x18
#define FF_PENDING   0x1C
#define FF_DEPC      0x20

    .section .data, "aw"
    .align  4

/* Stage 2.4 tick state. Lives in DRAM so the ISR can reach it with the cache
 * off, and so core 0 can read the counters without disturbing core 1. */
    .global k_tick_count
    .type   k_tick_count, @object
k_tick_count:
    .word   0
    .size   k_tick_count, 4

    .global k_need_resched
    .type   k_need_resched, @object
k_need_resched:
    .word   0
    .size   k_need_resched, 4

    .global k_tick_cycles
    .type   k_tick_cycles, @object
k_tick_cycles:
    .word   240000              /* replaced by k_tick_start() */
    .size   k_tick_cycles, 4

    .global k_chain_end_hit
    .type   k_chain_end_hit, @object
k_chain_end_hit:
    .word   0
    .size   k_chain_end_hit, 4

    .global k_fault_frame
    .type   k_fault_frame, @object
k_fault_frame:
    .space  40
    .size   k_fault_frame, . - k_fault_frame


/* ---------------------------------------------------------------------------
 * The table. VECBASE must be 1 KB aligned because the vectors span 0x000..0x400.
 * The .org offsets below are section relative and the table sits at offset 0 of
 * its own section.
 * ------------------------------------------------------------------------- */
    .section .iram1.s3vectors, "ax"
    .align  1024
    .global k_vector_table
    .type   k_vector_table, @function
k_vector_table:

/* --- 0x000  window overflow, call4 -------------------------------------- */
    .org    0x000
_s3_WindowOverflow4:
    s32e    a0, a5, -16
    s32e    a1, a5, -12
    s32e    a2, a5,  -8
    s32e    a3, a5,  -4
    rfwo

/* --- 0x040  window underflow, call4 ------------------------------------- */
    .org    0x040
_s3_WindowUnderflow4:
    l32e    a0, a5, -16
    l32e    a1, a5, -12
    l32e    a2, a5,  -8
    l32e    a3, a5,  -4
    rfwu

/* --- 0x080  window overflow, call8 -------------------------------------- */
    .org    0x080
_s3_WindowOverflow8:
    s32e    a0, a9, -16
    l32e    a0, a1, -12
    s32e    a1, a9, -12
    s32e    a2, a9,  -8
    s32e    a3, a9,  -4
    s32e    a4, a0, -32
    s32e    a5, a0, -28
    s32e    a6, a0, -24
    s32e    a7, a0, -20
    rfwo

/* --- 0x0C0  window underflow, call8 ------------------------------------- */
    .org    0x0C0
_s3_WindowUnderflow8:
    l32e    a0, a9, -16
    l32e    a1, a9, -12
    l32e    a2, a9,  -8
    l32e    a7, a1, -12
    l32e    a3, a9,  -4
    l32e    a4, a7, -32
    l32e    a5, a7, -28
    l32e    a6, a7, -24
    l32e    a7, a7, -20
    rfwu

/* --- 0x100  window overflow, call12 ------------------------------------- */
    .org    0x100
_s3_WindowOverflow12:
    s32e    a0,  a13, -16
    l32e    a0,  a1,  -12
    s32e    a1,  a13, -12
    s32e    a2,  a13,  -8
    s32e    a3,  a13,  -4
    s32e    a4,  a0,  -48
    s32e    a5,  a0,  -44
    s32e    a6,  a0,  -40
    s32e    a7,  a0,  -36
    s32e    a8,  a0,  -32
    s32e    a9,  a0,  -28
    s32e    a10, a0,  -24
    s32e    a11, a0,  -20
    rfwo

/* --- 0x140  window underflow, call12 ------------------------------------ */
    .org    0x140
_s3_WindowUnderflow12:
    l32e    a0,  a13, -16
    l32e    a1,  a13, -12
    l32e    a2,  a13,  -8
    l32e    a11, a1,  -12
    l32e    a3,  a13,  -4
    l32e    a4,  a11, -48
    l32e    a5,  a11, -44
    l32e    a6,  a11, -40
    l32e    a7,  a11, -36
    l32e    a8,  a11, -32
    l32e    a9,  a11, -28
    l32e    a10, a11, -24
    l32e    a11, a11, -20
    rfwu

/* --- 0x180..0x2C0  high level interrupts ---------------------------------
 * Nothing is routed to core 1 yet - kcore1_boot parks every peripheral source
 * on the invalid line - so reaching any of these means something is wrong.
 * Record it rather than execute padding.
 * ----------------------------------------------------------------------- */
    .org    0x180
_s3_Level2Vector:
    wsr     a0, excsave1
    call0   _s3_fault_common

    .org    0x1C0
_s3_Level3Vector:
    wsr     a0, excsave1
    call0   _s3_fault_common

    .org    0x200
_s3_Level4Vector:
    wsr     a0, excsave1
    call0   _s3_fault_common

    .org    0x240
_s3_Level5Vector:
    wsr     a0, excsave1
    call0   _s3_fault_common

    .org    0x280
_s3_DebugVector:
    wsr     a0, excsave1
    call0   _s3_fault_common

    .org    0x2C0
_s3_NMIVector:
    wsr     a0, excsave1
    call0   _s3_fault_common

/* --- 0x300  kernel exception (fault taken with PS.UM clear) -------------- */
    .org    0x300
_s3_KernelExceptionVector:
    wsr     a0, excsave1
    call0   _s3_fault_common

/* --- 0x340  user exception -----------------------------------------------
 * Level 1 interrupts also arrive here, with EXCCAUSE == 4, which is how the
 * stage 2.4 tick gets in. Everything else landing here is a fault.
 * ----------------------------------------------------------------------- */
    .org    0x340
_s3_UserExceptionVector:
    wsr     a0, excsave1
    rsr     a0, exccause
    beqi    a0, 4, 1f               /* EXCCAUSE 4 = level 1 interrupt */
    call0   _s3_fault_common        /* anything else is a fault */
1:  call0   _s3_int_entry           /* beqi only reaches +/-128 bytes, so the
                                       real handler is reached with call0,
                                       which is used here purely as a long jump */

/* --- 0x3C0  double exception -------------------------------------------- */
    .org    0x3C0
_s3_DoubleExceptionVector:
    wsr     a0, excsave1
    call0   _s3_fault_common

    .org    0x400
    .size   k_vector_table, . - k_vector_table


/* ---------------------------------------------------------------------------
 * Common fault recorder. Lives outside the table so it can use a literal pool.
 *
 * On entry:  EXCSAVE_1 = the faulting code's a0
 *            a0        = return address inside the vector we came through,
 *                        which is what identifies the vector that fired
 * ------------------------------------------------------------------------- */
    .section .iram1.s3fault, "ax"
    .align  4
    .global _s3_fault_common
    .type   _s3_fault_common, @function
_s3_fault_common:
    wsr     a0, excsave2            /* park the vector tag; a0 becomes scratch */

    movi    a0, k_fault_frame
    s32i    a1, a0, FF_SP           /* a1 is free to use as scratch from here */

    rsr     a1, excsave1            /* the faulting code's original a0 */
    s32i    a1, a0, FF_A0
    rsr     a1, excsave2            /* which vector we came through */
    s32i    a1, a0, FF_VECTOR
    rsr     a1, epc1
    s32i    a1, a0, FF_PC
    rsr     a1, exccause
    s32i    a1, a0, FF_EXCCAUSE
    rsr     a1, excvaddr
    s32i    a1, a0, FF_EXCVADDR
    rsr     a1, ps
    s32i    a1, a0, FF_PS
    rsr     a1, depc                /* set only for a double exception */
    s32i    a1, a0, FF_DEPC

    memw                            /* all of the above lands before the flag */
    movi    a1, 1
    s32i    a1, a0, FF_PENDING
    memw

    /* Stop here. A spin rather than waiti, so the core's state stays exactly as
     * it was for a debugger, and so this path cannot depend on the interrupt
     * configuration that may itself be what broke. */
1:  j       1b
    .size   _s3_fault_common, . - _s3_fault_common


/* ---------------------------------------------------------------------------
 * Stage 2.4 / preemption - the level 1 interrupt entry.
 *
 * The first version of this handler did its whole job in assembly and never
 * called C, because leaving exception mode by hand is the easiest thing in a
 * kernel to get subtly wrong. That was the right call while there was no board
 * to debug on, but it also meant the tick could only ask for a reschedule and
 * hope somebody yielded. Real preemption has to switch stacks from in here.
 *
 * WHAT HAS TO BE SAVED, AND WHAT DOES NOT
 *
 * All sixteen address registers, and that is not negotiable. The first cut of
 * this handler saved only a0..a3 on the theory that the register window would
 * look after the rest. It does not. ENTRY in the callee rotates WINDOWBASE by
 * CALLINC, so after a call4 the callee sees our a4..a15 as its own a0..a11 and
 * writes straight through them. Only a0..a3 sit below the rotation and survive.
 *
 * On the board that showed up as an argument to esp_rom_printf coming out as
 * garbage: the tick had landed in the middle of the ROM printf and handed its
 * registers to the C dispatch.
 *
 * Beyond the address registers: the interrupted SP, PS and EPC1 to be able to
 * return at all, SAR because compiled code uses it constantly, and
 * LBEG/LEND/LCOUNT because this core has zero-overhead loops and a loop
 * interrupted mid-flight would otherwise resume wrong.
 *
 * MAC16 and the FP unit are also present on this core but are left alone: the
 * kernel contains no MAC or float code, and saving them would cost every tick.
 * That assumption breaks the moment kernel code uses a float on core 1.
 *
 * THE STACK
 *
 * The frame is carved out of the interrupted task's own stack, not a separate
 * interrupt stack. That is deliberate: it makes the ISR just another frame on
 * that task, so k_yield() called from k_int_dispatch behaves exactly as it does
 * anywhere else - SPILL_ALL_WINDOWS pushes this frame down with the rest, the
 * switch happens, and when the task is eventually scheduled again it returns
 * right back into the exit path below and rfe's to where it was interrupted.
 * ------------------------------------------------------------------------- */

#define IF_A0     0
#define IF_A1     4
#define IF_A2     8
#define IF_A3     12
#define IF_A4     16
#define IF_A5     20
#define IF_A6     24
#define IF_A7     28
#define IF_A8     32
#define IF_A9     36
#define IF_A10    40
#define IF_A11    44
#define IF_A12    48
#define IF_A13    52
#define IF_A14    56
#define IF_A15    60
#define IF_PS     64
#define IF_PC     68
#define IF_SAR    72
#define IF_LBEG   76
#define IF_LEND   80
#define IF_LCOUNT 84
#define IF_SIZE   112       /* 88 used, rounded up and kept 16-byte aligned */

/* PS_WOE | PS_UM | PS_INTLEVEL(1) - the same value IDF's own _xt_lowint1 uses
 * to leave exception mode. INTLEVEL 1 keeps this handler from interrupting
 * itself while it runs. */
#define PS_FOR_C  0x00040021

    .section .iram1.s3tick, "ax"
    .align  4
    .global _s3_int_entry
    .type   _s3_int_entry, @function
_s3_int_entry:
    mov     a0, a1                  /* remember the interrupted stack pointer */
    addi    a1, a1, -IF_SIZE
    s32i    a0, a1, IF_A1
    s32i    a2, a1, IF_A2
    s32i    a3, a1, IF_A3
    s32i    a4, a1, IF_A4
    s32i    a5, a1, IF_A5
    s32i    a6, a1, IF_A6
    s32i    a7, a1, IF_A7
    s32i    a8, a1, IF_A8
    s32i    a9, a1, IF_A9
    s32i    a10, a1, IF_A10
    s32i    a11, a1, IF_A11
    s32i    a12, a1, IF_A12
    s32i    a13, a1, IF_A13
    s32i    a14, a1, IF_A14
    s32i    a15, a1, IF_A15

    /* Move the base save area down with the stack pointer.
     *
     * The sixteen bytes below any SP hold the caller's a0..a3, and the call8
     * and call12 window-overflow handlers do not merely write there - they READ
     * the caller's stack pointer back out of [sp-12] to find where to spill
     * a4..a7. Dropping a1 by IF_SIZE without bringing that area along leaves
     * the hardware reading uninitialised stack and then writing registers to
     * whatever address it finds.
     *
     * On the board this showed up as a LoadStoreError inside _WindowUnderflow8,
     * reached from a plain retw in k_ticks(), with a stack pointer that had
     * become a code address. a2/a3 are already saved above, so they are free
     * scratch here. */
    addi    a2, a0, -16             /* a0 still holds the interrupted SP */
    addi    a3, a1, -16
    l32i    a0, a2, 0
    s32i    a0, a3, 0
    l32i    a0, a2, 4
    s32i    a0, a3, 4
    l32i    a0, a2, 8
    s32i    a0, a3, 8
    l32i    a0, a2, 12
    s32i    a0, a3, 12

    rsr     a0, excsave1            /* the interrupted code's a0 */
    s32i    a0, a1, IF_A0
    rsr     a0, ps
    s32i    a0, a1, IF_PS
    rsr     a0, epc1
    s32i    a0, a1, IF_PC
    rsr     a0, sar
    s32i    a0, a1, IF_SAR
    rsr     a0, lbeg
    s32i    a0, a1, IF_LBEG
    rsr     a0, lend
    s32i    a0, a1, IF_LEND
    rsr     a0, lcount
    s32i    a0, a1, IF_LCOUNT

    /* Out of exception mode and into something C can run in. */
    movi    a0, PS_FOR_C
    wsr     a0, ps
    rsync

    /* a0 is deliberately left as the vector's return address rather than zeroed.
     * retw takes its window increment from a0[31:30], and zero is not a valid
     * encoding - a spilled and later reloaded a0 of 0 is a landmine. The value
     * the vector left there is a real code address, so its top bits are the
     * call4 pattern already. */
    mov     a6, a1                  /* call4 maps our a6 to the callee's a2 */
    call4   k_int_dispatch

    /* Either straight back, or a long time later on the far side of a context
     * switch - the exit path cannot tell the difference, which is the point. */

    /* Copy the base save area back where the interrupted code expects it.
     *
     * The copy on entry only fixed half the problem. While a1 pointed at this
     * frame, every window overflow wrote the caller's a0..a3 to OUR copy at
     * [isr_sp-16]. The moment a1 goes back, the matching underflow reads from
     * [orig_sp-16] instead - a location nothing has written since before the
     * interrupt. The two have to be reconciled here or the caller comes back
     * with stale registers, which is how a plain retw in k_ticks() ended up
     * with a code address for a stack pointer.
     *
     * a2/a3 are restored from the frame further down, so they are scratch. */
    addi    a2, a1, -16
    l32i    a3, a1, IF_A1
    addi    a3, a3, -16
    l32i    a0, a2, 0
    s32i    a0, a3, 0
    l32i    a0, a2, 4
    s32i    a0, a3, 4
    l32i    a0, a2, 8
    s32i    a0, a3, 8
    l32i    a0, a2, 12
    s32i    a0, a3, 12

    l32i    a0, a1, IF_LCOUNT
    wsr     a0, lcount
    l32i    a0, a1, IF_LBEG
    wsr     a0, lbeg
    l32i    a0, a1, IF_LEND
    wsr     a0, lend
    l32i    a0, a1, IF_SAR
    wsr     a0, sar
    l32i    a0, a1, IF_PC
    wsr     a0, epc1

    l32i    a2, a1, IF_A2
    l32i    a3, a1, IF_A3
    l32i    a4, a1, IF_A4
    l32i    a5, a1, IF_A5
    l32i    a6, a1, IF_A6
    l32i    a7, a1, IF_A7
    l32i    a8, a1, IF_A8
    l32i    a9, a1, IF_A9
    l32i    a10, a1, IF_A10
    l32i    a11, a1, IF_A11
    l32i    a12, a1, IF_A12
    l32i    a13, a1, IF_A13
    l32i    a14, a1, IF_A14
    l32i    a15, a1, IF_A15

    /* PS goes back last but one. It carries EXCM set, which disables window
     * exceptions, so nothing below here may touch the register window. */
    l32i    a0, a1, IF_PS
    wsr     a0, ps
    rsync
    l32i    a0, a1, IF_A0
    l32i    a1, a1, IF_A1
    rfe
    .size   _s3_int_entry, . - _s3_int_entry
