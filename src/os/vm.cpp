#include "os/vm.h"

#include "port/kport.h"

// ---------------------------------------------------------------------------
// The interpreter. See os/vm.h for why installed apps are interpreted at all.
//
// The shape of this file is one switch in one loop, and it stays that way.
// Every check is written out where it happens rather than hidden in a helper,
// because the checks are the reason this machine exists: an app cannot reach
// past its own memory, cannot call a system function that is not in the
// table, and cannot run for longer than it was given.
//
// K_IRAM: an app's draw() is called from the compositor on core 1, and core 1
// must not depend on the flash cache.
// ---------------------------------------------------------------------------

static K_DRAM uint32_t s_steps;

uint32_t vm_last_steps(void) { return s_steps; }

const char *vm_err_name(vm_err_t e)
{
    switch (e) {
        case VM_OK:          return "ok";
        case VM_HALTED:      return "halted";
        case VM_ERR_OPCODE:  return "unknown instruction";
        case VM_ERR_RANGE:   return "index out of range";
        case VM_ERR_STACK:   return "stack over- or underflow";
        case VM_ERR_DIV0:    return "divide by zero";
        case VM_ERR_BUDGET:  return "ran too long";
        case VM_ERR_SYSCALL: return "no such system call";
        case VM_ERR_DEPTH:   return "calls nested too deep";
    }
    return "?";
}

// How deep calls may nest. Each level costs one entry in a small array on the
// interpreter's own stack, so this is also what keeps an app's recursion from
// eating core 1's stack.
#define VM_MAX_DEPTH 16

typedef struct {
    uint32_t return_pc;
    uint16_t frame_base;      // where this call's locals start on the stack
    uint8_t  nargs;
} vm_frame_t;

// Reading operands out of the code. Each one checks it is still inside the
// program before it reads - a truncated or corrupt file cannot walk off the
// end into whatever follows it in memory.
#define NEED(n)   do { if (pc + (n) > p->code_len) { err = VM_ERR_RANGE; goto done; } } while (0)
#define U8()      (p->code[pc++])
#define I8()      ((int8_t)p->code[pc++])
#define U16()     (pc += 2, (uint16_t)(p->code[pc - 2] | (p->code[pc - 1] << 8)))
#define I16()     ((int16_t)U16())

#define PUSH(v)   do { if (sp >= p->stack_len) { err = VM_ERR_STACK; goto done; } \
                       p->stack[sp++] = (v); } while (0)
#define NEED_SP(n) do { if (sp < (n)) { err = VM_ERR_STACK; goto done; } } while (0)
// Two pops, in two statements. Written as one declaration with two
// initialisers it is a single full expression that modifies sp twice, which
// GCC rejects outright under -Werror=sequence-point - and is genuinely
// ambiguous, so the compiler is right.
#define POP2()    NEED_SP(2); const int32_t b = p->stack[--sp]; const int32_t a = p->stack[--sp]

