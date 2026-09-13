/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Push notifications: when something happens the operator would want to know
 * about while nobody is watching - a watched phrase on the screen, the screen
 * going blank - send a message to Telegram (with a screenshot when the codec
 * is MJPEG) and/or a webhook. All the work is on a low-priority background
 * task; nothing here runs in the video path.
 */
#pragma once

#include "esp_err.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start the worker and the event poll, and register the capability. Once. */
void kvm_notify_init(void);

/**
 * Queue a notification. @p want_photo asks for a screenshot; it is attached
 * only if the setting allows it and a JPEG frame is available (MJPEG codec).
 * Safe to call from any task; it copies and returns at once.
 */
void kvm_notify_send(const char *title, const char *body, bool want_photo);

typedef struct {
    bool enabled;
    char last_result[96]; /* "ok", or why the last send failed */
    char last_at[24];     /* device local time of the last attempt, or "" */
} kvm_notify_status_t;

void kvm_notify_status(kvm_notify_status_t *out);

#ifdef __cplusplus
}
#endif
