/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared between the web component's own files.
 */
#pragma once

#include <stddef.h>

#include "esp_http_server.h"

/** {"error": message} with @p status, e.g. "409 Conflict". */
esp_err_t send_json_error(httpd_req_t *req, const char *status, const char *message);

/** Recording, screenshots, and the files they leave on the card. See record_api.c. */
const httpd_uri_t *record_api_routes(size_t *count);
const httpd_uri_t *cec_api_routes(size_t *count);
/** Set the clock from the browser's time (Unix seconds) if it was never set. */
void kvm_web_clock_from_browser(long long t);
/** Restart a moment after the reply has gone out, from a timer. */
void kvm_web_restart_soon(uint32_t delay_ms);

