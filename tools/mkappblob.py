"""Turns a .app file into the C array the firmware embeds.

    python tools/sappc.py examples/apps/selftest.sap selftest.app
    python tools/mkappblob.py selftest.app src/tests/test_app_blob.h

The board installs that app on itself at every boot, so the whole install
path - write the file, read the header, check it, take memory from the kernel
pool, register it, run its callbacks, give the memory back - is a test that
runs without anybody holding a cable. See src/tests/test_appload.cpp.
"""
import os
import sys

HEADER = '''#pragma once

// A compiled SmartOS app, embedded so the board can install one on itself.
//
// GENERATED. Do not edit:
//     python tools/sappc.py examples/apps/selftest.sap selftest.app
//     python tools/mkappblob.py selftest.app src/tests/test_app_blob.h
//
// The source is examples/apps/selftest.sap. It is here so that the install
// path - write the file, read the header, check it, give it memory, register
// it, run it, give the memory back - is checked on every boot rather than by
// hand with a cable.
//
// ไฟล์นี้ถูกสร้างขึ้น ห้ามแก้ด้วยมือ มีไว้ให้บอร์ดติดตั้งแอปใส่ตัวเองทุกครั้งที่บูต
// เพื่อให้เส้นทางการติดตั้งถูกตรวจทุกครั้ง แทนที่จะตรวจด้วยมือผ่านสาย

static const unsigned char k_test_app[] = {
'''


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    data = open(sys.argv[1], "rb").read()
    rows = []
    for i in range(0, len(data), 12):
        rows.append("    " + " ".join("0x%02X," % b for b in data[i:i + 12]))
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as f:
        f.write(HEADER + "\n".join(rows) + "\n};\n")
    print(f"{sys.argv[2]}: {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
