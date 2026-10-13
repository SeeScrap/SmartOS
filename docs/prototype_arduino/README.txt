Snapshot of the last working Arduino-framework build (stage 1 bring-up),
taken just before the move to ESP-IDF.

Verified on hardware at this commit point:
  PSRAM      8,386,279 bytes
  flash      16 MB
  SPI 80MHz  50.73 ms/frame -> 19.7 FPS, 9.08 MB/s
  SPI 40MHz  96.84 ms/frame -> 10.3 FPS, 4.76 MB/s

To go back: copy platformio.ini and src/ from here to the project root.
PlatformIO does not compile anything outside src/, so this folder is inert.
