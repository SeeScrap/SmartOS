# Working on this

*[อ่านฉบับภาษาไทย](CONTRIBUTING.th.md)*

The project is an operating system written from the kernel upwards for one
board. It is meant to be read, built and taken further - this file is what you
need to do that without reading all 19,000 lines first.

## Getting it running

```
pio run -t upload -t monitor
```

One command on a fresh clone. PlatformIO fetches ESP-IDF and littlefs; the
version of littlefs is pinned in `dependencies.lock`.

Then **read what the board prints**. It checks itself at every boot and says so:

```
--- stage 5.4c: the app toolkit, fed a script ---
  headers are skipped          step 0 -> 1, DOWN -> 2 (2 is a header's neighbour) OK
  ...
```

74 checks on an ordinary boot. If one of them says FAIL on hardware that should
be working, that is a bug worth reporting with the whole line - the detail
column is there so the number that was wrong is in the report.

### The hardware

An ESP32-S3-DevKitC-1 **N16R8** (16 MB flash, 8 MB octal PSRAM), a 2.4"
240x320 SPI panel with an ST7789V, and six buttons. Wiring is in
[src/board/pins.h](src/board/devkitc_n16r8.h); the panel's pins are chosen to land on
the FSPI IOMUX pads, which is what allows 80 MHz.

**Buttons: one side to the GPIO, the other to GND.** Nothing else - the chip's
internal pull-ups are enabled, so a pressed button reads 0 and no resistor is
needed. UP 4, DOWN 5, LEFT 6, RIGHT 7, OK 15, BACK 16. On a tactile switch use
the diagonal pins; the two on the same side are already connected to each
other, and using those is the same as holding the button down forever.

Without buttons you can still drive everything: `key up`, `hold back`, `min`
in the serial shell inject the same events the sampler publishes.

**GPIO 26-37 are off limits** on this module - they are the octal flash and
PSRAM, and touching them stops the board booting.

## The house rules

These are not style preferences; they are what keeps the thing working.

**1. Nothing is claimed that was not measured on the board.** Every number in
the README came out of a test that runs on hardware. If you add a feature, add
the check that proves it, and put the measurement in the line it prints -
`"47 of 50 frames in 1003 ms"`, not `"frame rate ok"`. A number is what makes a
regression visible later.

**2. Core 1 may not call ESP-IDF.** The kernel owns core 1, and FreeRTOS does
not exist there: no `printf`, no `malloc`, no driver calls, no `ESP_LOG`. Use
`k_snprintf` (kernel/kfmt.h), `kmalloc`, and the mailbox (`kmbox_call`) for
anything that has to happen on core 0. An app that breaks this does not crash
politely - it corrupts a core.

**3. A call whose request lives on the caller's stack has no timeout.** If it
gave up waiting, core 0 would write the answer into stack memory that belongs
to something else by then. The filesystem calls are the example to copy.

**4. Build from the command line into `.pio/cli`:**

```
PLATFORMIO_BUILD_DIR=.pio/cli pio run -t upload
```

The VS Code PlatformIO extension runs builds of its own in `.pio/build`
whenever `platformio.ini` changes, and two builds in one folder collide in
whatever way the timing gives - "file does not exist" a moment after it was
written, a half-deleted component, permission denied. Keeping them apart is the
fix; see the long note at the bottom of `platformio.ini`.

**5. Say what the code is for, not what it does.** The comments here explain
the decision - why the reservation falls back instead of failing, why BACK is a
tap and not a hold, what was tried first and broke. The code already says what
it does.

## Adding an app

Copy [src/apps/app_hello.cpp](src/apps/app_hello.cpp). That file is the whole
of an app: four callbacks and a name, with the registration line at the bottom.

```c
APP_REGISTER(app_mything, 60);     // 60 is its place in Home's list
```

Nothing else in the tree needs to mention it. CMake picks up every `.cpp` in
`src/apps/`, the component is linked whole-archive so a file nobody calls is
still linked, and that line puts it in the registry before `app_main` runs.
**After adding a new file, delete `.pio/cli/s3-n16r8/` once** so CMake's glob
is evaluated again.

What an app gets, and the rules it lives by, are at the top of
[src/os/app.h](src/os/app.h). The pieces its screen is built from -
lists, bars, the on-screen keyboard, "press it again" confirmation - are in
[src/ui/ui.h](src/ui/ui.h) and [src/ui/keyboard.h](src/ui/keyboard.h); use
them rather than growing a fifth copy of a scrolling list, which is what stage
5.4c was about.

## Adding a test

Tests live in `src/tests/`, one file per area, and print one line each. The
ones that need apps running are inside `#ifdef SMARTOS_TEST_APPS` so an ordinary
build does not carry them.

- Logic that needs no hardware → a new case in `tests/test_ui.cpp` or its
  neighbours; it runs on every boot.
- Something that needs apps → `tests/test_apps.cpp` or a file of its own, its
  entry point declared in `tests/tests.h`, and a call from `system_loop()` in
  `src/system.cpp` inside the `#ifdef`.
- The harness - `pump`, `press`, `tap`, `wait_gone`, `count_colour`, `log_has`
  - is in `tests/harness.h`.

**Pace injected events.** A DOWN press is held back 150 ms to see whether BACK
follows it (that chord minimises an app), so a burst of taps sent microseconds
apart is not what a finger does. `tap(); pump(250);` is.

The other test builds are listed at the bottom of `platformio.ini` with what
each one must print - the kernel under load, a filesystem filled to the end, an
app that never yields.

## Where the layers are

```
components/smartos/   the kernel and what it stands on, as its own component
  kernel/     scheduler, context switch, allocator, mailbox, locks, the park
  arch/xtensa/  the vector table, the context switch, the atomics
  port/       this chip: core 1, the park interrupt, the console, the heap
include/      smartos.h (kernel), smartos_app.h (apps), smartos_config.h
src/os/       event bus, app model, files, settings, log, shell, screen, radio
src/gfx/      framebuffer, damage tracking, font, pictures
src/board/    the board itself: panel, buttons, console
src/ui/       what every app's screen is made of
src/apps/     the apps
src/tests/    every claim in the README, as code
examples/     kernel_blink/, a separate project on the kernel alone
tools/        the host side: shell driver, screenshots, image and font tools
```

Two checks worth running before a commit, both of which have already caught
things nobody was looking for:

```
python tools/boot_check.py      the board's self-tests, counted and named
python tools/check_links.py     every link in the documents points at a file
```

The README is the long version: each stage says what was built, what it
measured, and - where it applies - what was got wrong first and how it was
found. The mistakes are kept on purpose. They are the part worth reading.

## Things that are not done

Open, and a reasonable place to start:

- **Live button presses** have still never been checked by hand; everything
  about the buttons is proved with injected events (stage 3.6 in the README).
- **External keyboard and mouse** over USB host (stage 6.4c): the plan and the
  obstacle - USB host wants the same pins as the console - are in the README.
- **Sensors**: the device service has GPIO and ADC. I2C, SPI and pulse counting
  are not there yet (`src/os/dev.h`).
- **Networking** beyond scanning: the radio comes up and surveys the band, but
  nothing connects to an access point yet (`src/os/radio.h`).
- **A pointer**: the UI is built for six buttons. Nothing in it knows what a
  cursor is.
