/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "kvm_bridge.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bridge";

/*
 * Room for every bridge driver this firmware could hold.
 *
 * Four is generous for something that grows by one chip every year or two, and
 * a fixed array means registration cannot fail for want of memory at a point in
 * start-up where nothing could be done about it. Registration happens in
 * constructors, before any task runs, so no lock is needed here.
 */
#define KVM_BRIDGE_MAX_DRIVERS 4

typedef struct {
    const char *name;
    kvm_bridge_detect_fn detect;
} driver_t;

static driver_t s_drivers[KVM_BRIDGE_MAX_DRIVERS];
static size_t s_count;

esp_err_t kvm_bridge_register(const char *name, kvm_bridge_detect_fn fn)
{
    if (!name || !fn) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= KVM_BRIDGE_MAX_DRIVERS) {
        /* Cannot log usefully this early, and dropping a driver silently is the
         * worst outcome, so leave the count for the start-up line to report. */
        return ESP_ERR_NO_MEM;
    }
    s_drivers[s_count].name = name;
    s_drivers[s_count].detect = fn;
    s_count++;
    return ESP_OK;
}

size_t kvm_bridge_driver_count(void)
{
    return s_count;
}

/*
 * Who else is on the bus, when no driver recognised anything.
 *
 * "Nothing answered" and "something answered, and this firmware does not know
 * it" look the same from outside, and they send you to different places: a
 * ribbon in the first case, a missing driver in the second.
 */
int kvm_bridge_scan(i2c_master_bus_handle_t bus, char *out, size_t out_len)
{
    int count = 0;
    size_t len = 0;
    if (out && out_len) {
        out[0] = '\0';
    }
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 50) != ESP_OK) {
            continue;
        }
        count++;
        if (out && len + 6 < out_len) {
            len += (size_t)snprintf(out + len, out_len - len, " 0x%02x", addr);
        }
    }
    return count;
}

static void log_bus_scan(i2c_master_bus_handle_t bus, const char *when)
{
    char found[96];
    if (kvm_bridge_scan(bus, found, sizeof(found)) == 0) {
        ESP_LOGE(TAG, "%s: nothing at all answers on the capture I2C bus - is the ribbon to "
                      "the capture board seated?",
                 when);
    } else {
        ESP_LOGE(TAG, "%s: on the bus, unrecognised:%s", when, found);
    }
}

esp_err_t kvm_bridge_detect(i2c_master_bus_handle_t bus, kvm_bridge_t *out)
{
    if (!bus || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    if (s_count == 0) {
        /* Every driver was linked out. That is a build problem - the -u line
         * missing from a driver's CMakeLists - not a cable problem, so say so
         * rather than sending someone to look at their ribbon. */
        ESP_LOGE(TAG, "no bridge drivers registered - none were linked in");
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Ask more than once. A bridge that runs its own firmware can be seconds
     * behind its reset line before it answers I2C at all, and a single probe
     * taken too early reads exactly like an empty bus.
     */
    for (int waited_s = 0; waited_s <= 2; waited_s++) {
        if (waited_s > 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        for (size_t i = 0; i < s_count; i++) {
            esp_err_t err = s_drivers[i].detect(bus, out);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "%s found", out->name ? out->name : s_drivers[i].name);
                return ESP_OK;
            }
            if (err != ESP_ERR_NOT_FOUND) {
                /* It answered and then went wrong, which is worth knowing: a
                 * chip that is present but unhappy reads as an absent one
                 * otherwise. */
                ESP_LOGW(TAG, "%s: %s", s_drivers[i].name, esp_err_to_name(err));
            }
        }
        char when[32];
        snprintf(when, sizeof(when), "%d s after reset", waited_s);
        log_bus_scan(bus, when);
    }
    return ESP_ERR_NOT_FOUND;
}

bool kvm_bridge_timings_valid(const kvm_bridge_timings_t *t)
{
    if (!t || !t->tmds || !t->sync) {
        return false;
    }
    /* Guard against half-latched counters while the source retrains: a mode is
     * only believable if the active area fits inside the total. */
    return t->hact >= 320u && t->vact >= 200u && t->htotal > t->hact && t->vtotal > t->vact;
}
