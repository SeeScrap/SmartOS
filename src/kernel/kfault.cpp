#include "kernel/kfault.h"

#include "s3hal/log.h"

// Xtensa EXCCAUSE values. Only the ones a kernel realistically trips over are
// spelled out; anything else is reported by number.
const char *kfault_cause_name(uint32_t c)
{
    switch (c) {
        case 0:  return "IllegalInstruction";
        case 1:  return "Syscall";
        case 2:  return "InstructionFetchError";
        case 3:  return "LoadStoreError";
        case 4:  return "Level1Interrupt";
        case 5:  return "Alloca (movsp with unspilled windows)";
        case 6:  return "IntegerDivideByZero";
        case 8:  return "Privileged";
        case 9:  return "LoadStoreAlignment";
        case 12: return "InstrPIFDataError";
        case 13: return "LoadStorePIFDataError";
        case 14: return "InstrPIFAddrError";
        case 15: return "LoadStorePIFAddrError";
        case 16: return "InstTLBMiss";
        case 17: return "InstTLBMultiHit";
        case 18: return "InstFetchPrivilege";
        case 20: return "InstFetchProhibited";
        case 24: return "LoadStoreTLBMiss";
        case 25: return "LoadStoreTLBMultiHit";
        case 26: return "LoadStorePrivilege";
        case 28: return "LoadProhibited";
        case 29: return "StoreProhibited";
        default: return "unknown";
    }
}

bool kfault_pending(void)
{
    return k_fault_frame.pending != 0;
}

static unsigned      s_reported;
static kfault_sink_t s_sink;
unsigned kfault_reported(void) { return s_reported; }
void     kfault_set_sink(kfault_sink_t sink) { s_sink = sink; }

void kfault_report(void)
{
    if (!kfault_pending()) return;

    // Take a copy first: core 1 is parked, but reading the volatile struct
    // field-by-field while formatting would interleave awkwardly.
    k_fault_frame_t f;
    f.a0       = k_fault_frame.a0;
    f.sp       = k_fault_frame.sp;
    f.pc       = k_fault_frame.pc;
    f.exccause = k_fault_frame.exccause;
    f.excvaddr = k_fault_frame.excvaddr;
    f.ps       = k_fault_frame.ps;
    f.vector   = k_fault_frame.vector;
    f.depc     = k_fault_frame.depc;

    // Consumed here, not by the caller. It used to be the caller's job, and
    // the stage 3.6 main loop did clear it while wake_core1() did not - so a
    // fault during boot was reported once there and again by the main loop.
    // A report that clears its own flag cannot be printed twice.
    k_fault_frame.pending = 0;
    s_reported++;

    logln();
    logln("################ CORE 1 FAULT ################");
    logf("cause    : %lu (%s)\n", (unsigned long)f.exccause,
         kfault_cause_name(f.exccause));
    logf("PC       : 0x%08lx   <- the instruction that faulted\n",
         (unsigned long)f.pc);
    logf("EXCVADDR : 0x%08lx   <- address it was touching\n",
         (unsigned long)f.excvaddr);
    logf("PS       : 0x%08lx   (INTLEVEL %lu, EXCM %lu, UM %lu, WOE %lu)\n",
         (unsigned long)f.ps,
         (unsigned long)(f.ps & 0xF),
         (unsigned long)((f.ps >> 4) & 1),
         (unsigned long)((f.ps >> 5) & 1),
         (unsigned long)((f.ps >> 18) & 1));
    logf("SP       : 0x%08lx\n", (unsigned long)f.sp);
    logf("a0       : 0x%08lx   <- return address of the faulting function\n",
         (unsigned long)f.a0);
    logf("vector   : 0x%08lx   <- which vector caught it\n",
         (unsigned long)f.vector);
    if (f.depc) {
        logf("DEPC     : 0x%08lx   <- DOUBLE fault: this is where the handler\n",
             (unsigned long)f.depc);
        logln("                        itself died. PC above is the first fault.");
    }
    logln();
    logln("Core 1 is parked in a spin loop. Decode PC and a0 with:");
    logln("  xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/s3-n16r8/firmware.elf <addr>");
    logln("##############################################");

    if (s_sink) {
        s_sink((unsigned)f.exccause, kfault_cause_name(f.exccause), (unsigned)f.pc,
               (unsigned)f.excvaddr, f.depc != 0);
    }
}
