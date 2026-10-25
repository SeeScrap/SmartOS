"""Installs an app on the board, over the same USB cable everything else uses.

    python tools/install_app.py counter.app
    python tools/install_app.py counter.app --name mycounter
    python tools/install_app.py --list
    python tools/install_app.py --remove counter

The operating system is not rebuilt and not reflashed. The file goes to
/fs/installed/ on the board's own filesystem, which is a different partition
from the firmware - so installing an app cannot touch the OS, and that is
checkable: the md5 of the `factory` partition is the same before and after.

Base64 over the line-based shell, because that shell already exists and
tools/shell_test.py already drives it. An app is a few hundred bytes.
"""
import argparse
import base64
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shell_test import PROMPT, Board, find_port   # noqa: E402

LINE = 60          # base64 characters per line, well inside SHELL_LINE


def connect():
    port = find_port()
    if not port:
        print("no board found on any serial port")
        return None
    b = Board(port)
    b.s.write(b"\r")
    if b.read_until(PROMPT, 5) is None:
        print("the shell did not answer - is the default firmware flashed?")
        return None
    return b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", help="the .app file to install")
    ap.add_argument("--name", help="install it under a different name")
    ap.add_argument("--list", action="store_true", help="what is installed")
    ap.add_argument("--remove", metavar="APP", help="uninstall one")
    a = ap.parse_args()

    b = connect()
    if not b:
        return 1

    if a.list:
        out, _ = b.cmd("apps", timeout=10)
        print(out.strip())
        return 0

    if a.remove:
        out, _ = b.cmd(f"uninstall {a.remove}", timeout=15)
        print(out.strip())
        return 0

    if not a.file:
        ap.print_help()
        return 1

    data = open(a.file, "rb").read()
    # The name comes out of the file, not out of the filename: the board
    # checks the two agree, and this is the end that can be sure of it.
    inside = data[8:24].rstrip(bytes([0])).decode(errors="replace")
    name = a.name or inside
    if name != inside:
        print(f"  the file calls itself {inside!r}; the board will refuse {name!r}")
        return 1
    print(f"{a.file}: {len(data)} bytes -> {name}")

    out, _ = b.cmd(f"install {name} {len(data)}", timeout=10)
    if "send" not in out:
        print(out.strip())
        return 1

    # No prompt comes back between these: the board is reading data, not
    # commands. Send them, then "end", and wait for the prompt after that.
    text = base64.b64encode(data).decode()
    t0 = time.time()
    for i in range(0, len(text), LINE):
        b.send_raw(text[i:i + LINE].encode() + b"\r")
        time.sleep(0.004)
    out, _ = b.cmd("end", timeout=20)
    print(out.strip())
    print(f"  {len(text)} base64 characters in {time.time() - t0:.1f} s")
    return 0 if "installed" in out else 1


if __name__ == "__main__":
    sys.exit(main())
