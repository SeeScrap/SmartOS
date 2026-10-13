#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "os/app.h"

// ---------------------------------------------------------------------------
// Stage 4.4 - files.
//
// LittleFS on the 'storage' partition (about 12 MB), mounted at /fs. It
// survives losing power mid-write and spreads wear across the flash by itself.
// The filesystem lives on core 0, where the flash driver is; apps reach it
// through the mailbox, like the device service.
//
// EACH APP HAS ITS OWN FOLDER
//
// An app names a file - "best", "log.csv" - and gets /fs/apps/<app>/<name>.
// It cannot name anything else: a name is 1-32 characters of letters, digits,
// '.', '_' and '-', may not start with '.', and so can contain neither '/' nor
// "..". Two apps can both keep a file called "best" without ever meeting.
//
// WHOLE FILES, NOT HANDLES
//
// There is no open/read/close. Every call does one whole thing - replace a
// file, append to it, read it - and returns with nothing left open. So an app
// that is closed halfway through its life cannot leak a file handle, and the
// system never has to guess whether a handle still belongs to anyone.
//
// fs_write REPLACES ATOMICALLY: the data goes to a hidden temporary file that
// is renamed over the old one only once complete. Losing power in between
// leaves the old file whole, never half of each.
//
// THESE CALLS BLOCK, WITH NO TIMEOUT
//
// A request points at the app's own buffer, usually on its stack. A call that
// gave up waiting would return while core 0 might still be writing into that
// buffer - into stack memory by then reused for something else. So the call
// waits for core 0's answer however long it takes; a flash erase is tens of
// milliseconds, a large write longer. Core 1's scheduler keeps running while
// it waits: only this app is held up.
//
// --- ภาษาไทย ---------------------------------------------------------------
//
// stage 4.4 - ไฟล์
//
// ใช้ LittleFS บนพาร์ทิชัน 'storage' (ประมาณ 12 MB) เมานต์ไว้ที่ /fs มันทนต่อ
// ไฟดับกลางการเขียน และกระจายการสึกหรอทั่วทั้งแฟลชให้เอง ระบบไฟล์อยู่บน core 0
// ซึ่งเป็นที่ที่ไดรเวอร์แฟลชอยู่ แอปจึงเข้าถึงมันผ่าน mailbox เหมือนที่ทำกับบริการ
// ฮาร์ดแวร์
//
// แต่ละแอปมีโฟลเดอร์ของตัวเอง
//
// แอประบุชื่อไฟล์ เช่น "best" หรือ "log.csv" แล้วจะได้ /fs/apps/<ชื่อแอป>/<ชื่อไฟล์>
// มันระบุอย่างอื่นไม่ได้เลย - ชื่อต้องยาว 1-32 ตัวอักษร ประกอบด้วยตัวอักษร ตัวเลข
// '.', '_' และ '-' ห้ามขึ้นต้นด้วย '.' จึงมีทั้ง '/' และ ".." ไม่ได้ สองแอปจึงเก็บไฟล์
// ชื่อ "best" ได้ทั้งคู่โดยไม่เจอกันเลย
//
// ทำงานกับไฟล์ทั้งไฟล์ ไม่มี handle
//
// ไม่มี open/read/close ทุกการเรียกทำงานหนึ่งอย่างจบในตัว - แทนที่ไฟล์ ต่อท้าย
// หรืออ่าน - แล้วคืนค่าโดยไม่เหลืออะไรค้างเปิดไว้ แอปที่ถูกปิดกลางคันจึงทำ
// file handle รั่วไม่ได้ และระบบไม่ต้องเดาว่า handle ยังเป็นของใครอยู่หรือเปล่า
//
// fs_write แทนที่แบบ atomic: ข้อมูลถูกเขียนลงไฟล์ชั่วคราวที่ซ่อนไว้ แล้วเปลี่ยนชื่อ
// ทับไฟล์เดิมเมื่อเขียนครบแล้วเท่านั้น ไฟดับระหว่างนั้นจะเหลือไฟล์เก่าทั้งไฟล์
// ไม่มีทางได้ไฟล์ที่เป็นของเก่าครึ่งของใหม่ครึ่ง
//
// การเรียกเหล่านี้รอจนเสร็จ และไม่มี timeout
//
// คำขอชี้ไปยังบัฟเฟอร์ของแอปเอง ซึ่งมักอยู่บน stack ของมัน การเรียกที่เลิกรอ
// จะคืนค่ากลับไปทั้งที่ core 0 อาจยังเขียนลงบัฟเฟอร์นั้นอยู่ - ลงหน่วยความจำ stack
// ที่ตอนนั้นถูกเอาไปใช้ทำอย่างอื่นแล้ว การเรียกจึงรอคำตอบจาก core 0 นานเท่าไหร่
// ก็รอ การลบแฟลชใช้เวลาหลายสิบมิลลิวินาที การเขียนก้อนใหญ่นานกว่านั้น
// ตัวจัดคิวของ core 1 ยังทำงานต่อระหว่างรอ มีเฉพาะแอปตัวนี้ที่ถูกหยุดไว้
// ---------------------------------------------------------------------------

#define FS_NAME_MAX   32

#define FS_OK           0
#define FS_ERR_NAME    -1   // not a valid name
#define FS_ERR_NOENT   -2   // no such file
#define FS_ERR_IO      -3   // the filesystem refused, or it is not mounted
#define FS_ERR_FULL    -4   // no space left

// --- for apps: kernel tasks on core 1 -----------------------------------------------
int fs_write(app_ctx_t *c, const char *name, const void *data, size_t len);
int fs_append(app_ctx_t *c, const char *name, const void *data, size_t len);

// Reads up to max bytes; *got is how many. A file longer than max is read
// only in part - ask fs_size first if that matters.
int fs_read(app_ctx_t *c, const char *name, void *buf, size_t max, size_t *got);
int fs_size(app_ctx_t *c, const char *name, size_t *size);
int fs_remove(app_ctx_t *c, const char *name);

// This app's files, one name per line, NUL-terminated. Returns how many there
// are; if buf is too small the list is cut short, at a line boundary.
int fs_list(app_ctx_t *c, char *buf, size_t max);

bool fs_name_ok(const char *name);

// The whole filesystem's size and use, in bytes - for a settings screen. Asks
// core 0, which is the only side that can.
int  fs_usage(app_ctx_t *c, uint32_t *total, uint32_t *used);

// This boot's number, as fs_boot_count() recorded it. Readable anywhere.
uint32_t fs_boot_number(void);

// --- system side: core 0 ------------------------------------------------------------
bool     fs_init(void);                      // mount, formatting on first boot
bool     fs_mounted(void);
void     fs_info(size_t *total, size_t *used);
uint32_t fs_boot_count(void);                // this boot's number, kept in /fs/sys
void     fs_print_tree(void);                // every app's files, to the log
