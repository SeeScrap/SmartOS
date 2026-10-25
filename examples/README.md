# Examples / ตัวอย่าง

Two ways to write code for SmartOS, and they are genuinely different jobs.

มีสองทางในการเขียนโค้ดบน SmartOS และทั้งสองเป็นคนละงานกันจริงๆ

---

## 1. A program on the kernel — `kernel_blink/`

**Its own project.** It is not part of the operating system's build, does not
share its `sdkconfig`, and borrows exactly one thing: the SmartOS component,
by path.

```
cd examples/kernel_blink
PLATFORMIO_BUILD_DIR=.pio/cli pio run -t upload
```

What it links against is the whole of it:

```cmake
REQUIRES
    smartos     # the kernel, the architecture and the port
    esp_rom     # esp_rom_printf and the GPIO pad select, for the blink
    soc         # the GPIO registers the blink task writes
```

No event bus, no app manager, no filesystem, no screen, no toolkit. It runs
two tasks sharing a counter through a mutex, a task that sleeps on the tick, a
task that drives a pin, and reports what happened in the same format the
board's own self-tests use — so `tools/boot_check.py` reads it too.

**นี่คือข้อพิสูจน์ของทั้งโปรเจกต์** ถ้าโปรแกรมนี้ลิงก์ติดและทำงานได้ แปลว่า
เคอร์เนลแยกออกจากระบบปฏิบัติการได้จริง ไม่ใช่แค่คำพูดใน README

### Starting the kernel is not the same as using it

`kernel_blink/src/main.cpp` includes four headers rather than one:

| | |
|---|---|
| `smartos.h` | tasks, locks, the clock, the heap, printing. What a program **on** the kernel uses. |
| `port/kcore1.h` | waking core 1, which ESP-IDF leaves asleep |
| `port/kpark.h` | parking it around a flash write |
| `kernel/kmbox.h`, `kernel/kfault.h` | core 1 asking core 0 for things; noticing a crash |

The last four are integration, not application code, and they are the part
that differs most between chips. They are reached for by name on purpose, so
that the line between "using the kernel" and "bringing it up" stays visible.

การปลุกเคอร์เนลกับการใช้เคอร์เนลเป็นคนละเรื่อง ส่วนที่ปลุกคือส่วนที่ต่างกัน
มากที่สุดระหว่างชิป จึงให้เรียกถึงตรงๆ เพื่อให้เส้นแบ่งยังมองเห็นได้

---

## 2. An app on the operating system

There is no template file here, because the repository already has one that is
compiled, run and tested on every build:

**[`src/apps/app_hello.cpp`](../src/apps/app_hello.cpp)** — copy it.

ไม่มีไฟล์เทมเพลตในโฟลเดอร์นี้ เพราะโปรเจกต์มีอยู่แล้ว และมันเป็นแอปจริงที่ถูก
คอมไพล์ รัน และทดสอบทุกครั้งที่บิลด์ ซึ่งเชื่อถือได้มากกว่าเทมเพลตที่ไม่มีใครบิลด์

Adding an app is one file:

```cpp
#include "smartos_app.h"

typedef struct { uint32_t presses; } state_t;

static void draw(app_ctx_t *c, fb_t *fb, bool full) { /* ... */ }

static const app_t app_mything = {
    .name      = "mything",
    .about     = "one line, shown under Home's list",
    .user_size = sizeof(state_t),
    .draw      = draw,            // the only callback that is required
};
APP_REGISTER(app_mything, 60);    // 60 is its place in Home's list
```

Nothing else in the tree mentions it. It compiles because it sits in
`src/apps/`, links because the component is whole-archive, and registers
itself from a static constructor before `app_main` runs. Delete
`.pio/build/s3-n16r8/` once after adding a file, so CMake's glob notices it.

`os/app.h` is the full contract: what the runtime hands an app, when each
callback is called, and the rules an app has to live by.

### One include, and it is a promise

An app includes `smartos_app.h` and nothing else. Everything reachable from
that header is the application interface, and
`SMARTOS_ABI_VERSION` in `include/smartos_config.h` is its version.

That matters because of what comes next: stage 7 installs apps that were
**built separately from the operating system**, as files, without reflashing
it. Such an app is linked against whatever that header reaches, so every name
in it has to keep working. Adding something to it is a decision, not a
tidying-up.

แอปใช้ `smartos_app.h` ไฟล์เดียว ทุกอย่างที่เอื้อมถึงจากไฟล์นั้นคือ
อินเทอร์เฟซของแอปพลิเคชัน และเป็น **คำสัญญา** เพราะ stage 7 จะติดตั้งแอปที่
สร้างแยกจากระบบปฏิบัติการ โดยไม่ต้องแฟลช OS ใหม่

---

## Which one am I writing?

| | a program on the kernel | an app on the OS |
|---|---|---|
| lives in | its own project | one file in `src/apps/` |
| includes | `smartos.h` | `smartos_app.h` |
| gets | tasks, locks, clock, heap | all of that, plus screen, events, files, settings, UI |
| draws | nothing — there is no screen layer | through `draw()`, on the app area |
| started by | its own `app_main` | the user, from Home |
| needs | a board and a serial console | the whole operating system |
