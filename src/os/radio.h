#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Stage 6.3 - the radio.
//
// The ESP32-S3 has one radio and it is 2.4 GHz only: Wi-Fi 802.11 b/g/n and
// Bluetooth LE 5, sharing the same antenna. There is no 5 GHz band on this
// chip, no sub-GHz, and no 802.15.4 - a board that needs those needs another
// chip beside this one. radio_chip() reads what the silicon itself says rather
// than repeating a datasheet.
//
// WHAT THE ANTENNA CONNECTOR MEANS
//
// The module decides. ESP32-S3-WROOM-1 has a printed antenna on the board and
// no connector; the -1U has the U.FL connector and NO printed antenna, so
// without something screwed onto it the radio is working into nearly nothing.
// That is exactly what the scan below can show: run it with the antenna off
// and again with it on, and compare the numbers for the same access points.
//
// THE RADIO IS NOT STARTED AT BOOT
//
// It costs internal RAM and it is not needed to run a launcher, so it comes up
// when someone asks for it - the shell's "wifi up", and in stage 6.4 an app.
// radio_ram_cost() is what it took, measured rather than guessed.
//
// CORE 0 ONLY
//
// Everything here is ESP-IDF, so it is core 0's. Core 1 reaches it the way it
// reaches the filesystem: through the mailbox (stage 6.4).
//
// ESP32-S3 มีวิทยุตัวเดียวและเป็น 2.4 GHz เท่านั้น: Wi-Fi 802.11 b/g/n กับ
// Bluetooth LE 5 ใช้เสาอากาศร่วมกัน ชิปนี้ไม่มีย่าน 5 GHz ไม่มี sub-GHz และ
// ไม่มี 802.15.4 บอร์ดที่ต้องการสิ่งเหล่านั้นต้องมีชิปอีกตัววางข้างกัน
//
// โมดูลเป็นตัวตัดสินเรื่องเสาอากาศ: WROOM-1 มีเสาพิมพ์บนแผ่นและไม่มีขั้วต่อ
// ส่วน -1U มีขั้ว U.FL และ **ไม่มี** เสาพิมพ์ ถ้าไม่ขันอะไรเข้าไปก็เท่ากับส่งออก
// สู่ความว่างเปล่า และวิทยุไม่ถูกเปิดตอนบูต เพราะมันกิน RAM ภายในและ
// ไม่จำเป็นต่อการรัน launcher
// ---------------------------------------------------------------------------

typedef struct {
    char     model[20];
    uint8_t  revision_major, revision_minor;
    uint8_t  cores;
    bool     wifi_bgn;        // 2.4 GHz 802.11 b/g/n
    bool     wifi_5g;         // the chip does not have it; read, not assumed
    bool     ble;
    bool     bt_classic;
    bool     ieee802154;
    bool     mac_ok;
    uint8_t  mac_sta[6], mac_ap[6], mac_bt[6];
} radio_chip_t;

// What the silicon reports. No radio needed, safe at boot.
void radio_chip(radio_chip_t *out);

// Brings the radio up in station mode (NVS, netif, event loop, esp_wifi) and
// leaves it up. True if it is up, whether it already was or not.
bool   radio_up(void);
void   radio_down(void);
bool   radio_is_up(void);
size_t radio_ram_cost(void);           // internal RAM it took, measured

// The country whose channel plan the driver enforces - "TH" by default, which
// is channels 1..13. Takes effect at once if the radio is up, at its next
// start if not. Which channels may be used is a matter of law, not of radio.
bool     radio_set_country(const char *cc);

// The plan in force: the country code, its first channel and how many. 2.4 GHz
// channels are 5 MHz apart from 2412 MHz, except 14, which is at 2484.
bool     radio_channels(char cc[4], uint8_t *first, uint8_t *count);
uint32_t radio_channel_mhz(uint8_t ch);
int      radio_tx_power_dbm10(void);   // tenths of a dBm, -1 if the radio is down

typedef struct {
    char    ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    int8_t  rssi;                      // dBm
    uint8_t auth;                      // wifi_auth_mode_t
} radio_ap_t;

// One scan of every channel in the plan. Returns how many access points were
// written to `out` (the driver's own list is longer if there were more), or -1
// if the radio is not up. `ms` is how long the scan took.
int radio_scan(radio_ap_t *out, int max, uint32_t *ms);

const char *radio_auth_name(uint8_t auth);
