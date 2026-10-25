"""Counts the self-tests the board runs at boot, and says what changed.

    python tools/boot_check.py                     check against the baseline
    python tools/boot_check.py --save              write the baseline instead
    python tools/boot_check.py --baseline F --save for the -D SMARTOS_TEST_APPS build

Resets the board, captures the boot log up to the shell's first prompt, and
counts every line the test harness printed. Prints one summary line:

    PASS 51 FAIL 0  missing: -  new: -

Counting alone is not enough. A change that stops a test from running still
leaves the count right if another test was added, so the names are compared
against a committed baseline too: `missing` is a test that used to run and no
longer does, `new` is one that was not there before. Both are worth seeing.

Exit code is the number of failures plus the number of differences, so this
can gate a commit. Uses pyserial through tools/shell_test.py:
    ~/.platformio/penv/Scripts/python.exe tools/boot_check.py
"""
import argparse
import collections
import re
import sys
import time

from shell_test import PROMPT, Board, find_port, port_kind

# A test case is a line the board printed as "  <name> <detail> OK|FAIL", from
# any of the five printers that use that column layout (harness.cpp:17 and :22,
# test_evbus.cpp:28, test_input.cpp:76, test_ui.cpp:30) plus the two the park
# test builds by hand in main.cpp. Requiring the two leading spaces is what
# keeps IDF's own "SPI SRAM memory test OK" and the demo tasks' "[consumer]
# ... OK" out: those start at column 0.
CASE = re.compile(r"^  (\S.*?)\s+(OK|FAIL)\s*$")

# The name is padded to a fixed width, so the detail starts after a run of
# spaces - except when the name is longer than its column, where one space is
# all there is. Then the fixed width is the only honest cut.
NAME_COL = 28
SPLIT = re.compile(r"  +")


def case_name(text):
    """A stable id for a case, from the text before the OK or FAIL."""
    head = SPLIT.split(text, 1)[0]
    if head == text:                     # the name overflowed its column
        head = text[:NAME_COL]
    return head.strip()


DEFAULT_BASELINE = "tools/baseline_default.txt"


def capture(timeout):
    """(boot log, seconds to the prompt) or (None, elapsed) if it never came."""
    port = find_port()
    if not port:
        print("no board found on any serial port")
        return None, 0.0
    b = Board(port)
    print(f"port {port} ({port_kind(port)})")
    b.reset()
    t0 = time.time()
    log = b.read_until(b"[shell] type help", timeout)
    if log is None:
        print(f"the shell never came up in {timeout} s; last bytes seen:")
        print(b.buf[-400:].decode(errors="replace"))
        return None, time.time() - t0
    tail = b.read_until(PROMPT, 5) or b""
    return (log + tail).decode(errors="replace"), time.time() - t0


def cases(log):
    """[(name, ok)] in the order the board printed them."""
    out = []
    for line in log.splitlines():
        m = CASE.match(line.rstrip())
        if m:
            out.append((case_name(m.group(1)), m.group(2) == "OK"))
    return out


def differences(seen, baseline):
    """(missing, new) as name*count, comparing two lists of names."""
    a, b = collections.Counter(baseline), collections.Counter(seen)
    fmt = lambda c: ", ".join(n if k == 1 else f"{n} x{k}"
                              for n, k in sorted(c.items())) or "-"
    return fmt(a - b), fmt(b - a)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--baseline", default=DEFAULT_BASELINE)
    p.add_argument("--save", action="store_true", help="write the baseline")
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--log", help="also write the raw boot log here")
    a = p.parse_args()

    log, secs = capture(a.timeout)
    if log is None:
        return 1
    if a.log:
        with open(a.log, "w", encoding="utf-8") as f:
            f.write(log)

    found = cases(log)
    if not found:
        print(f"the board booted in {secs:.1f} s but printed no test cases at all")
        return 1
    names = [n for n, _ in found]
    failed = [n for n, ok in found if not ok]

    if a.save:
        with open(a.baseline, "w", encoding="utf-8") as f:
            f.write("".join(n + "\n" for n in names))
        print(f"PASS {len(found) - len(failed)} FAIL {len(failed)}  "
              f"wrote {len(names)} names to {a.baseline}")
        return len(failed)

    try:
        with open(a.baseline, encoding="utf-8") as f:
            base = [l.strip() for l in f if l.strip()]
    except FileNotFoundError:
        print(f"no baseline at {a.baseline}; run once with --save")
        return 1

    missing, new = differences(names, base)
    print(f"PASS {len(found) - len(failed)} FAIL {len(failed)}  "
          f"missing: {missing}  new: {new}   ({secs:.1f} s to the prompt)")
    for n in failed:
        print(f"  FAIL  {n}")
    return len(failed) + (missing != "-") + (new != "-")


if __name__ == "__main__":
    sys.exit(main())
