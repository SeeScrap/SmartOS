#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "smartos_config.h"

// ===========================================================================
// The machine installed apps run on.
// เครื่องที่แอปซึ่งติดตั้งทีหลังทำงานอยู่บนนั้น
// ===========================================================================
//
// WHY THERE IS A MACHINE HERE AT ALL
//
// Stage 7 wants a user to install an app as a file, without reflashing the
// operating system. The obvious way is to load native code - and stage 7a
// measured what that costs on this chip: ESP-IDF's memory protection unit
// marks every byte the allocator hands out as non-executable, it is locked at
// boot, and turning it off is a build-time decision that would let a bug
// anywhere in the system jump into data. The instruction bus does not reach
// PSRAM either, so an installed app would be stuck in internal RAM besides.
//
// So installed apps are not code. They are data, and this interprets them.
//
// THAT IS A BETTER ANSWER, NOT A CONSOLATION PRIZE
//
// A built-in app is compiled into the firmware and can do anything the kernel
// can: a stray pointer takes the system down. An app running here cannot.
// It has no pointers - only indices, every one of them checked - and it can
// only reach the system through a numbered table that this file owns. It
// cannot loop forever either: every callback runs under an instruction
// budget. An installed app is the safest thing in this operating system.
//
// แอปที่คอมไพล์เข้าเฟิร์มแวร์ทำอะไรก็ได้ที่เคอร์เนลทำได้ พอยน์เตอร์หลงตัวเดียว
// ระบบล่มทั้งระบบ แต่แอปที่รันบนเครื่องนี้ทำไม่ได้ มันไม่มีพอยน์เตอร์ มีแต่
// ดัชนีที่ถูกตรวจทุกตัว เข้าถึงระบบได้ทางตารางหมายเลขที่ไฟล์นี้เป็นเจ้าของ
// เท่านั้น และวนไม่รู้จบไม่ได้เพราะทุก callback มีเพดานจำนวนคำสั่ง
//
// WHAT KIND OF MACHINE
//
// A stack machine with 32-bit words, because that is the smallest thing a
// compiler can target without thinking and the smallest thing an interpreter
// can be. Four separate spaces, none of which can reach another:
//
//   code      the program. Read-only, never executed by the processor.
//   consts    numbers and strings the program refers to by index.
//   globals   the app's own state, the equivalent of app_ctx_t::user.
//   stack     operands, and one frame of locals per call.
//
// Everything is an index into one of those, and every index is checked
// against that space's length before it is used. There is no instruction
// that takes an address.
// ---------------------------------------------------------------------------

// --- the instruction set ---------------------------------------------------
// One byte of opcode, then the operands each one names. Kept small on
// purpose: a bigger set is a bigger interpreter, and the thing being
// interpreted is an app on a 240 MHz microcontroller, not a language.
typedef enum : uint8_t {
    VM_HALT     = 0x00,  //            stop, successfully

    // constants and the stack
    VM_PUSH_I8  = 0x01,  // i8         push a small literal
    VM_PUSH_K   = 0x02,  // u16 index  push consts[index]
    VM_DUP      = 0x03,  //            push a copy of the top
    VM_DROP     = 0x04,  //            discard the top
    VM_SWAP     = 0x05,  //            exchange the top two

    // the app's own memory
    VM_LD_G     = 0x06,  // u16 index  push globals[index]
    VM_ST_G     = 0x07,  // u16 index  globals[index] = pop
    VM_LD_L     = 0x08,  // u8 index   push this frame's local
    VM_ST_L     = 0x09,  // u8 index   this frame's local = pop

    // arithmetic. Division by zero stops the app rather than the system.
    VM_ADD      = 0x10,
    VM_SUB      = 0x11,
    VM_MUL      = 0x12,
    VM_DIV      = 0x13,
    VM_MOD      = 0x14,
    VM_NEG      = 0x15,

    // bits
    VM_AND      = 0x18,
    VM_OR       = 0x19,
    VM_XOR      = 0x1A,
    VM_SHL      = 0x1B,
    VM_SHR      = 0x1C,  // arithmetic, so negative numbers keep their sign

    // comparison: push 1 or 0
    VM_EQ       = 0x20,
    VM_NE       = 0x21,
    VM_LT       = 0x22,
    VM_LE       = 0x23,
    VM_GT       = 0x24,
    VM_GE       = 0x25,

    // control
    VM_JMP      = 0x30,  // i16 offset   relative to the next instruction
    VM_JZ       = 0x31,  // i16 offset   jump if the popped value is 0
    VM_JNZ      = 0x32,  // i16 offset
    VM_CALL     = 0x33,  // u16 addr, u8 nargs, u8 nlocals
    VM_RET      = 0x34,  //              return the top of the stack

    // the way out to the operating system
    VM_SYS      = 0x40,  // u8 number, u8 nargs
} vm_op_t;

// --- what can go wrong ------------------------------------------------------
// Every one of these stops the app and leaves it stopped. None of them can
// affect anything else on the board - that is the whole point of running a
// program this way.
typedef enum {
    VM_OK = 0,
    VM_HALTED,            // the program said it was finished
    VM_ERR_OPCODE,        // an opcode this machine does not have
    VM_ERR_RANGE,         // an index outside the space it names
    VM_ERR_STACK,         // the operand stack over- or underflowed
    VM_ERR_DIV0,
    VM_ERR_BUDGET,        // it ran too long without returning
    VM_ERR_SYSCALL,       // a system call number that does not exist
    VM_ERR_DEPTH,         // calls nested too deeply
} vm_err_t;

const char *vm_err_name(vm_err_t e);

// --- a program --------------------------------------------------------------
// Everything is given from outside; this file allocates nothing. The loader
// owns the memory and hands it over, so what an app may have is decided
// where apps are decided, not here.
typedef struct {
    const uint8_t  *code;
    uint32_t        code_len;
    const int32_t  *consts;
    uint16_t        const_len;
    const char     *strings;      // consts can hold an offset into this
    uint32_t        strings_len;
    int32_t        *globals;
    uint16_t        global_len;
    int32_t        *stack;
    uint16_t        stack_len;
} vm_program_t;

// A system call, by number. argv points at the arguments the app pushed,
// leftmost first; the return value is pushed back. The table is owned by
// os/vm_sys.cpp, and it is the whole of what an installed app can do.
typedef int32_t (*vm_syscall_fn)(void *user, const int32_t *argv, int argc, vm_err_t *err);

typedef struct {
    const vm_syscall_fn *table;
    uint16_t             count;
    void                *user;    // handed to every call: the app's context
} vm_syscalls_t;

// --- running one -----------------------------------------------------------
// Runs from `entry` until it returns, halts, or breaks a rule. `budget` is
// the most instructions it may execute first - a callback that overruns it is
// stopped, and the app with it.
//
// Returns VM_OK or VM_HALTED when all is well; anything else is the reason it
// stopped, and `out` is left untouched.
vm_err_t vm_run(const vm_program_t *p, const vm_syscalls_t *sys,
                uint32_t entry, uint32_t budget, int32_t *out);

// How many instructions the last run took. For the task manager, and for
// knowing whether a budget is anywhere near being reached.
uint32_t vm_last_steps(void);
