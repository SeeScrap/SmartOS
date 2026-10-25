"""Compiles a SmartOS app into a .app file.

    python tools/sappc.py examples/apps/tally.sap tally.app

The language is small on purpose. It is not C and is not trying to be: it is
the smallest thing in which the apps this operating system actually has can be
written, and every feature in it earns its place by being needed by one of
them.

    app tally {
        order 100
        var count

        on draw {
            if (full) {
                text(8, 6, "Tally", 0x56BF, 2)
                text(8, 100, "any button counts up", 0x5AEB, 1)
            }
            text_bg(8, 50, fmt("pressed %d times", count), 0xFFFF, 0, 1)
        }

        on event {
            if (ev_type == 0 && ev_a == 1) {
                count = count + 1
                invalidate()
            }
        }
    }

WHAT IS IN IT
  var            a value the app keeps. 32-bit, signed.
  on init/event/draw/stop   the four moments the system calls an app
  fun name(a, b) { }        your own functions, with their own variables
  if / else, while
  + - * / %, == != < <= > >=, && || !, and & | ^ << >>
  "strings", numbers in decimal or 0x hex
  calls to the system by name - the list is in src/os/vm_sys.h

WHAT IS NOT, and why
  No arrays, no structs, no pointers. The machine has no addresses at all
  (see src/os/vm.h), which is exactly why an installed app cannot hurt the
  system - so this is a property to keep, not a gap to fill. An app that
  needs a table today uses several variables; if that becomes the thing
  holding apps back, the machine gets an indexed-global instruction and this
  gets `[]`, in that order.

FOUR NAMES ARE ALREADY TAKEN. `full` is true when draw() is starting with a
freshly cleared screen. `ev_type`, `ev_a` and `ev_b` are the event that
on-event was called for. They are globals 0..3, which the loader fills in.

It compiles to the assembly language tools/mkapp.py reads, and hands it to
that to turn into bytes - so the one assembler is the only thing that knows
the instruction encoding, and the intermediate can be printed with --asm when
something looks wrong.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkapp                                             # noqa: E402

RESERVED = {"full": 0, "ev_type": 1, "ev_a": 2, "ev_b": 3}
KEYWORDS = {"app", "order", "var", "fun", "on", "if", "else", "while", "return",
            "globals", "stack"}
ENTRIES = {"init", "event", "draw", "stop"}


class Err(Exception):
    def __init__(self, msg, line=0):
        super().__init__(f"line {line}: {msg}" if line else msg)


# --- words ------------------------------------------------------------------

TOKEN = re.compile(r"""
    (?P<ws>\s+|\#[^\n]*)
  | (?P<num>0[xX][0-9a-fA-F]+|\d+)
  | (?P<str>"(?:[^"\\]|\\.)*")
  | (?P<name>[A-Za-z_][A-Za-z_0-9]*)
  | (?P<op><<|>>|<=|>=|==|!=|&&|\|\||[-+*/%()<>={},!&|^])
