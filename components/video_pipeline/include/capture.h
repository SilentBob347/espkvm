/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Live state of the capture path, for the REST API and the status bar. */
typedef struct {
    bool signal;           /**< HDMI is locked and delivering pixels */
    uint32_t hres;         /**< active mode, 0 until the first lock */
    uint32_t vres;
    bool interlaced;
    uint32_t fps_x100;     /**< encoded frames per second, hundredths */
    uint32_t kbps;         /**< encoded bitrate, kbit/s */
    uint32_t mode_changes; /**< resolution switches handled since boot */
    uint32_t skipped_fps_x100; /**< frames dropped as unchanged, per second */
    uint32_t encode_us;        /**< mean time the encoder alone took per frame */
    uint32_t ppa_us;           /**< mean PPA colour-conversion time per frame (H.264) */
    uint32_t encoder_busy_pct; /**< share of wall clock spent in conversion + encode */
    uint8_t sys_status;    /**< raw TC358743 SYS_STATUS, for diagnostics */
    uint8_t input_hz;      /**< refresh rate the source sends, 0 when unknown */
    bool too_fast;         /**< the mode needs more than the CSI lanes carry */
    /**
     * How long the picture has been one flat colour, in ms; 0 when it is not.
     *
     * The text reader covers screens drawn from a character generator. This
     * covers the other kind of bad news - a Windows stop screen, a blanked
     * output, a desktop that died into its background - which has no grid to
     * read and is nearly all one colour. See capture_flat.c.
     */
    uint32_t flat_ms;
    /** The flat colour is black or nearly: a blanked output, not a stop screen. */
    bool flat_dark;
} kvm_video_status_t;

/**
 * Something else is holding PSRAM it could give back - the recorder's ring of
 * frames is the one that matters - and a codec has just failed to get a buffer.
 * The callback frees what it can; the codec then tries once more.
 */
typedef void (*capture_memory_pressure_cb_t)(void);
void capture_set_memory_pressure_cb(capture_memory_pressure_cb_t cb);

/**
 * Probe the codecs and build the H.264 encoder while internal RAM is still in
 * one piece. Call early in boot, before the network starts. capture_start()
 * does it itself when this was not called.
 */
void capture_reserve_early(void);

void capture_start(void);

void capture_status_get(kvm_video_status_t *out);

/**
 * The screen as a JPEG, on either codec. On success @p out is a PSRAM buffer the
 * caller frees. ESP_ERR_NOT_FOUND when there is no signal, ESP_ERR_TIMEOUT when
 * no frame came in @p timeout_ms (a device too hot to encode sends none).
 */
esp_err_t capture_snapshot_jpeg(uint8_t **out, size_t *out_len, uint32_t timeout_ms);

/**
 * The I2C master bus the capture bridge lives on (I2C_NUM_0, the board's
 * TC358743 SDA/SCL). Shared so an optional status OLED wired to the same two
 * lines can be probed and driven without any dedicated pins. NULL before the
 * capture hardware has been brought up.
 */
/**
 * Create the I2C bus the capture chip and the status OLED share, if it does not
 * exist yet. Safe to call more than once; call it early if anything other than
 * the capture path needs the bus before capture starts.
 */
esp_err_t capture_i2c_bus_init(void);

i2c_master_bus_handle_t capture_i2c_bus(void);

#ifdef __cplusplus
}
#endif
