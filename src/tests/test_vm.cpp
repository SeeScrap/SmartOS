#include "tests/tests.h"

#include <stdio.h>
#include <string.h>

#include "board/log.h"
#include "kernel/ktime.h"
#include "os/vm.h"
#include "os/vm_sys.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 7b - the machine installed apps run on.
//
// Two halves, and the second one matters more.
//
// First: does it compute? Arithmetic, a loop, a function call, a system call.
// If those work the interpreter is a machine.
//
// Second, and this is the point of the whole design: does every guard fire?
// An installed app is a file a user put on the board, and the reason it is
// allowed to run at all is that it cannot do any of the things tested below.
// Each case here is a program written to break one rule, and a pass means the
// machine stopped it and said why - rather than reaching past its memory,
// taking the system down, or looping until the watchdog does.
//
// ครึ่งหลังสำคัญกว่า: แอปที่ติดตั้งคือไฟล์ที่ผู้ใช้เอามาวาง เหตุผลที่ยอมให้มัน
// รันคือมันทำสิ่งเหล่านี้ไม่ได้ แต่ละเคสคือโปรแกรมที่เขียนมาเพื่อแหกกฎหนึ่งข้อ
// และ "ผ่าน" แปลว่าเครื่องหยุดมันได้และบอกได้ว่าเพราะอะไร
// ---------------------------------------------------------------------------

#define GLOBALS 8
#define STACK   32

static int32_t s_globals[GLOBALS];
static int32_t s_stack[STACK];
static int32_t s_consts[5] = { 1000, -7, 0x1234, 42, INT32_MIN };

// A system call that doubles its argument, so a test can tell a real call
// from a lucky return value. Number 0 in the table below.
static int32_t sys_double(void *user, const int32_t *argv, int argc, vm_err_t *err)
{
    (void)user;
    if (argc != 1) { *err = VM_ERR_SYSCALL; return 0; }
    return argv[0] * 2;
}
static const vm_syscall_fn s_table[] = { sys_double };
static const vm_syscalls_t s_sys = { s_table, 1, nullptr };

static vm_program_t prog(const uint8_t *code, uint32_t len, uint16_t stack_len = STACK)
{
    memset(s_globals, 0, sizeof(s_globals));
    vm_program_t p = {};
    p.code       = code;
    p.code_len   = len;
    p.consts     = s_consts;
    p.const_len  = (uint16_t)(sizeof(s_consts) / sizeof(s_consts[0]));
    p.globals    = s_globals;
    p.global_len = GLOBALS;
    p.stack      = s_stack;
    p.stack_len  = stack_len;
    return p;
}

// Runs a program and reports what it answered and why it stopped.
static bool ran(const uint8_t *code, uint32_t len, int32_t want, char *d, size_t dn,
                uint32_t budget = 10000)
{
    const vm_program_t p = prog(code, len);
    int32_t got = 0;
    const vm_err_t e = vm_run(&p, &s_sys, 0, budget, &got);
    snprintf(d, dn, "%s after %lu steps, got %ld (want %ld)",
             vm_err_name(e), (unsigned long)vm_last_steps(), (long)got, (long)want);
    return (e == VM_OK || e == VM_HALTED) && got == want;
}

// Runs a program that is MEANT to be stopped, and checks it was stopped for
// the right reason.
static bool stopped(const uint8_t *code, uint32_t len, vm_err_t want, char *d, size_t dn,
                    uint32_t budget = 200, uint16_t stack_len = STACK)
{
    const vm_program_t p = prog(code, len, stack_len);
    int32_t got = 0;
    const vm_err_t e = vm_run(&p, &s_sys, 0, budget, &got);
    snprintf(d, dn, "stopped: %s (wanted %s), %lu steps",
             vm_err_name(e), vm_err_name(want), (unsigned long)vm_last_steps());
    return e == want;
}

