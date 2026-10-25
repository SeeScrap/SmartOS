#include "os/radio.h"

#include <string.h>

#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "board/log.h"
#include "os/syslog.h"

// Which channels the driver may use is a matter of law, not of radio: the
// country code picks the plan and the driver enforces it. IDF's default is
// "01", the world-safe set, which stops at channel 11 - so two channels this
// board is allowed to use at home would never be scanned. It is a setting
// rather than a constant because a board can travel: "wifi country JP" would
// add channel 14, "wifi country US" would take 12 and 13 away.
static char   s_country[4] = "TH";        // 1..13 here

static bool   s_up;
static size_t s_ram_cost;
static bool   s_netif_done;

static const uint32_t CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

void radio_chip(radio_chip_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));

    esp_chip_info_t info;
    esp_chip_info(&info);
    switch (info.model) {
        case CHIP_ESP32:   strcpy(out->model, "ESP32");    break;
        case CHIP_ESP32S2: strcpy(out->model, "ESP32-S2"); break;
        case CHIP_ESP32S3: strcpy(out->model, "ESP32-S3"); break;
        case CHIP_ESP32C3: strcpy(out->model, "ESP32-C3"); break;
        default:           strcpy(out->model, "unknown");  break;
    }
    out->cores          = info.cores;
    out->revision_major = (uint8_t)(info.revision / 100);
    out->revision_minor = (uint8_t)(info.revision % 100);
    out->wifi_bgn       = (info.features & CHIP_FEATURE_WIFI_BGN) != 0;
    out->ble            = (info.features & CHIP_FEATURE_BLE) != 0;
    out->bt_classic     = (info.features & CHIP_FEATURE_BT) != 0;
    out->ieee802154     = (info.features & CHIP_FEATURE_IEEE802154) != 0;

    // There is no 5 GHz feature bit to read on this family: the flag stays
    // false, and says so because the chip has nothing to say otherwise.
    out->wifi_5g = false;

    out->mac_ok = esp_read_mac(out->mac_sta, ESP_MAC_WIFI_STA) == ESP_OK &&
                  esp_read_mac(out->mac_ap,  ESP_MAC_WIFI_SOFTAP) == ESP_OK &&
                  esp_read_mac(out->mac_bt,  ESP_MAC_BT) == ESP_OK;
}

bool radio_up(void)
{
    if (s_up) return true;

    const size_t before = heap_caps_get_free_size(CAPS);

    // Wi-Fi keeps its calibration data in NVS. The partition has been in the
    // table since stage 4.4; this is the first thing to use it.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) { logf("[radio] nvs: %s\n", esp_err_to_name(err)); return false; }

    if (!s_netif_done) {                     // once per boot, never undone
        if ((err = esp_netif_init()) != ESP_OK) {
            logf("[radio] netif: %s\n", esp_err_to_name(err));
            return false;
        }
        if ((err = esp_event_loop_create_default()) != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            logf("[radio] event loop: %s\n", esp_err_to_name(err));
            return false;
        }
        esp_netif_create_default_wifi_sta();
        s_netif_done = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if ((err = esp_wifi_init(&cfg)) != ESP_OK) {
        logf("[radio] wifi init: %s\n", esp_err_to_name(err));
        return false;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // nothing to remember yet
    if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (err = esp_wifi_start()) != ESP_OK) {
        logf("[radio] wifi start: %s\n", esp_err_to_name(err));
        esp_wifi_deinit();
        return false;
    }

    esp_wifi_set_country_code(s_country, true);

    s_up       = true;
    s_ram_cost = before - heap_caps_get_free_size(CAPS);
    slog("radio", "wifi up, %u bytes of internal RAM", (unsigned)s_ram_cost);
    return true;
}

void radio_down(void)
{
    if (!s_up) return;
    esp_wifi_stop();
    esp_wifi_deinit();
    s_up = false;
    slog("radio", "wifi down");
}

bool   radio_is_up(void)   { return s_up; }
size_t radio_ram_cost(void) { return s_ram_cost; }

bool radio_set_country(const char *cc)
{
    if (!cc || !cc[0] || !cc[1]) return false;
    s_country[0] = cc[0];
    s_country[1] = cc[1];
    s_country[2] = 0;
    if (!s_up) return true;                       // it will be used at the next start
    return esp_wifi_set_country_code(s_country, true) == ESP_OK;
}

bool radio_channels(char cc[4], uint8_t *first, uint8_t *count)
{
    if (!s_up) return false;
    wifi_country_t c;
    if (esp_wifi_get_country(&c) != ESP_OK) return false;
    if (cc) { memcpy(cc, c.cc, 3); cc[3] = 0; }
    if (first) *first = c.schan;
    if (count) *count = c.nchan;
    return true;
}

// 2.4 GHz: channel 1 is 2412 MHz and they are 5 MHz apart, except channel 14,
// which sits 12 MHz above 13 and is Japan-only.
uint32_t radio_channel_mhz(uint8_t ch)
{
    if (ch < 1 || ch > 14) return 0;
    return (ch == 14) ? 2484u : 2412u + (uint32_t)(ch - 1) * 5u;
}

int radio_tx_power_dbm10(void)
{
    int8_t p = 0;                       // the driver counts in quarter dBm
    if (!s_up || esp_wifi_get_max_tx_power(&p) != ESP_OK) return -1;
    return (int)p * 10 / 4;
}

int radio_scan(radio_ap_t *out, int max, uint32_t *ms)
{
    if (!s_up) return -1;

    wifi_scan_config_t cfg = {};
    cfg.show_hidden = true;             // an access point with no name is still a signal

    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_wifi_scan_start(&cfg, true);      // blocks until done
    if (ms) *ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err != ESP_OK) {
        logf("[radio] scan: %s\n", esp_err_to_name(err));
        return -1;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0 || !out || max <= 0) { esp_wifi_clear_ap_list(); return (int)found; }

    uint16_t want = (uint16_t)(found < (uint16_t)max ? found : (uint16_t)max);
    wifi_ap_record_t *recs = (wifi_ap_record_t *)heap_caps_malloc(
        (size_t)want * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM);
    if (!recs) { esp_wifi_clear_ap_list(); return -1; }

    if (esp_wifi_scan_get_ap_records(&want, recs) != ESP_OK) {
        heap_caps_free(recs);
        return -1;
    }
    for (int i = 0; i < want; ++i) {
        memcpy(out[i].ssid, recs[i].ssid, 32);
        out[i].ssid[32] = 0;
        memcpy(out[i].bssid, recs[i].bssid, 6);
        out[i].channel = recs[i].primary;
        out[i].rssi    = recs[i].rssi;
        out[i].auth    = (uint8_t)recs[i].authmode;
    }
    heap_caps_free(recs);
    return (int)want;
}

const char *radio_auth_name(uint8_t auth)
{
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
        case WIFI_AUTH_ENTERPRISE:      return "enterprise";
        default:                        return "?";
    }
}
