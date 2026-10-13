#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 2.6a, hardened in 4.1 - core 0 as a service processor.
//
// The ESP32-S3 has exactly two cores and core 0 is not idle: it is holding up
// USB, flash, PSRAM and (if it is ever wanted) WiFi. So "use the spare core"
// really means one of two things, and this is the cheap one - core 1 keeps the
// kernel, and hands core 0 the jobs that are awkward or unsafe to do from a
// non-FreeRTOS core. Printing with a real printf is the obvious first example:
// core 1 cannot take newlib's locks, but core 0 can. Flash writes are next.
//
// EVERY HAND-OVER IS A COMPARE-AND-SWAP
//
// This mailbox was first written on the premise that exactly one core produces
// and exactly one consumes, so no lock was needed. Stage 2.6b broke that
// premise and nobody noticed: kernel tasks run on both cores now, and even on
// one core a poster can be preempted between finding an EMPTY slot and taking
// it. Stage 4.1's stress test caught two posters in one slot 98 times in 500
// calls - each handed the other's answer.
//
// So a slot changes owner only through k_cas(), and the state says who owns it:
//
//     EMPTY     -> any poster       CAS to CLAIMED (only one can win)
//     CLAIMED   -> that poster      fills the payload, stores POSTED
//     POSTED    -> contested        core 0 CASes to RUNNING to take it;
//                                   the poster CASes to EMPTY to withdraw it
//     RUNNING   -> core 0           CASes to DONE when finished
//     ABANDONED -> core 0           the poster timed out mid-job; core 0
//                                   finishes and stores EMPTY itself
//     DONE      -> that poster      reads the result, stores EMPTY
//
// The two contested moments - POSTED and RUNNING - are exactly where the old
// code went wrong on a timeout: it wrote EMPTY straight over a slot core 0 was
// still working in, so the next poster's payload could be swapped in mid-job
// and receive the previous job's answer.
//
// Slots live in internal SRAM: S32C1I, which k_cas uses, does not work on PSRAM.
//
// Core 0 currently discovers work by polling kmbox_service(). An inter-processor
// interrupt would remove the latency, and is the natural next step here.
// ---------------------------------------------------------------------------

#define KMBOX_SLOTS 8

// How long kmbox_call waits for an answer. Jobs that are slow on purpose -
// a flash erase, later - should use kmbox_call_timeout with their own figure.
#define KMBOX_TIMEOUT_MS 1000

typedef enum {
    K_JOB_NONE = 0,
    K_JOB_LOG,      // a0 = pointer to a NUL-terminated string in DRAM
    K_JOB_SUM,      // a0 = pointer to uint32_t[], a1 = count -> result is the sum
    K_JOB_SLEEP,    // a0 = milliseconds core 0 spends on it. For testing timeouts.
} k_job_op_t;

typedef enum {
    KMB_EMPTY = 0,
    KMB_CLAIMED,
    KMB_POSTED,
    KMB_RUNNING,
    KMB_DONE,
    KMB_ABANDONED,
} kmb_state_t;

typedef struct {
    volatile uint32_t state;
    uint32_t op;
    uint32_t a0, a1, a2;
    uint32_t result;
    uint32_t status;     // 0 = ok
    // Who posted it (stage 5.2), so a killed poster's slots can be found. Set
    // while the slot is CLAIMED, with preemption held from the claim to here.
    uint32_t owner_gen;
    uint8_t  owner;      // task id, or 0xFF from outside any kernel task
} k_job_t;

void     kmbox_init(void);

// --- poster side: kernel tasks, on either core -------------------------------
int      kmbox_post(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2);
bool     kmbox_done(int slot);
uint32_t kmbox_result(int slot);
void     kmbox_release(int slot);   // only once kmbox_done() is true

// Posts and waits for the answer, yielding meanwhile. False on a full mailbox,
// an error status, or a timeout - after which the slot is never touched again
// by this caller, whatever core 0 is doing with it.
bool     kmbox_call(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2,
                    uint32_t *result);
bool     kmbox_call_timeout(uint32_t op, uint32_t a0, uint32_t a1, uint32_t a2,
                            uint32_t *result, uint32_t timeout_ms);

// Number of slots in each state - for working out who is stuck. busy counts
// CLAIMED, RUNNING and ABANDONED together.
void     kmbox_census(uint32_t *empty, uint32_t *posted, uint32_t *done,
                      uint32_t *busy);

// Timeouts, by how they ended: withdrawn before core 0 started the job, or
// abandoned while it was running.
uint32_t kmbox_withdrawn_count(void);
uint32_t kmbox_abandoned_count(void);

// Stage 5.2. A killed task's slots, cleared so they can neither leak nor be
// served into its stack: CLAIMED, POSTED (withdrawn - the job never runs) and
// DONE (the answer nobody will read) go back to EMPTY. RUNNING cannot be
// cleared - core 0 is in the middle of it - and is counted in *running; the
// caller must not reap the task until a later call finds none. Core 0, from
// its main loop, never from inside a mailbox handler.
typedef struct {
    uint32_t claimed, posted, done, running;
} kmbox_forgot_t;
kmbox_forgot_t kmbox_forget(uint8_t owner, uint32_t owner_gen);

// --- extending it -------------------------------------------------------------
// Ops from KMBOX_OP_EXT up are served by whoever registers for them, so a
// service - the device service in os/dev.cpp, first - can hand work to core 0
// without the mailbox knowing anything about it. The handler runs on core 0 in
// FreeRTOS context, from kmbox_service(), and may use any IDF driver. It
// returns the result; *status is 0 on success.
#define KMBOX_OP_EXT  16
#define KMBOX_OPS     48
typedef uint32_t (*kmbox_handler_t)(const k_job_t *job, uint32_t *status);
bool     kmbox_register(uint32_t op, kmbox_handler_t fn);   // core 0, at start-up

// --- server side: core 0, FreeRTOS context -----------------------------------
// Runs every posted job it finds. Safe to call as often as you like.
void     kmbox_service(void);
uint32_t kmbox_served_count(void);
