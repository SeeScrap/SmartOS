"""Saves the board's screen as a PNG, through the serial shell (stage 5.3b).

    python tools/screenshot.py shot.png                      the screen as it is
    python tools/screenshot.py shot.png "open tasks" "key left"   type these first

Commands are typed one by one, with a pause after each so the app can draw,
then `screenshot` is typed and its hex rows are decoded. The board is not
reset. Uses PlatformIO's Python (pyserial); PNG writing is standard library.
"""
import struct
import sys
import time
import zlib

sys.path.insert(0, __import__("os").path.dirname(__file__))
from shell_test import Board, PROMPT, find_port   # noqa: E402


def write_png(path, w, h, rgb_rows):
    raw = b"".join(b"\x00" + row for row in rgb_rows)

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    b = Board(find_port())
    b.s.write(b"\r")
    b.read_until(PROMPT, 3)
    for cmd in sys.argv[2:]:
        b.cmd(cmd)
        time.sleep(1.5)

    b.s.write(b"screenshot\r")
    if b.read_until(b"SHOT ", 5) is None:
        print("no SHOT header")
        return 1
    head = b.read_until(b"\n", 2).decode().split()
    w, h = int(head[0]), int(head[1])
    body = b.read_until(b"END", 30)
    rows = [l.strip() for l in body.decode(errors="replace").splitlines() if len(l.strip()) == w * 4]
    if len(rows) != h:
        print(f"got {len(rows)} of {h} rows")
        return 1
    out = []
    for r in rows:
        line = bytearray()
        for x in range(w):
            v = int(r[x * 4:x * 4 + 4], 16)
            v = ((v & 0xFF) << 8) | (v >> 8)       # wire order back to RGB565
            rr, gg, bb = (v >> 11) & 31, (v >> 5) & 63, v & 31
            line += bytes(((rr * 255) // 31, (gg * 255) // 63, (bb * 255) // 31))
        out.append(bytes(line))
    write_png(sys.argv[1], w, h, out)
    print(f"{sys.argv[1]}: {w} x {h}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