void vm_logic_test(void)
{
    logln();
    logln("--- stage 7b: the machine installed apps run on ---");
    char d[140];

    // --- does it compute? --------------------------------------------------

    // 7 * 6 + 2
    static const uint8_t p_math[] = { 0x01,7, 0x01,6, 0x12, 0x01,2, 0x10, 0x34 };
    sdk_case("arithmetic", ran(p_math, sizeof(p_math), 44, d, sizeof(d)), d);

    // A constant from the pool, doubled by hand: 1000 + 1000.
    static const uint8_t p_const[] = { 0x02,0,0, 0x02,0,0, 0x10, 0x34 };
    sdk_case("the constant pool", ran(p_const, sizeof(p_const), 2000, d, sizeof(d)), d);

    // sum = 0; i = 1; while (i <= 10) { sum += i; i++; } return sum;   -> 55
    static const uint8_t p_loop[] = {
        0x01,0,        0x07,0,0,          // globals[0] = 0        (sum)
        0x01,1,        0x07,1,0,          // globals[1] = 1        (i)
        0x06,1,0,      0x01,10, 0x24,     // i > 10 ?
        0x32,22,0,                        //   if so, jump to the end
        0x06,0,0,      0x06,1,0, 0x10, 0x07,0,0,   // sum = sum + i
        0x06,1,0,      0x01,1,  0x10, 0x07,1,0,    // i = i + 1
        0x30,0xE1,0xFF,                   // back to the test
        0x06,0,0,      0x34,              // return sum
    };
    sdk_case("a loop", ran(p_loop, sizeof(p_loop), 55, d, sizeof(d)), d);

    // square(5), as a real call with its own frame.
    static const uint8_t p_call[] = {
        0x01,5, 0x33,8,0,1,0, 0x34,        // push 5; call 8 (1 arg); return
        0x08,0, 0x08,0, 0x12, 0x34,        // square: local0 * local0
    };
    sdk_case("a function call", ran(p_call, sizeof(p_call), 25, d, sizeof(d)), d);

    // A system call: double(21).
    static const uint8_t p_sys[] = { 0x01,21, 0x40,0,1, 0x34 };
    sdk_case("a system call", ran(p_sys, sizeof(p_sys), 42, d, sizeof(d)), d);

    // --- and does every guard fire? ----------------------------------------

    static const uint8_t g_opcode[] = { 0xFE, 0x34 };
    sdk_case("an instruction it lacks",
             stopped(g_opcode, sizeof(g_opcode), VM_ERR_OPCODE, d, sizeof(d)), d);

    // globals[999] on a program with eight of them.
    static const uint8_t g_global[] = { 0x06,0xE7,0x03, 0x34 };
    sdk_case("reading past its own memory",
             stopped(g_global, sizeof(g_global), VM_ERR_RANGE, d, sizeof(d)), d);

    // A jump that lands outside the program.
    static const uint8_t g_jump[] = { 0x30,0x00,0x7F, 0x34 };
    sdk_case("jumping out of the program",
             stopped(g_jump, sizeof(g_jump), VM_ERR_RANGE, d, sizeof(d)), d);

    // Add with nothing to add.
    static const uint8_t g_under[] = { 0x10, 0x34 };
    sdk_case("taking from an empty stack",
             stopped(g_under, sizeof(g_under), VM_ERR_STACK, d, sizeof(d)), d);

    // Push for ever, on a stack of four.
    static const uint8_t g_over[] = { 0x01,1, 0x30,0xFB,0xFF };
    sdk_case("filling the stack",
             stopped(g_over, sizeof(g_over), VM_ERR_STACK, d, sizeof(d), 200, 4), d);

    static const uint8_t g_div0[] = { 0x01,1, 0x01,0, 0x13, 0x34 };
    sdk_case("dividing by zero",
             stopped(g_div0, sizeof(g_div0), VM_ERR_DIV0, d, sizeof(d)), d);

    // The three sums that do not fit, in one program, because signed overflow
    // is undefined in C++ and this machine runs numbers it did not choose:
    //   INT32_MIN / -1  ->  INT32_MIN   (the one division that overflows)
    //   -INT32_MIN      ->  INT32_MIN
    //   INT32_MIN + INT32_MIN -> 0
    // Every other way out of this machine has a case above. Until A1 review
    // these three had none, and none of them was handled.
    static const uint8_t g_wrap[] = { 0x02,4,0, 0x01,0xFF, 0x13, 0x15,
                                      0x02,4,0, 0x10, 0x34 };
    sdk_case("numbers that will not fit",
             ran(g_wrap, sizeof(g_wrap), 0, d, sizeof(d)), d);

    // The one that matters most on a system with a watchdog: a program that
    // never returns is stopped, and nothing else on the board notices.
    static const uint8_t g_spin[] = { 0x30,0xFD,0xFF };
    sdk_case("looping for ever",
             stopped(g_spin, sizeof(g_spin), VM_ERR_BUDGET, d, sizeof(d)), d);

    static const uint8_t g_sys[] = { 0x40,99,0, 0x34 };
    sdk_case("a system call that does not exist",
             stopped(g_sys, sizeof(g_sys), VM_ERR_SYSCALL, d, sizeof(d)), d);

    // Endless recursion: each level is a frame, and there are sixteen.
    static const uint8_t g_deep[] = { 0x33,0,0,0,0, 0x34 };
    sdk_case("recursing without end",
             stopped(g_deep, sizeof(g_deep), VM_ERR_DEPTH, d, sizeof(d)), d);

    // --- the bridge to the operating system (stage 7c) ---------------------
    //
    // The real syscall table, called the way an app calls it. No framebuffer
    // is set, which is how an app outside draw() sees the world - so every
    // drawing call here must be refused, not quietly dropped.
    {
        // Three strings end to end, the way a loader lays an app's blob out:
        // each NUL-terminated, and an app refers to one by its offset. Written
        // as an array because a string literal with NULs inside it is a
        // warning this build treats as an error.
        static const char blob[] = {
            'h','e','l','l','o', 0,                                             //  0
            'p','r','e','s','s','e','d',' ','%','d',' ','t','i','m','e','s', 0, //  6
            'b','a','c','k','l','i','g','h','t', 0,                             // 23
        };
        static vm_app_t ctx = {};
        vm_program_t p = prog(nullptr, 0);
        p.strings     = blob;
        p.strings_len = sizeof(blob);
        ctx.prog      = &p;
        ctx.fb        = nullptr;
        const vm_syscalls_t sys = { vm_sys_table(), vm_sys_count(), &ctx };

        vm_err_t e = VM_OK;
        const int32_t w = sys.table[VMSYS_WIDTH](&ctx, nullptr, 0, &e);
        const int32_t h = sys.table[VMSYS_HEIGHT](&ctx, nullptr, 0, &e);
        snprintf(d, sizeof(d), "%d calls in the table; width %ld, height %ld",
                 (int)vm_sys_count(), (long)w, (long)h);
        sdk_case("the system call table", e == VM_OK && w == LCD_WIDTH && h == APP_AREA_H, d);

        // Formatting: the app hands an offset into its own string blob.
        const int32_t fargs[4] = { 6, 3, 0, 0 };        // "pressed %d times", 3
        e = VM_OK;
        const int32_t hdl = sys.table[VMSYS_FMT](&ctx, fargs, 4, &e);
        const char *made = (hdl < 0 && -hdl - 1 < VM_SCRATCH_SLOTS) ? ctx.scratch[-hdl - 1] : "?";
        snprintf(d, sizeof(d), "handle %ld -> \"%s\"", (long)hdl, made);
        sdk_case("formatting a string", e == VM_OK && strcmp(made, "pressed 3 times") == 0, d);

        // Drawing with no framebuffer: refused.
        const int32_t targs[5] = { 0, 0, 0, 0, 1 };
        e = VM_OK;
        sys.table[VMSYS_TEXT](&ctx, targs, 5, &e);
        snprintf(d, sizeof(d), "drawing outside draw() gave: %s", vm_err_name(e));
        sdk_case("drawing when there is no screen", e == VM_ERR_SYSCALL, d);

        // A string handle that points outside the blob.
        const int32_t bad[2] = { 9999, 1 };
        e = VM_OK;
        sys.table[VMSYS_TEXT_WIDTH](&ctx, bad, 2, &e);
        snprintf(d, sizeof(d), "a handle past the end of its strings gave: %s", vm_err_name(e));
        sdk_case("a string that is not its own", e == VM_ERR_RANGE, d);

        // The wrong number of arguments.
        e = VM_OK;
        sys.table[VMSYS_RGB](&ctx, bad, 2, &e);
        snprintf(d, sizeof(d), "rgb with 2 arguments instead of 3 gave: %s", vm_err_name(e));
        sdk_case("a call with the wrong arguments", e == VM_ERR_SYSCALL, d);
    }

    // --- what it costs -----------------------------------------------------
    {
        const vm_program_t p = prog(p_loop, sizeof(p_loop));
        const uint32_t t0 = k_millis();
        int32_t got = 0;
        int runs = 0;
        while (k_millis() - t0 < 100) { vm_run(&p, &s_sys, 0, 10000, &got); runs++; }
        const uint32_t steps = vm_last_steps();
        snprintf(d, sizeof(d), "%d runs of %lu instructions in 100 ms, about %lu k instr/s",
                 runs, (unsigned long)steps,
                 (unsigned long)((uint64_t)runs * steps / 100));
        sdk_case("how fast it runs", runs > 0 && got == 55, d);
    }
}