""", re.X)


def lex(text):
    out, pos, line = [], 0, 1
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if not m:
            raise Err(f"cannot read {text[pos]!r}", line)
        line += text.count("\n", pos, m.end())
        pos = m.end()
        kind = m.lastgroup
        if kind == "ws":
            continue
        out.append((kind, m.group(), line))
    out.append(("eof", "", line))
    return out


# --- the shape of a program -------------------------------------------------

class P:
    def __init__(self, toks):
        self.t, self.i = toks, 0

    def peek(self, n=0):
        return self.t[min(self.i + n, len(self.t) - 1)]

    def next(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def at(self, text):
        k, v, _ = self.peek()
        return v == text

    def eat(self, text):
        if not self.at(text):
            k, v, ln = self.peek()
            raise Err(f"expected {text!r}, found {v!r}", ln)
        return self.next()

    def accept(self, text):
        if self.at(text):
            self.next()
            return True
        return False


def parse(toks):
    p = P(toks)
    p.eat("app")
    kind, name, ln = p.next()
    if kind != "name":
        raise Err("the app needs a name", ln)
    p.eat("{")

    app = {"name": name, "order": 100, "globals": [], "init": {}, "funs": {},
           "entries": {}, "stack": 48}
    while not p.at("}"):
        k, v, ln = p.peek()
        if v == "order":
            p.next()
            app["order"] = int(p.next()[1], 0)
        elif v == "stack":
            p.next()
            app["stack"] = int(p.next()[1], 0)
        elif v == "var":
            p.next()
            n = p.next()[1]
            if n in RESERVED or n in app["globals"]:
                raise Err(f"{n} is already a name here", ln)
            app["globals"].append(n)
            if p.accept("="):
                app["init"][n] = int(p.next()[1], 0)
        elif v == "fun":
            p.next()
            fname = p.next()[1]
            p.eat("(")
            params = []
            while not p.at(")"):
                params.append(p.next()[1])
                if not p.at(")"):
                    p.eat(",")
            p.eat(")")
            app["funs"][fname] = {"params": params, "body": block(p), "locals": list(params)}
        elif v == "on":
            p.next()
            which = p.next()[1]
            if which not in ENTRIES:
                raise Err(f"there is no '{which}' moment; try {sorted(ENTRIES)}", ln)
            app["entries"][which] = block(p)
        else:
            raise Err(f"did not expect {v!r} here", ln)
    p.eat("}")
    return app


def block(p):
    p.eat("{")
    out = []
    while not p.at("}"):
        out.append(stmt(p))
    p.eat("}")
    return out


def stmt(p):
    k, v, ln = p.peek()
    if v == "if":
        p.next(); p.eat("(")
        cond = expr(p)
        p.eat(")")
        then = block(p)
        other = block(p) if p.accept("else") else []
        return ("if", cond, then, other, ln)
    if v == "while":
        p.next(); p.eat("(")
        cond = expr(p)
        p.eat(")")
        return ("while", cond, block(p), ln)
    if v == "var":
        p.next()
        name = p.next()[1]
        val = expr(p) if p.accept("=") else ("num", 0, ln)
        return ("local", name, val, ln)
    if v == "return":
        p.next()
        if p.at("}"):
            return ("return", ("num", 0, ln), ln)
        return ("return", expr(p), ln)
    if k == "name" and p.peek(1)[1] == "=":
        name = p.next()[1]
        p.next()
        return ("assign", name, expr(p), ln)
    return ("expr", expr(p), ln)


def expr(p):      return binary(p, 0)

LEVELS = [["||"], ["&&"], ["|"], ["^"], ["&"],
          ["==", "!="], ["<", "<=", ">", ">="], ["<<", ">>"],
          ["+", "-"], ["*", "/", "%"]]


def binary(p, level):
    if level >= len(LEVELS):
        return unary(p)
    left = binary(p, level + 1)
    while p.peek()[1] in LEVELS[level]:
        op = p.next()[1]
        right = binary(p, level + 1)
        left = ("bin", op, left, right, p.peek()[2])
    return left


def unary(p):
    k, v, ln = p.peek()
    if v in ("-", "!"):
        p.next()
        return ("un", v, unary(p), ln)
    return primary(p)


def primary(p):
    k, v, ln = p.next()
    if k == "num":
        return ("num", int(v, 0), ln)
    if k == "str":
        return ("str", v[1:-1].encode().decode("unicode_escape"), ln)
    if v == "(":
        e = expr(p)
        p.eat(")")
        return e
    if k == "name":
        if p.at("("):
            p.next()
            args = []
            while not p.at(")"):
                args.append(expr(p))
                if not p.at(")"):
                    p.eat(",")
            p.eat(")")
            return ("call", v, args, ln)
        return ("name", v, ln)
    raise Err(f"did not expect {v!r} in an expression", ln)


# --- turning it into assembly ------------------------------------------------

BINOPS = {"+": "add", "-": "sub", "*": "mul", "/": "div", "%": "mod",
          "&": "and", "|": "or", "^": "xor", "<<": "shl", ">>": "shr",
          "==": "eq", "!=": "ne", "<": "lt", "<=": "le", ">": "gt", ">=": "ge"}


class Gen:
    def __init__(self, app):
        self.app = app
        self.out = []
        self.consts = {}        # value -> name
        self.strings = {}       # text  -> name
        self.n = 0
        self.locals = None      # name -> index, while inside a function
        self.globals = dict(RESERVED)
        for i, g in enumerate(app["globals"]):
            self.globals[g] = 4 + i

    def label(self, stem):
        self.n += 1
        return f"{stem}_{self.n}"

    def emit(self, line):
        self.out.append("    " + line)

    def const(self, value):
        if value not in self.consts:
            self.consts[value] = f"k{len(self.consts)}"
        return self.consts[value]

    def string(self, text):
        if text not in self.strings:
            self.strings[text] = f"s{len(self.strings)}"
        return self.strings[text]

    def push_num(self, v, ln):
        if -128 <= v <= 127:
            self.emit(f"push {v}")
        else:
            self.emit(f"pushk #{self.const(v)}")

    def load(self, name, ln):
        if self.locals is not None and name in self.locals:
            self.emit(f"ldl {self.locals[name]}")
        elif name in self.globals:
            self.emit(f"ldg {self.globals[name]}")
        else:
            raise Err(f"there is no variable called {name}", ln)

    def store(self, name, ln):
        if self.locals is not None and name in self.locals:
            self.emit(f"stl {self.locals[name]}")
        elif name in self.globals:
            self.emit(f"stg {self.globals[name]}")
        else:
            raise Err(f"there is no variable called {name} - declare it with var", ln)

    # --- expressions, each leaving exactly one value on the stack ------------
    def expr(self, e):
        kind = e[0]
        if kind == "num":
            self.push_num(e[1], e[2])
        elif kind == "str":
            self.emit(f"str {self.string(e[1])}")
        elif kind == "name":
            self.load(e[1], e[2])
        elif kind == "un":
            _, op, sub, ln = e
            self.expr(sub)
            if op == "-":
                self.emit("neg")
            else:                       # ! is "is it zero"
                self.emit("push 0")
                self.emit("eq")
        elif kind == "bin":
            _, op, a, b, ln = e
            if op in ("&&", "||"):
                # Short-circuit, so that `x != 0 && 100 / x > 2` is safe.
                end = self.label("sc")
                self.expr(a)
                self.emit("dup")
                self.emit(f"{'jz' if op == '&&' else 'jnz'} {end}")
                self.emit("drop")
                self.expr(b)
                self.out.append(f"{end}:")
            else:
                self.expr(a)
                self.expr(b)
                self.emit(BINOPS[op])
        elif kind == "call":
            _, name, args, ln = e
            for a in args:
                self.expr(a)
            if name in mkapp.SYSCALLS:
                self.emit(f"sys {name} {len(args)}")
            elif name in self.app["funs"]:
                f = self.app["funs"][name]
                if len(args) != len(f["params"]):
                    raise Err(f"{name} takes {len(f['params'])} argument(s)", ln)
                extra = len(f["locals"]) - len(f["params"])
                self.emit(f"call fn_{name} {len(args)} {extra}")
            else:
                raise Err(f"nothing called {name} to call", ln)
        else:
            raise Err(f"cannot work out {kind}")

    # --- statements, each leaving the stack as it found it -------------------
    def stmt(self, s):
        kind = s[0]
        if kind == "expr":
            self.expr(s[1])
            self.emit("drop")
        elif kind == "assign":
            self.expr(s[2])
            self.store(s[1], s[3])
        elif kind == "local":
            if self.locals is None:
                raise Err("var inside a block only works in a fun", s[3])
            if s[1] not in self.locals:
                self.locals[s[1]] = len(self.locals)
            self.expr(s[2])
            self.emit(f"stl {self.locals[s[1]]}")
        elif kind == "if":
            _, cond, then, other, ln = s
            els, end = self.label("else"), self.label("endif")
            self.expr(cond)
            self.emit(f"jz {els if other else end}")
            for st in then:
                self.stmt(st)
            if other:
                self.emit(f"jmp {end}")
                self.out.append(f"{els}:")
                for st in other:
                    self.stmt(st)
            self.out.append(f"{end}:")
        elif kind == "while":
            _, cond, body, ln = s
            top, end = self.label("while"), self.label("endwhile")
            self.out.append(f"{top}:")
            self.expr(cond)
            self.emit(f"jz {end}")
            for st in body:
                self.stmt(st)
            self.emit(f"jmp {top}")
            self.out.append(f"{end}:")
        elif kind == "return":
            self.expr(s[1])
            self.emit("ret")
        else:
            raise Err(f"cannot work out {kind}")


def collect_locals(fn):
    """Every var declared anywhere in a function's body is a local."""
    names = list(fn["params"])

    def walk(stmts):
        for s in stmts:
            if s[0] == "local" and s[1] not in names:
                names.append(s[1])
            elif s[0] == "if":
                walk(s[2]); walk(s[3])
            elif s[0] == "while":
                walk(s[2])
    walk(fn["body"])
    fn["locals"] = names


