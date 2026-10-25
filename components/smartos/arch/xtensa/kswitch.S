/* ---------------------------------------------------------------------------
 * Stage 2.3 - the Xtensa context switch.
 *
 * WHY THIS IS THE HARD PART
 *
 * On ARM a context switch pushes registers, swaps SP and pops. Xtensa keeps 64
 * physical registers and shows the code a rotating 16-register window, so at
 * the moment we want to change stacks most of the caller chain is still sitting
 * in registers, not in memory. Change a1 with live windows and the hardware
 * will later spill those registers onto whichever stack a1 happens to point at,
 * quietly corrupting the task we switched to.
 *
 * SPILL_ALL_WINDOWS is the fix. It walks the window all the way round with
 * no-op writes to high registers, letting the window-overflow handler push each
 * frame to its own stack, and comes back to the original frame having clobbered
 * nothing. After it runs, only the current frame is live in registers, and
 * changing a1 is safe.
 *
 * The one thing the hardware will not restore for us is where to resume, so a0
 * is written into the frame explicitly. Everything else rides on the stack and
 * comes back through the window-underflow handler when retw runs.
 *
 * The return-address encoding matters: retw takes the window rotation from
 * a0[31:30]. Code addresses on this chip are 0x4xxxxxxx, whose top two bits are
 * already 0b01 - the call4 encoding - so a plain function address works as a
 * resume point with no fixing up. k_stack_init() in ksched.cpp relies on this.
 * ------------------------------------------------------------------------- */

#include "xtensa/config/core-isa.h"   /* XCHAL_NUM_AREGS, needed by the macro below */
#include "xt_asm_utils.h"

/* Offset within our own 32-byte frame where the resume address is kept.
 * Hand-written asm, so the whole frame is ours to use. */
#define KSW_PC  0

/* ---------------------------------------------------------------------------
 * void k_ctx_switch(uint32_t **old_sp, uint32_t *new_sp);
 *
 * Returns - on some later call - as if it had just been called by whoever was
 * running on *old_sp. Interrupt state is deliberately not saved: each context
 * restores its own PS after the call returns, so a context that yielded with
 * interrupts masked comes back with them masked.
 * ------------------------------------------------------------------------- */
    .section .iram1.kswitch, "ax"
    .align  4
    .global k_ctx_switch
    .type   k_ctx_switch, @function
k_ctx_switch:
    entry   a1, 32                  /* a2 = old_sp, a3 = new_sp */

    s32i    a0, a1, KSW_PC          /* remember where to come back to */
    SPILL_ALL_WINDOWS               /* the rest of this context is now memory */

    s32i    a1, a2, 0               /* *old_sp = our stack pointer */
    mov     a1, a3                  /* stand on the incoming stack */
    l32i    a0, a1, KSW_PC          /* where that context wants to resume */

    retw                            /* underflow reloads its window on the way */
    .size   k_ctx_switch, . - k_ctx_switch


/* ---------------------------------------------------------------------------
 * Where a task begins life.
 *
 * Reached by the retw above landing on a stack that k_stack_init() built rather
 * than one that k_ctx_switch() left behind. Entered WITHOUT a call, so it must
 * not execute `entry` - the window has already been rotated for us.
 *
 * On arrival: a0 = 0 (end of the call chain, so a later spill stops here)
 *             a1 = the task's stack
 * ------------------------------------------------------------------------- */
    .align  4
    .global k_task_trampoline
    .type   k_task_trampoline, @function
k_task_trampoline:
    movi    a0, 0                   /* nothing above us to return to */
    call4   k_task_launch           /* reads the current TCB and runs it */
1:  j       1b                      /* k_task_launch must never return */
    .size   k_task_trampoline, . - k_task_trampoline


/* ---------------------------------------------------------------------------
 * Where a register-window chain ends.
 *
 * Every task stack is built with this address as the return address of its
 * outermost frame. retw takes its window increment from a0[31:30], so a zero
 * terminator is not merely useless, it is an illegal encoding - and the first
 * preemption bring-up proved it, ending in "IllegalInstruction at 0x65a5a5a5",
 * which is the stack fill pattern reinterpreted as a jump target.
 *
 * Reaching here means the window machinery walked further down a task's stack
 * than there are frames. Record it and stop, so core 0 can say so plainly.
 * ------------------------------------------------------------------------- */
    .section .iram1.kswitch, "ax"
    .align  4
    .global k_chain_end
    .type   k_chain_end, @function
k_chain_end:
    movi    a2, k_chain_end_hit
    movi    a3, 1
    s32i    a3, a2, 0
    memw
1:  j       1b
    .size   k_chain_end, . - k_chain_end
