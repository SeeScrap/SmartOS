#include "tests/tests.h"

#ifdef S3_TEST_APPS

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#include "os/radio.h"
#include "s3hal/log.h"
#include "tests/harness.h"

// ---------------------------------------------------------------------------
// Stage 6.3 - the radio, brought up and put back down.
//
// What can be claimed here does not depend on what happens to be on the air:
// that the radio starts at all, what it costs in internal RAM, that the
// channel plan it enforces is the 2.4 GHz one and nothing else, that every
// access point a scan returns is inside that plan and at the frequency the
// arithmetic says, and that stopping it gives the memory back.
//
// How well it hears is a different question, and one a test cannot settle on
// its own - it depends on the antenna, the room and the neighbours. That is
// what tools/wifi_survey.py is for.
// ---------------------------------------------------------------------------

void sys63_radio_test(void)
{
    logln();
    logln("--- stage 6.3: the radio ---");
    char d[160];
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    // 1. What the silicon says, before anything is started.
    {
        radio_chip_t c;
        radio_chip(&c);
        const bool macs_differ = memcmp(c.mac_sta, c.mac_ap, 6) != 0 &&
                                 memcmp(c.mac_sta, c.mac_bt, 6) != 0;
        snprintf(d, sizeof(d), "%s rev %u.%u: 2.4 GHz %s, 5 GHz %s, BLE %s, 802.15.4 %s",
                 c.model, c.revision_major, c.revision_minor, c.wifi_bgn ? "yes" : "no",
                 c.wifi_5g ? "yes" : "no", c.ble ? "yes" : "no", c.ieee802154 ? "yes" : "no");
        sdk_case("one radio, 2.4 GHz only", c.wifi_bgn && c.ble && !c.wifi_5g &&
                 !c.ieee802154 && c.mac_ok && macs_differ, d);
    }

    // 2. The channel arithmetic, which the scan's frequencies are read off.
    {
        const bool ok = radio_channel_mhz(1) == 2412 && radio_channel_mhz(6) == 2437 &&
                        radio_channel_mhz(13) == 2472 && radio_channel_mhz(14) == 2484 &&
                        radio_channel_mhz(0) == 0 && radio_channel_mhz(15) == 0;
        snprintf(d, sizeof(d), "ch1 %lu, ch6 %lu, ch13 %lu, ch14 %lu MHz",
                 (unsigned long)radio_channel_mhz(1), (unsigned long)radio_channel_mhz(6),
                 (unsigned long)radio_channel_mhz(13), (unsigned long)radio_channel_mhz(14));
        sdk_case("channels to frequencies", ok, d);
    }

    // 3. Up, and what it costs.
    const size_t before = heap_caps_get_free_size(caps);
    const bool up = radio_up();
    {
        snprintf(d, sizeof(d), "%s, %u bytes of internal RAM, %u KB free after",
                 up ? "up" : "DID NOT START", (unsigned)radio_ram_cost(),
                 (unsigned)(heap_caps_get_free_size(caps) / 1024));
        sdk_case("the radio starts", up && radio_is_up() && radio_ram_cost() > 10000 &&
                 radio_ram_cost() < 90000, d);
    }

    // 4. The plan it enforces: 2.4 GHz, and nothing outside 1..14.
    char cc[4] = "??";
    uint8_t first = 0, count = 0;
    if (up) {
        const bool got = radio_channels(cc, &first, &count);
        const int p10 = radio_tx_power_dbm10();
        snprintf(d, sizeof(d), "country %s, channels %u..%u (%lu..%lu MHz), max TX %d.%d dBm",
                 cc, first, first + count - 1, (unsigned long)radio_channel_mhz(first),
                 (unsigned long)radio_channel_mhz((uint8_t)(first + count - 1)),
                 p10 / 10, p10 % 10);
        sdk_case("the channel plan", got && first == 1 && count >= 11 && count <= 14 &&
                 p10 >= 50 && p10 <= 220, d);
    }

    // 5. One scan. Nothing is claimed about what is out there - only that
    //    whatever came back is inside the plan and at the right frequency.
    if (up) {
        static radio_ap_t ap[32];
        uint32_t ms = 0;
        const int n = radio_scan(ap, 32, &ms);
        int bad_ch = 0, strongest = -127;
        for (int i = 0; i < n; ++i) {
            if (ap[i].channel < first || ap[i].channel >= first + count ||
                radio_channel_mhz(ap[i].channel) == 0) bad_ch++;
            if (ap[i].rssi > strongest) strongest = ap[i].rssi;
        }
        snprintf(d, sizeof(d), "%d access point(s) in %lu ms, %d outside the plan, strongest %d dBm",
                 n, (unsigned long)ms, bad_ch, n ? strongest : 0);
        sdk_case("a scan of the band", n >= 0 && bad_ch == 0 && ms < 15000, d);
    }

    // 6. Down, and the memory comes back.
    if (up) {
        radio_down();
        pump(300);
        const size_t after = heap_caps_get_free_size(caps);
        const int lost = (int)before - (int)after;
        snprintf(d, sizeof(d), "%u KB free before, %u KB after; %d bytes not returned",
                 (unsigned)(before / 1024), (unsigned)(after / 1024), lost);
        // Some of what the first start allocates is the network interface and
        // the event loop, which are created once per boot and never undone.
        sdk_case("and gives it back", !radio_is_up() && lost < 20000, d);
    }
}

#endif // S3_TEST_APPS
