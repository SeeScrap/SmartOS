#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "smartos_config.h"

#include "os/app.h"

// ---------------------------------------------------------------------------
// Stage 4.5 - settings.
//
// SYSTEM SETTINGS are named integers with a default and a range - backlight
// 5..100, say - kept in /fs/sys/settings as plain "key=value" lines, so the
// file can be read by eye. The table lives in DRAM: settings_get() is a lookup
// and a load, safe from either core, no mailbox.
//
// Changing one goes through core 0, which owns both the file and whatever the
// setting controls. A change is applied at once - its hook runs, and
// SYS_SETTING_CHANGED is published on the bus - but written to flash only once
// the value has been left alone for SETTINGS_SAVE_MS. Dragging the brightness
// from 100 to 10 is one flash write, not ten.
//
// APP SETTINGS belong to the app, in its own folder like its other files:
// app_setting_get / app_setting_set read and replace the file "settings"
// there. Each set is a flash write, so an app sets a value when it matters -
// when the user commits to a choice - not on every keypress.
// ---------------------------------------------------------------------------


typedef void (*settings_hook_t)(int32_t value);

// --- core 0, at boot: define every setting, then load the file ----------------------
bool settings_define(const char *key, int32_t def, int32_t min, int32_t max,
                     settings_hook_t on_change);
void settings_load(void);          // runs every hook once, with the loaded value

// --- anywhere ---------------------------------------------------------------------------
int32_t settings_get(const char *key);           // 0 if there is no such setting
bool    settings_range(const char *key, int32_t *min, int32_t *max);
int         settings_count(void);
const char *settings_key_at(int i);              // null past the end

// --- changing one --------------------------------------------------------------------------
int  settings_set(const char *key, int32_t value);     // core 0, FreeRTOS context
int  settings_set_k(const char *key, int32_t value);   // kernel tasks and apps
void settings_poll(void);          // core 0's main loop: saves once things settle
uint32_t settings_saves(void);     // times the file has been written

// --- an app's own ------------------------------------------------------------------------
int32_t app_setting_get(app_ctx_t *c, const char *key, int32_t def);
int     app_setting_set(app_ctx_t *c, const char *key, int32_t value);
