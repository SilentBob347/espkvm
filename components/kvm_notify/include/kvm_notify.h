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
#include <stddef.h>

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

/**
 * Send a saved video: to Telegram as a video that plays in the chat (when it is
 * under the Bot API's 50 MB), to the webhook as a message naming the file.
 * @p path is the file's full path; @p card_path is what to call it, e.g.
 * "VIDEO/20260917-140322-event.mp4". Queued, like kvm_notify_send.
 */
void kvm_notify_send_clip(const char *path, const char *card_path, const char *caption);

typedef struct {
    bool enabled;
    char last_result[96]; /* "ok", or why the last send failed */
    char last_at[24];     /* device local time of the last attempt, or "" */
} kvm_notify_status_t;

void kvm_notify_status(kvm_notify_status_t *out);

/**
 * Ask Telegram, with the saved bot token, which chats have written to the bot,
 * so the chat id can be picked rather than looked up. Runs on the notify task;
 * read the result with kvm_notify_chats_json().
 */
esp_err_t kvm_notify_find_chats(void);

/**
 * The last run as JSON: {"state":"idle|running|ok|error","error":"...",
 * "bot":"username","chats":[{"id":"..","type":"..","name":".."}]}.
 * Returns the length, or 0 if @p cap is too small.
 */
size_t kvm_notify_chats_json(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