K_IRAM vm_err_t vm_run(const vm_program_t *p, const vm_syscalls_t *sys,
                       uint32_t entry, uint32_t budget, int32_t *out)
{
    if (!p || !p->code) return VM_ERR_RANGE;
    if (entry >= p->code_len) return VM_ERR_RANGE;

    uint32_t pc    = entry;
    uint16_t sp    = 0;
    uint16_t base  = 0;            // this frame's locals start here
    uint8_t  depth = 0;
    vm_frame_t frames[VM_MAX_DEPTH];
    vm_err_t err  = VM_OK;
    uint32_t left = budget;

    s_steps = 0;

    for (;;) {
        if (left-- == 0) { err = VM_ERR_BUDGET; goto done; }
        s_steps++;
        NEED(1);
        const uint8_t op = U8();

        switch (op) {
            case VM_HALT: err = VM_HALTED; goto done;

            case VM_PUSH_I8: { NEED(1); PUSH(I8()); break; }
            case VM_PUSH_K:  { NEED(2); const uint16_t k = U16();
                               if (k >= p->const_len) { err = VM_ERR_RANGE; goto done; }
                               PUSH(p->consts[k]); break; }
            case VM_DUP:     { NEED_SP(1); const int32_t v = p->stack[sp - 1]; PUSH(v); break; }
            case VM_DROP:    { NEED_SP(1); sp--; break; }
            case VM_SWAP:    { NEED_SP(2); const int32_t a = p->stack[sp - 1];
                               p->stack[sp - 1] = p->stack[sp - 2];
                               p->stack[sp - 2] = a; break; }

            case VM_LD_G: { NEED(2); const uint16_t i = U16();
                            if (i >= p->global_len) { err = VM_ERR_RANGE; goto done; }
                            PUSH(p->globals[i]); break; }
            case VM_ST_G: { NEED(2); const uint16_t i = U16();
                            if (i >= p->global_len) { err = VM_ERR_RANGE; goto done; }
                            NEED_SP(1); p->globals[i] = p->stack[--sp]; break; }

            // Locals live on the operand stack, from `base` up. The bound is
            // the stack's length, so a frame cannot read another frame's.
            case VM_LD_L: { NEED(1); const uint16_t i = (uint16_t)(base + U8());
                            if (i >= sp) { err = VM_ERR_RANGE; goto done; }
                            PUSH(p->stack[i]); break; }
            case VM_ST_L: { NEED(1); const uint16_t i = (uint16_t)(base + U8());
                            if (i >= sp) { err = VM_ERR_RANGE; goto done; }
                            NEED_SP(1); p->stack[i] = p->stack[--sp]; break; }

            // ARITHMETIC IS DONE UNSIGNED, AND THAT IS NOT A STYLE CHOICE.
            // Signed overflow is undefined behaviour in C++: the compiler is
            // entitled to assume it never happens and to rewrite the code
            // around it on that assumption. This build is -O2, the numbers
            // come out of a file somebody else wrote, and this machine's
            // whole claim is that every way out is a checked error rather
            // than a fault. Unsigned arithmetic is defined to wrap, and
            // wrapping is what a 32-bit integer in this language means.
            // VM_SHL below has always done it this way; now so does the rest.
            case VM_ADD: { POP2(); PUSH((int32_t)((uint32_t)a + (uint32_t)b)); break; }
            case VM_SUB: { POP2(); PUSH((int32_t)((uint32_t)a - (uint32_t)b)); break; }
            case VM_MUL: { POP2(); PUSH((int32_t)((uint32_t)a * (uint32_t)b)); break; }

            // INT32_MIN / -1 is the one division that overflows, and it is as
            // undefined as dividing by zero - on some chips it traps the same
            // way. Dividing by zero is an error the app can be told about;
            // this one has an answer, so it is given the wrapped answer
            // rather than being turned into a second kind of failure.
            case VM_DIV: { POP2();
                           if (b == 0) { err = VM_ERR_DIV0; goto done; }
                           if (a == INT32_MIN && b == -1) { PUSH(INT32_MIN); break; }
                           PUSH(a / b); break; }
            case VM_MOD: { POP2();
                           if (b == 0) { err = VM_ERR_DIV0; goto done; }
                           if (a == INT32_MIN && b == -1) { PUSH(0); break; }
                           PUSH(a % b); break; }
            case VM_NEG: { NEED_SP(1); const int32_t v = p->stack[--sp];
                           PUSH((int32_t)(0u - (uint32_t)v)); break; }

            case VM_AND: { POP2(); PUSH(a & b); break; }
            case VM_OR:  { POP2(); PUSH(a | b); break; }
            case VM_XOR: { POP2(); PUSH(a ^ b); break; }
            // Shifts are masked to 31 rather than checked: a shift by 32 is
            // undefined in C, and an app asking for one is not worth stopping.
            case VM_SHL: { POP2();
                           PUSH((int32_t)((uint32_t)a << (b & 31))); break; }
            case VM_SHR: { POP2(); PUSH(a >> (b & 31)); break; }

            case VM_EQ: { POP2(); PUSH(a == b); break; }
            case VM_NE: { POP2(); PUSH(a != b); break; }
            case VM_LT: { POP2(); PUSH(a <  b); break; }
            case VM_LE: { POP2(); PUSH(a <= b); break; }
            case VM_GT: { POP2(); PUSH(a >  b); break; }
            case VM_GE: { POP2(); PUSH(a >= b); break; }

            case VM_JMP: { NEED(2); const int16_t d = I16();
                           const int64_t t = (int64_t)pc + d;
                           if (t < 0 || t >= p->code_len) { err = VM_ERR_RANGE; goto done; }
                           pc = (uint32_t)t; break; }
            case VM_JZ:
            case VM_JNZ: { NEED(2); const int16_t d = I16(); NEED_SP(1);
                           const int32_t v = p->stack[--sp];
                           if ((op == VM_JZ) == (v == 0)) {
                               const int64_t t = (int64_t)pc + d;
                               if (t < 0 || t >= p->code_len) { err = VM_ERR_RANGE; goto done; }
                               pc = (uint32_t)t;
                           }
                           break; }

            case VM_CALL: { NEED(4);
                            const uint16_t addr    = U16();
                            const uint8_t  nargs   = U8();
                            const uint8_t  nlocals = U8();
                            if (addr >= p->code_len) { err = VM_ERR_RANGE; goto done; }
                            if (depth >= VM_MAX_DEPTH) { err = VM_ERR_DEPTH; goto done; }
                            NEED_SP(nargs);
                            // The arguments are already on the stack and
                            // become the first locals; the rest are zeroed.
                            for (uint8_t i = 0; i < nlocals; ++i) PUSH(0);
                            frames[depth].return_pc  = pc;
                            frames[depth].frame_base = base;
                            frames[depth].nargs      = nargs;
                            depth++;
                            base = (uint16_t)(sp - nargs - nlocals);
                            pc   = addr;
                            break; }

            case VM_RET: { NEED_SP(1);
                           const int32_t v = p->stack[--sp];
                           if (depth == 0) { if (out) *out = v; err = VM_OK; goto done; }
                           depth--;
                           sp   = base;                 // drop args and locals
                           pc   = frames[depth].return_pc;
                           base = frames[depth].frame_base;
                           PUSH(v);
                           break; }

            case VM_SYS: { NEED(2);
                           const uint8_t  num   = U8();
                           const uint8_t  nargs = U8();
                           if (!sys || num >= sys->count || !sys->table[num]) {
                               err = VM_ERR_SYSCALL; goto done;
                           }
                           NEED_SP(nargs);
                           sp = (uint16_t)(sp - nargs);
                           vm_err_t serr = VM_OK;
                           const int32_t r = sys->table[num](sys->user, &p->stack[sp], nargs, &serr);
                           if (serr != VM_OK) { err = serr; goto done; }
                           PUSH(r);
                           break; }

            default: err = VM_ERR_OPCODE; goto done;
        }
    }

done:
    if (err == VM_HALTED && out) *out = (sp > 0) ? p->stack[sp - 1] : 0;
    return err;
}