def compile_text(text):
    app = parse(lex(text))
    for fn in app["funs"].values():
        collect_locals(fn)

    g = Gen(app)
    body = []

    # Functions first, so an entry point's code does not run into them.
    for name, fn in app["funs"].items():
        g.out = []
        g.locals = {n: i for i, n in enumerate(fn["locals"])}
        for s in fn["body"]:
            g.stmt(s)
        g.emit("push 0")            # a function that falls off the end gives 0
        g.emit("ret")
        body.append((f"fn_{name}", None, g.out))
    g.locals = None

    for which in ("init", "event", "draw", "stop"):
        if which not in app["entries"]:
            continue
        g.out = []
        for s in app["entries"][which]:
            g.stmt(s)
        g.emit("push 0")
        g.emit("ret")
        body.append((None, which, g.out))

    lines = []
    # Functions are jumped to by name, so they need a label; entry points are
    # named by a directive instead.
    for label, entry, code in body:
        lines.append("")
        if label:
            lines.append(f"{label}:")
        if entry:
            lines.append(f".{entry}")
        lines.extend(code)

    # Globals with a starting value are set by init, which the compiler adds
    # to if the app did not write one. This happens BEFORE the header is
    # built, because it can need constants of its own - getting that the
    # wrong way round left the .const lines out and the assembler refused
    # them, which is how it was found.
    if app["init"]:
        pre = []
        for name, value in app["init"].items():
            idx = g.globals[name]
            if -128 <= value <= 127:
                pre.append(f"    push {value}")
            else:
                pre.append(f"    pushk #{g.const(value)}")
            pre.append(f"    stg {idx}")
        if ".init" in lines:
            at = lines.index(".init")
            lines[at + 1:at + 1] = pre
        else:
            lines += ["", ".init"] + pre + ["    push 0", "    ret"]

    head = ["; compiled by tools/sappc.py - do not edit",
            f".name    {app['name']}",
            f".order   {app['order']}",
            f".globals {4 + len(app['globals'])}",
            f".stack   {app['stack']}"]
    for text_, nm in g.strings.items():
        head.append(f'.str  {nm} "{text_}"')
    for value, nm in g.consts.items():
        head.append(f".const {nm} {value}")

    lines = head + lines

    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--asm", action="store_true", help="print the assembly and stop")
    a = ap.parse_args()

    try:
        asm = compile_text(open(a.source, encoding="utf-8").read())
    except Err as e:
        print(f"{a.source}: {e}")
        return 1

    if a.asm or not a.out:
        print(asm)
        return 0
    try:
        blob = mkapp.assemble(asm)
    except mkapp.Err as e:
        print(f"{a.source}: the compiler produced assembly the assembler refused: {e}")
        print(asm)
        return 1
    open(a.out, "wb").write(blob)
    print(f"{a.out}: {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
