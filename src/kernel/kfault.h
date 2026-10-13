#pragma once
#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 2.2 - fault reporting for core 1.
//
// Core 1 has no panic handler of its own and cannot borrow IDF's, so when it
// faults the vector table writes what it knows into this frame and stops. The
// frame lives in internal DRAM, which both cores see coherently, so core 0 can
// pick it up and print it from a context where printf is safe.
//
// The layout is duplicated as FF_* offsets in kvectors.S - change both together.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t a0;         // 0x00  the faulting code's a0
    uint32_t sp;         // 0x04  a1 at the moment of the fault
    uint32_t pc;         // 0x08  EPC1 - the instruction that faulted
    uint32_t exccause;   // 0x0C
    uint32_t excvaddr;   // 0x10  address involved, for load/store faults
    uint32_t ps;         // 0x14
    uint32_t vector;     // 0x18  address inside the vector that fired
    uint32_t pending;    // 0x1C  non-zero once a fault has been recorded
    uint32_t depc;       // 0x20  double-exception PC; 0 for a normal fault
} k_fault_frame_t;

extern "C" volatile k_fault_frame_t s3_fault_frame;

// True if core 1 has faulted and is now parked.
bool kfault_pending(void);

// Prints a decoded report of the recorded fault. Call from core 0.
// Prints the pending fault once and clears it. Safe to call when none is pending.
void kfault_report(void);

// How many faults have been reported since boot - for tests, which cannot use
// kfault_pending() once a report has cleared it.
unsigned kfault_reported(void);

// Somewhere a fault report should also go - the system log, since stage 4.5.
// The kernel does not know about the log; whoever does installs a sink, and
// every report after that calls it once, after printing, on core 0.
typedef void (*kfault_sink_t)(unsigned cause, const char *cause_name,
                              unsigned pc, unsigned addr, bool double_fault);
void kfault_set_sink(kfault_sink_t sink);

// Human-readable name for an Xtensa EXCCAUSE value.
const char *kfault_cause_name(uint32_t exccause);
