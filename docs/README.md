# docs/

What is kept here, and why.

| folder | what it holds |
| --- | --- |
| `screens/` | screenshots of the running board, taken with `tools/screenshot.py` over the serial shell - not mock-ups |
| `radio/` | Wi-Fi surveys from `tools/wifi_survey.py`: five scans each, medians per BSSID. Two pairs taken with and without an external antenna (stage 6.3), and one confirming the board is back on its printed antenna |
| `art/` | the wallpaper's source picture. `tools/mkimage.py` turns it into `src/gfx/wallpaper_data.cpp` |
| `prototype_arduino/` | where this project started: the same board brought up on the Arduino core, before stage 2.0 moved it to bare ESP-IDF so the second core could be claimed. Kept because the README refers back to it; nothing in the build touches it |

The measurements these support are written up in the main
[README](../README.md), stage by stage.

---

## ภาษาไทย

| โฟลเดอร์ | เก็บอะไร |
| --- | --- |
| `screens/` | ภาพหน้าจอของบอร์ดที่กำลังทำงานจริง ถ่ายด้วย `tools/screenshot.py` ผ่านเชลล์อนุกรม ไม่ใช่ภาพจำลอง |
| `radio/` | ผลสำรวจ Wi-Fi จาก `tools/wifi_survey.py` สแกนชุดละ 5 รอบ เก็บค่ากลางรายอุปกรณ์ มีสองคู่ที่วัดตอนต่อและไม่ต่อเสาอากาศภายนอก (stage 6.3) และอีกหนึ่งชุดยืนยันว่ากลับมาใช้เสาบนแผ่นวงจรแล้ว |
| `art/` | รูปต้นฉบับของภาพพื้นหลัง `tools/mkimage.py` แปลงมันเป็น `src/gfx/wallpaper_data.cpp` |
| `prototype_arduino/` | จุดเริ่มต้นของโปรเจกต์: บอร์ดตัวเดียวกันตอนที่ยังใช้ Arduino ก่อน stage 2.0 จะย้ายมาเป็น ESP-IDF เปล่าเพื่อให้ยึดคอร์ที่สองได้ เก็บไว้เพราะ README อ้างถึง ไม่มีส่วนไหนของการ build แตะมัน |

**หมายเหตุเรื่องความเป็นส่วนตัว:** ไฟล์ผลสำรวจใน `radio/` เป็นค่าที่วัดได้จริง
แต่ตัวระบุตัวตนถูกแทนที่แล้ว — access point ถูกไล่หมายเลขตามลำดับที่พบครั้งแรก
และใช้หมายเลขเดียวกันทุกไฟล์ การเปรียบเทียบจึงยังทำงานได้ โดยไม่ต้องเผยแพร่
เราเตอร์ของเพื่อนบ้านไปด้วย
