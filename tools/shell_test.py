"""Stage 5.1 - drives the serial shell from the PC, the way a person would.

    python tools/shell_test.py            (the default firmware must be flashed)

Resets the board, waits for the shell's first prompt, then types each command
and checks the answer. Prints one line per check, OK or FAIL, and exits with
the number of failures. Uses pyserial, which PlatformIO already has:
    ~/.platformio/penv/Scripts/python.exe tools/shell_test.py
"""
import re
import sys
import time

import serial
import serial.tools.list_ports

PROMPT = b"s3> "


# The board can be reached two ways, and stage 6.4c is why it matters: the
# console sits on the chip's own USB (303A) today, and USB host needs that
# peripheral for the keyboard - so the console moves to UART0, which reaches
# this computer through the board's second USB-C socket and whatever bridge
# chip is behind it. Espressif's own VID is tried first, then the usual
# bridges, so both arrangements work without editing anything.
PORT_VIDS = (
    (0x303A, "Espressif USB Serial/JTAG"),   # the chip's own USB, GPIO19/20
    (0x1A86, "CH340/CH343 bridge"),          # the commonest on these boards
    (0x10C4, "CP210x bridge"),
    (0x0403, "FTDI bridge"),
)


def find_port(prefer=None):
    """The board's serial port. `prefer` is a VID to look for first."""
    ports = list(serial.tools.list_ports.comports())
    order = ([prefer] if prefer else []) + [v for v, _ in PORT_VIDS]
    for vid in order:
        for p in ports:
            if p.vid == vid:
                return p.device
    for p in ports:                          # last resort: a raw hwid match
        if p.hwid and "303A" in p.hwid.upper():
            return p.device
    return None


def port_kind(port):
    """What is behind that port, for a test report to say so."""
    for p in serial.tools.list_ports.comports():
        if p.device != port:
            continue
        for vid, name in PORT_VIDS:
            if p.vid == vid:
                return name
        return f"vid {p.vid:04X}" if p.vid else "unknown"
    return "not present"


class Board:
    def __init__(self, port):
        # DTR and RTS set low BEFORE the port opens. pyserial's default raises
        # them on open, and on this chip's USB port that is a reset - found
        # when every screenshot rebooted the board and closed the app being
        # photographed. reset() below is the deliberate one.
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = 115200
        self.s.timeout = 0.05
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = b""

    def reset(self):
        # Windows' usbser.sys only sends a new RTS along with a DTR write, so
        # DTR is written again after each RTS change - esptool's work-around.
        # (It used to work only because opening the port wrote both.)
        self.s.dtr = False
        self.s.rts = True
        self.s.dtr = self.s.dtr
        time.sleep(0.15)
        self.s.rts = False
        self.s.dtr = self.s.dtr
        self.s.reset_input_buffer()
        self.buf = b""

    def read_until(self, token, timeout):
        """Everything up to and including token; None on timeout."""
        end = time.time() + timeout
        while time.time() < end:
            i = self.buf.find(token)
            if i >= 0:
                out, self.buf = self.buf[: i + len(token)], self.buf[i + len(token):]
                return out
            self.buf += self.s.read(4096)
        return None

    def send_raw(self, data):
        self.s.write(data)

    def cmd(self, line, timeout=5.0):
        """Types a line, returns (text up to the next prompt, seconds taken)."""
        t0 = time.time()
        self.s.write(line.encode() + b"\r")
        out = self.read_until(PROMPT, timeout)
        dt = time.time() - t0
        text = out.decode(errors="replace") if out is not None else ""
        # The heartbeat and [slog] echoes can land in the middle; leave them out.
        text = "\n".join(l for l in text.splitlines()
                         if not l.startswith("[alive]") and not l.startswith("[slog]"))
        return text, dt


fails = 0


def check(what, ok, detail=""):
    global fails
    if not ok:
        fails += 1
    print(f"  {what:<34} {detail[:70]:<70} {'OK' if ok else 'FAIL'}")


def events_of(ps_text, app):
    """(state, input events) of an app in ps, or (None, None) if it is not there."""
    m = re.search(rf"^\s+\d+ {app}\s+(running, focus|running|closing)\s+\d+\s+(\d+)",
                  ps_text, re.M)
    return (m.group(1), int(m.group(2))) if m else (None, None)


def last_line(text):
    """The last line of an answer that is not the prompt."""
    lines = [l.strip() for l in text.splitlines() if l.strip() and not l.startswith("s3>")]
    return lines[-1] if lines else ""


