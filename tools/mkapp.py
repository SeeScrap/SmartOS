"""Builds a .app file for SmartOS out of a tiny assembly language.

    python tools/mkapp.py counter.sapp counter.app

The language is one instruction per line, with labels, and it exists so that
an app can be written at all before the compiler does. It is not meant to be
pleasant:

    ; comments start with a semicolon
    .name   counter          the name Home shows
    .order  100              where it sits in Home's list
    .globals 8               32-bit words of state (the first four are the system's)
    .stack  32
    .str    greeting "hello" declares a string, referred to as $greeting
    .const  big 100000       a constant too large for push, referred to as #big

    .draw                    the entry point for draw(); also .init .event .stop
        push 8
        push 8
        str greeting
        push 65535
        push 2
        sys text 5           call system call 9 (text) with 5 arguments
        ret

WHAT THE SYSTEM RESERVES. globals[0] is draw()'s "is this a fresh screen"
flag. globals[1..3] carry an event: type, a, b. An app declares at least four
globals and uses 4 and up for itself.

The file format is in src/os/appload.h.
"""
import re
import struct
import sys

MAGIC = 0x50504153          # "SAPP"
ABI = 1                     # must match SMARTOS_ABI_VERSION
HDR = 56
NONE = 0xFFFFFFFF

# name -> (opcode, operand kinds). "i8" is one signed byte, "u16"/"i16" two
# little-endian, "u8" one. "rel" is a label, stored as an i16 offset from the
# end of the instruction.
OPS = {
    "halt": (0x00, []),
    "push": (0x01, ["i8"]),      "pushk": (0x02, ["u16"]),
    "dup":  (0x03, []),          "drop":  (0x04, []),      "swap": (0x05, []),
    "ldg":  (0x06, ["u16"]),     "stg":   (0x07, ["u16"]),
    "ldl":  (0x08, ["u8"]),      "stl":   (0x09, ["u8"]),
    "add":  (0x10, []), "sub": (0x11, []), "mul": (0x12, []),
    "div":  (0x13, []), "mod": (0x14, []), "neg": (0x15, []),
    "and":  (0x18, []), "or":  (0x19, []), "xor": (0x1A, []),
    "shl":  (0x1B, []), "shr": (0x1C, []),
    "eq":   (0x20, []), "ne":  (0x21, []), "lt":  (0x22, []),
    "le":   (0x23, []), "gt":  (0x24, []), "ge":  (0x25, []),
    "jmp":  (0x30, ["rel"]), "jz": (0x31, ["rel"]), "jnz": (0x32, ["rel"]),
    "call": (0x33, ["lbl16", "u8", "u8"]),
    "ret":  (0x34, []),
    "sys":  (0x40, ["u8", "u8"]),
}

# The system calls, by name, so an app does not have to remember numbers.
# This list and the enum in src/os/vm_sys.h are the same thing; if they ever
# disagree the board refuses the app rather than doing the wrong call.
SYSCALLS = [
    "invalidate", "millis", "rand", "rgb", "bg", "clear", "fill_rect", "rect",
    "line", "text", "text_bg", "text_width", "fmt", "num", "setting_get",
    "setting_set", "log", "width", "height",
]


class Err(Exception):
    pass


def assemble(text):
    name, order, nglobals, nstack = "", 100, 8, 32
    strings, consts = {}, {}            # name -> offset / index
    blob, pool = bytearray(), []
    entries = {"init": NONE, "event": NONE, "draw": NONE, "stop": NONE}
    labels, patches, code = {}, [], bytearray()
    pending_entry = None

    def emit_rel(label, at_end):
        patches.append(("rel", len(code), label, at_end))
        code.extend(b"\0\0")

    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.split(";")[0].strip()
        if not line:
            continue
        try:
            if line.startswith("."):
                w = line.split(None, 2)
                d = w[0][1:]
                if d == "name":        name = w[1]
                elif d == "order":     order = int(w[1], 0)
                elif d == "globals":   nglobals = int(w[1], 0)
                elif d == "stack":     nstack = int(w[1], 0)
                elif d == "str":
                    key = w[1]
                    val = re.match(r'\s*"(.*)"\s*$', w[2]).group(1)
                    strings[key] = len(blob)
                    blob.extend(val.encode() + b"\0")
                elif d == "const":
                    consts[w[1]] = len(pool)
                    pool.append(int(w[2], 0))
                elif d in entries:
                    pending_entry = d
                else:
                    raise Err(f"unknown directive .{d}")
                continue

            if line.endswith(":"):
                labels[line[:-1]] = len(code)
                continue

            if pending_entry is not None:
                entries[pending_entry] = len(code)
                pending_entry = None

            parts = line.split()
            op = parts[0].lower()
            if op == "str":                      # push a string handle
                if parts[1] not in strings:
                    raise Err(f"no string called {parts[1]}")
                code.append(OPS["pushk"][0])
                key = f"__str_{parts[1]}"
                if key not in consts:
                    consts[key] = len(pool)
                    pool.append(strings[parts[1]])
                code.extend(struct.pack("<H", consts[key]))
                continue
            if op not in OPS:
                raise Err(f"no instruction called {op}")
            opcode, kinds = OPS[op]
            args = parts[1:]
            if op == "sys" and args and not args[0].isdigit():
                if args[0] not in SYSCALLS:
                    raise Err(f"no system call called {args[0]}")
                args[0] = str(SYSCALLS.index(args[0]))
            if len(args) != len(kinds):
                raise Err(f"{op} takes {len(kinds)} operand(s), got {len(args)}")
            code.append(opcode)
            for kind, a in zip(kinds, args):
                if kind == "i8":
                    v = int(a, 0)
                    if not -128 <= v <= 127:
                        raise Err(f"{v} does not fit in push; use .const and pushk")
                    code.extend(struct.pack("<b", v))
                elif kind == "u8":
                    code.append(int(a, 0) & 0xFF)
                elif kind == "u16":
                    idx = consts[a[1:]] if a.startswith("#") else int(a, 0)
                    code.extend(struct.pack("<H", idx))
                elif kind == "rel":
                    emit_rel(a, None)
                elif kind == "lbl16":
                    patches.append(("abs", len(code), a, None))
                    code.extend(b"\0\0")
        except Err:
            raise
        except Exception as e:
            raise Err(f"{e}") from None
        else:
            continue

    for kind, at, label, _ in patches:
        if label not in labels:
            raise Err(f"no label called {label}")
        if kind == "abs":
            struct.pack_into("<H", code, at, labels[label])
        else:
            struct.pack_into("<h", code, at, labels[label] - (at + 2))

    if not name:
        raise Err("no .name")
    if entries["draw"] == NONE:
        raise Err("no .draw - an app has to be able to put something on screen")
    if nglobals < 4:
        raise Err("at least four globals: the system uses 0..3")

    body = b"".join(struct.pack("<i", c) for c in pool) + bytes(code) + bytes(blob)
    hdr = struct.pack("<IHH16sHHHHII IIII".replace(" ", ""),
                      MAGIC, ABI, 0, name.encode()[:16], order,
                      nglobals, nstack, len(pool), len(code), len(blob),
                      entries["init"], entries["event"], entries["draw"], entries["stop"])
    assert len(hdr) == HDR, len(hdr)
    return hdr + body


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    try:
        out = assemble(open(sys.argv[1], encoding="utf-8").read())
    except Err as e:
        print(f"{sys.argv[1]}: {e}")
        return 1
    open(sys.argv[2], "wb").write(out)
    print(f"{sys.argv[2]}: {len(out)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
