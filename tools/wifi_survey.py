"""Measures what the radio hears, so two antennas can be compared (stage 6.3).

    python tools/wifi_survey.py without.json            take a survey
    python tools/wifi_survey.py with.json               take another one
    python tools/wifi_survey.py without.json with.json  compare them

One scan is not a measurement: the same access point moves several dB between
scans, and a weak one drops out of half of them. So a survey is N scans (5 by
default), and each access point is kept by its BSSID - its radio's own address,
which is the only thing that is the same one from scan to scan - with the
median of the RSSIs it was seen at and how many scans it appeared in.

Comparing two surveys is what answers "is the antenna doing anything": the same
BSSIDs, side by side, with the difference in dB. +3 dB is twice the power.
Access points seen in only one of the two are listed separately - appearing at
all is itself a result.

    --scans N    how many scans to average (default 5)
    --port COM4  serial port, if it cannot be found on its own
"""
import json
import re
import statistics
import sys
import time

sys.path.insert(0, __import__("os").path.dirname(__file__))
from shell_test import Board, PROMPT, find_port   # noqa: E402

ROW = re.compile(r"^\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(\S+)\s+"
                 r"([0-9a-f]{2}(?::[0-9a-f]{2}){5})\s+(.*)$")


def survey(port, scans):
    b = Board(port)
    b.s.write(b"\r")
    b.read_until(PROMPT, 3)
    b.cmd("wifi up", timeout=20)

    seen = {}
    for i in range(scans):
        out, dt = b.cmd("wifi scan", timeout=40)
        rows = 0
        for line in out.splitlines():
            m = ROW.match(line)
            if not m:
                continue
            ch, mhz, rssi, auth, bssid, ssid = m.groups()
            e = seen.setdefault(bssid, {"ssid": ssid.strip(), "ch": int(ch),
                                        "mhz": int(mhz), "auth": auth, "rssi": []})
            e["rssi"].append(int(rssi))
            rows += 1
        print(f"  scan {i + 1}/{scans}: {rows} access points in {dt:.1f} s")
        time.sleep(0.5)

    for e in seen.values():
        e["median"] = int(statistics.median(e["rssi"]))
        e["best"] = max(e["rssi"])
        e["seen"] = len(e["rssi"])
    return {"scans": scans, "aps": seen}


def show(data):
    aps = data["aps"]
    print(f"\n  {len(aps)} access point(s) over {data['scans']} scans")
    print("   ch   MHz  median  best  seen  ssid")
    print("   --  ----  ------  ----  ----  ------------------------")
    for bssid, e in sorted(aps.items(), key=lambda kv: -kv[1]["median"]):
        print(f"   {e['ch']:2d}  {e['mhz']:4d}  {e['median']:6d}  {e['best']:4d}"
              f"  {e['seen']:2d}/{data['scans']}  {e['ssid'] or '(hidden)'}")
    by_ch = {}
    for e in aps.values():
        by_ch.setdefault(e["ch"], []).append(e["median"])
    print("   per channel:", ", ".join(f"ch{c} {len(v)}ap/{max(v)}dBm"
                                       for c, v in sorted(by_ch.items())))


def compare(a, b):
    """a = the first survey, b = the second. Positive dB means b heard better."""
    print(f"\n  {a['file']} -> {b['file']}")
    print("   ch  median A  median B   change  seen A  seen B  ssid")
    print("   --  --------  --------  -------  ------  ------  ---------------------")
    both = []
    for bssid, ea in a["aps"].items():
        eb = b["aps"].get(bssid)
        if not eb:
            continue
        d = eb["median"] - ea["median"]
        both.append(d)
        print(f"   {ea['ch']:2d}  {ea['median']:8d}  {eb['median']:8d}  {d:+6d}  "
              f"{ea['seen']:4d}/{a['scans']}  {eb['seen']:4d}/{b['scans']}  "
              f"{ea['ssid'] or '(hidden)'}")
    only_a = [e for k, e in a["aps"].items() if k not in b["aps"]]
    only_b = [e for k, e in b["aps"].items() if k not in a["aps"]]
    for e in only_a:
        print(f"   {e['ch']:2d}  {e['median']:8d}         -        -  "
              f"{e['seen']:4d}/{a['scans']}       -  {e['ssid'] or '(hidden)'}  (only in A)")
    for e in only_b:
        print(f"   {e['ch']:2d}         -  {e['median']:8d}        +       -  "
              f"{e['seen']:4d}/{b['scans']}  {e['ssid'] or '(hidden)'}  (only in B)")
    if both:
        print(f"\n  {len(both)} access point(s) in both: median change "
              f"{int(statistics.median(both)):+d} dB, best {max(both):+d}, worst {min(both):+d}")
        print(f"  seen only in A: {len(only_a)}, only in B: {len(only_b)}")
        print("  (+3 dB is twice the power; +10 dB is ten times)")


def main():
    args = [a for a in sys.argv[1:]]
    scans, port = 5, None
    out = []
    i = 0
    while i < len(args):
        if args[i] == "--scans":  scans = int(args[i + 1]); i += 2
        elif args[i] == "--port": port = args[i + 1]; i += 2
        else: out.append(args[i]); i += 1

    if not out:
        print(__doc__)
        return 1

    if len(out) == 2 and all(__import__("os").path.exists(f) for f in out):
        a, b = (json.load(open(f)) for f in out)
        a["file"], b["file"] = out
        compare(a, b)
        return 0

    path = out[0]
    data = survey(port or find_port(), scans)
    json.dump(data, open(path, "w"), indent=1)
    show(data)
    print(f"\n  written to {path}")
    return 0


sys.exit(main())