def main():
    port = find_port()
    if not port:
        print("no Espressif USB port found")
        return 1
    b = Board(port)
    b.reset()
    t0 = time.time()
    if b.read_until(b"[shell] type help", 90) is None or b.read_until(PROMPT, 5) is None:
        print(f"the shell never came up; {time.time() - t0:.1f} s, last bytes seen:")
        print(b.buf[-400:].decode(errors="replace"))
        return 1
    print(f"shell up {time.time() - t0:.1f} s after reset\n")

    out, dt = b.cmd("help")
    names = ["help", "ps", "free", "df", "log", "ls", "cat", "open", "close", "kill", "set",
             "key", "hold", "min", "screenshot", "bootscreen", "wifi", "reboot"]
    have = [n for n in names if re.search(rf"^\s+{n}\s", out, re.M)]
    check("help", len(have) == len(names) and "apps: home" in out,
          f"{len(have)} of {len(names)} commands listed; answered in {dt * 1000:.0f} ms")

    out, _ = b.cmd("ps")
    check("ps", "stack used/total" in out and re.search(r"home\s+running, focus", out) is not None,
          "task table, and Home running with focus")
    time.sleep(1.0)
    out, _ = b.cmd("ps")
    m = re.search(r"core 1: busy (\d+\.\d)%, parked (\d+\.\d)%, idle (\d+\.\d)% over (\d+) ms", out)
    total = sum(float(m.group(i)) for i in (1, 2, 3)) if m else 0
    check("ps: where core 1's time went", m is not None and 99.0 <= total <= 101.0,
          m.group(0) if m else "no core 1 line")

    # Stage 5.4d/5.4e: the counters for what the system refused, and what it
    # did not send while the screen was off. On a board nobody has pushed to a
    # limit they all read zero, which is the point - a number that is not zero
    # is a thing to go and look at.
    m = re.search(r"frames: (\d+) sent, (\d+) taken while the screen was off; "
                  r"timers refused (\d+); log batches lost (\d+)", out)
    check("ps: what was refused", m is not None and m.group(3) == "0" and m.group(4) == "0",
          m.group(0)[:78] if m else "no counters line")

    out, _ = b.cmd("free")
    check("free", all(k in out for k in ("internal RAM", "PSRAM", "kernel heap")),
          " / ".join(re.findall(r"(\d+ KB free)", out)[:2]))
    m = re.search(r"one app may take (\d+) B of it .*the bus has (\d+) of (\d+) slots free", out)
    check("free: the per-app ceiling", m is not None and int(m.group(1)) == 32768 and
          int(m.group(2)) > 0, m.group(0)[:78] if m else "no ceiling line")

    # Stage 6.1: the boot screen, painted again so it can be looked at without
    # rebooting - and the figures from the boot that actually happened.
    out, _ = b.cmd("bootscreen")
    m = re.search(r"boot screen painted; it was up (\d+) ms at boot, (\d+) steps, (\d+) frames", out)
    check("bootscreen", m is not None and int(m.group(1)) >= 1500 and int(m.group(2)) >= 8 and
          int(m.group(3)) > int(m.group(2)),
          f"{m.group(1)} ms at boot, {m.group(2)} steps, {m.group(3)} frames" if m
          else out.strip()[:60])

    # Stage 6.3: the radio answers about itself without being started.
    out, _ = b.cmd("wifi")
    m = re.search(r"2\.4 GHz Wi-Fi b/g/n: (\w+)\s+5 GHz: (.+)", out)
    check("wifi: what the chip is", m is not None and m.group(1) == "yes" and
          m.group(2).startswith("no"), m.group(0)[:70] if m else out.strip()[:60])

    out, _ = b.cmd("df")
    m = re.search(r"/fs\s+(\d+) KB used of (\d+) KB", out)
    check("df", m is not None, m.group(0) if m else out.strip()[:60])

    out, _ = b.cmd("ls /fs/sys")
    check("ls", "log.txt" in out and "settings" in out, "log.txt and settings in /fs/sys")

    refused = 0
    for bad in ("ls /", "ls /fs/../", "cat /fs/sys/../../etc", "cat ../x", "ls /fsx"):
        o, _ = b.cmd(bad)
        refused += "only paths under /fs" in o
    check("paths outside /fs refused", refused == 5, f"{refused} of 5")

    out, _ = b.cmd("cat /fs/sys/settings")
    check("cat", "backlight=" in out, last_line(out))

    out, _ = b.cmd("set")
    check("set: list", re.search(r"backlight\s+\d+\s+\(5\.\.100\)", out) is not None,
          last_line(out))
    o1, _ = b.cmd("set backlight 50")
    o2, _ = b.cmd("set backlight 500")
    o3, _ = b.cmd("set backlight abc")
    o4, _ = b.cmd("set nosuch 1")
    check("set: change, clamp, refuse",
          "backlight = 50" in o1 and "backlight = 100" in o2 and "clamped" in o2
          and "not a number" in o3 and "no setting" in o4,
          "50; 500 -> 100 clamped; abc and nosuch refused")
    b.cmd("set backlight 60")
    time.sleep(1.6)                          # settings save a second after the last change
    out, _ = b.cmd("cat /fs/sys/settings")
    check("set: saved to flash", "backlight=60" in out, "backlight=60 on flash 1.6 s later")
    b.cmd("set backlight 100")

    out, _ = b.cmd("open counter")
    m = re.search(r"counter opened, id (\d+)", out)
    time.sleep(0.3)
    ps1, _ = b.cmd("ps")
    state, ev0 = events_of(ps1, "counter")
    check("open", m is not None and state == "running, focus",
          f"{m.group(0) if m else last_line(out)}; ps: {state}")

    b.cmd("key ok ok ok")
    time.sleep(0.3)
    ps2, _ = b.cmd("ps")
    _, ev1 = events_of(ps2, "counter")
    got = (ev1 - ev0) if ev0 is not None and ev1 is not None else -1
    check("key reaches the focused app", got == 6,
          f"counter got {got} input events for 3 taps (want 6: press + release each)")

    out, _ = b.cmd("hold back")
    time.sleep(0.5)
    ps3, _ = b.cmd("ps")
    state, _ = events_of(ps3, "counter")
    check("hold back closes it", state is None and re.search(r"home\s+running, focus", ps3) is not None,
          "counter gone, Home in front")

    # Stage 5.5: minimise keeps it running, with Home in front.
    b.cmd("open counter")
    time.sleep(0.4)
    out, _ = b.cmd("min")
    time.sleep(0.4)
    ps_min, _ = b.cmd("ps")
    state, _ = events_of(ps_min, "counter")
    home_front = re.search(r"home\s+running, focus", ps_min) is not None
    check("min: minimised, still running", state == "running" and home_front,
          f"counter is {state}, Home in front: {home_front}")

    b.cmd("open counter")
    time.sleep(0.3)
    out, _ = b.cmd("kill counter")
    time.sleep(0.3)
    ps4, _ = b.cmd("ps")
    state, _ = events_of(ps4, "counter")
    o2, _ = b.cmd("kill home")
    o3, _ = b.cmd("kill counter")
    check("kill", "killed" in out and state is None and "part of the system" in o2
          and "not open" in o3, f"{last_line(out)}; then Home and a closed app refused")

    o1, _ = b.cmd("open nosuch")
    o2, _ = b.cmd("close home")
    o3, _ = b.cmd("close counter")
    check("open/close refusals", "no app called" in o1 and "part of the system" in o2
          and "not open" in o3, "unknown app; Home; an app that is not open")

    out, _ = b.cmd("log 6")
    lines = [l for l in out.splitlines()[1:] if l.startswith("#")]
    check("log n", len(lines) == 6 and any("shell open counter" in l for l in lines + [out]),
          f"{len(lines)} lines; the shell's own open is logged")
    out, _ = b.cmd("log")
    check("log (default 20)", len([l for l in out.splitlines() if l.startswith("#")]) == 20, "")

    b.s.write(b"screenshot\r")
    shot = (b.read_until(PROMPT, 20) or b"").decode(errors="replace")
    rows = [l for l in shot.splitlines() if len(l.strip()) == 320 * 4]
    check("screenshot", "SHOT 320 240" in shot and len(rows) == 240 and "END" in shot,
          f"{len(rows)} rows of 320 pixels")

    out, _ = b.cmd("frobnicate")
    check("unknown command", "unknown command 'frobnicate'" in out, "")

    out, _ = b.cmd("x" * 150)
    check("overlong line", "line too long" in out, "150 characters")

    b.send_raw(b"psx\x7f\r")                 # typed psx, backspace, Enter
    out = (b.read_until(PROMPT, 5) or b"").decode(errors="replace")
    check("backspace", "stack used/total" in out and "unknown" not in out, "'psx' + DEL runs ps")

    b.send_raw(b"df\r\n")                    # a terminal that sends CR LF
    time.sleep(0.5)
    b.buf += b.s.read(4096)
    out = b.buf.decode(errors="replace")
    b.buf = b""
    check("CR LF is one Enter", out.count("s3> ") == 1 and out.count("KB used of") == 1,
          f"{out.count('s3> ')} prompt(s)")

    t = time.time()
    out, dt = b.cmd("cat /fs/sys/log.txt", timeout=30)
    size = len(out.encode())
    check("cat a whole log", size > 1000, f"{size} bytes in {dt * 1000:.0f} ms")

    out, _ = b.cmd("reboot", timeout=2)
    t0 = time.time()
    up = b.read_until(b"[shell] type help", 90) is not None and b.read_until(PROMPT, 5) is not None
    out, _ = b.cmd("log 40") if up else ("", 0)
    check("reboot", up and "reboot from the shell" in out and "last reset: software restart" in out,
          f"back in {time.time() - t0:.1f} s; the log says why")

    print(f"\n{fails} FAIL" if fails else "\nall OK")
    return fails


if __name__ == "__main__":
    sys.exit(main())
