/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * One low-priority task does two things: it watches for events worth a
 * notification (a watched phrase appearing, the screen going flat) and it
 * delivers the queue to Telegram and a webhook. The delivery holds a TLS
 * session and a copy of the screen JPEG for a moment; both come from PSRAM, so
 * the internal RAM the H.264 encoder needs is never touched.
 */
#include "kvm_notify.h"

#include "capture.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kvm_caps.h"
#include "kvm_log.h"
#include "kvm_settings.h"
#include "screentext_store.h"
#include "tg_chats.h"
#include "video_frame.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *TAG = "notify";

/* TLS to Telegram needs room: the handshake alone wants several KB, and the
   sender keeps its buffers off the stack besides. 6 KB overflowed on the
   first real send. */
#define TASK_STACK 12288
#define TASK_PRIO 3 /* below the video and web tasks on purpose */
#define POLL_MS 2000
#define TITLE_MAX 80
#define BODY_MAX 200
#define QUEUE_DEPTH 6
#define HTTP_TIMEOUT_MS 15000
/* A screen JPEG we are willing to attach. Bigger than this and we send text
   only - a notification is not a place for a megabyte. */
#define PHOTO_MAX (2 * 1024 * 1024) /* a 1080p screenshot at quality 90; Telegram takes 10 MB */
/* How much of the tail of the device log to attach, when asked. Telegram takes
   up to 4096 characters in one message; a webhook takes it as a field. */
#define LOG_TAIL_MAX 2048
/* Finding chats: how much of getUpdates to read, and how many chats to offer. */
#define UPDATES_MAX (256 * 1024)
#define CHATS_MAX 10
#define CHATS_JSON_MAX 3072

typedef enum {
    EV_SEND,
    EV_FIND_CHATS,
    EV_CLIP,
} ev_kind_t;

typedef struct {
    ev_kind_t kind;
    char title[TITLE_MAX];
    char body[BODY_MAX];
    bool want_photo;
    char path[112];     /* EV_CLIP: the file */
    char card_path[80]; /* EV_CLIP: how the card names it */
} event_t;

/* The Bot API takes uploads up to 50 MB; a little under, for the form around it. */
#define TG_VIDEO_MAX (49 * 1024 * 1024)
#define CLIP_CHUNK (32 * 1024)

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
static kvm_notify_status_t s_status;

/* The last "find chats" run. Guarded by s_lock. */
static const char *s_find_state = "idle";
static const char *s_find_error = "";
static char s_find_bot[64];
static char *s_find_chats; /* JSON array, PSRAM */

void kvm_notify_send(const char *title, const char *body, bool want_photo)
{
    if (!s_queue) {
        return;
    }
    event_t ev = {.kind = EV_SEND, .want_photo = want_photo};
    strlcpy(ev.title, title ? title : "", sizeof(ev.title));
    strlcpy(ev.body, body ? body : "", sizeof(ev.body));
    /* Never block a caller (it might be the capture task): drop if the queue is
       full - a backlog of stale alerts helps no one. */
    (void)xQueueSend(s_queue, &ev, 0);
}

void kvm_notify_send_clip(const char *path, const char *card_path, const char *caption)
{
    if (!s_queue) {
        return;
    }
    event_t ev = {.kind = EV_CLIP};
    strlcpy(ev.title, "Dashcam clip", sizeof(ev.title));
    strlcpy(ev.body, caption ? caption : "", sizeof(ev.body));
    strlcpy(ev.path, path ? path : "", sizeof(ev.path));
    strlcpy(ev.card_path, card_path ? card_path : "", sizeof(ev.card_path));
    (void)xQueueSend(s_queue, &ev, 0);
}

static void set_result(const char *result)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_status.last_result, result, sizeof(s_status.last_result));
    if (now > 1600000000) {
        strftime(s_status.last_at, sizeof(s_status.last_at), "%Y-%m-%d %H:%M:%S", &t);
    }
    xSemaphoreGive(s_lock);
}

/* --- small encoders ------------------------------------------------------- */

/* Percent-encode @p src for a URL query value. */
static void url_encode(const char *src, char *dst, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 4 < cap; p++) {
        const unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0xf];
        }
    }
    dst[o] = '\0';
}

/* JSON-escape @p src into @p dst (no surrounding quotes). */
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 7 < cap; p++) {
        const unsigned char c = *p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c < 0x20) {
            o += (size_t)snprintf(dst + o, cap - o, "\\u%04x", c);
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* --- a screen JPEG, copied out of the frame store into PSRAM -------------- */

static uint8_t *grab_photo(size_t *out_len)
{
    /* Either codec: on H.264 the device makes the JPEG from the held frame. */
    uint8_t *jpeg = NULL;
    if (capture_snapshot_jpeg(&jpeg, out_len, 3000) != ESP_OK) {
        *out_len = 0;
        return NULL;
    }
    if (*out_len > PHOTO_MAX) {
        free(jpeg);
        *out_len = 0;
        return NULL;
    }
    return jpeg;
}

/* --- Telegram ------------------------------------------------------------- */

/* sendMessage: text only, form-urlencoded. Returns true on a 2xx. */
static bool tg_message(const char *token, const char *chat, const char *text)
{
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
    /* The text can be a 2 KB log tail, tripled by the encoding: heap, not stack. */
    const size_t enc_cap = (LOG_TAIL_MAX + TITLE_MAX + BODY_MAX) * 3 + 64;
    const size_t body_cap = enc_cap + 128;
    char *enc = heap_caps_malloc(enc_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *body = heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!enc || !body) {
        free(enc);
        free(body);
        return false;
    }
    url_encode(text, enc, enc_cap);
    const int n = snprintf(body, body_cap, "chat_id=%s&disable_web_page_preview=true&text=%s",
                           chat, enc);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(enc);
        free(body);
        return false;
    }
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, body, n);
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(enc);
    free(body);
    return err == ESP_OK && status >= 200 && status < 300;
}

/* sendPhoto: multipart/form-data with the JPEG, caption is the text. Streamed
   so the whole request never sits in one buffer. Returns true on a 2xx. */
static bool tg_photo(const char *token, const char *chat, const char *text, const uint8_t *jpeg,
                     size_t jpeg_len)
{
    static const char *const boundary = "espkvmXXbnd7391";
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendPhoto", token);

    char pre[BODY_MAX + 512];
    int pn = 0;
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
                   boundary, chat);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n",
                   boundary, text);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"photo\"; "
                   "filename=\"screen.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n",
                   boundary);
    char post[64];
    const int pon = snprintf(post, sizeof(post), "\r\n--%s--\r\n", boundary);
    const int total = pn + (int)jpeg_len + pon;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    char ctype[80];
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
    esp_http_client_set_header(c, "Content-Type", ctype);

    bool ok = false;
    if (esp_http_client_open(c, total) == ESP_OK) {
        if (esp_http_client_write(c, pre, pn) == pn &&
            esp_http_client_write(c, (const char *)jpeg, (int)jpeg_len) == (int)jpeg_len &&
            esp_http_client_write(c, post, pon) == pon) {
            esp_http_client_fetch_headers(c);
            const int status = esp_http_client_get_status_code(c);
            ok = status >= 200 && status < 300;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

/*
 * sendVideo, streamed from the card: the file never has to fit in memory. With
 * supports_streaming the chat plays it at once, which the MP4's index at the
 * front of the file allows.
 */
static bool tg_video(const char *token, const char *chat, const char *text, const char *path,
                     size_t size)
{
    FILE *f = fopen(path, "rb");
    char *chunk = heap_caps_malloc(CLIP_CHUNK, MALLOC_CAP_SPIRAM);
    if (!f || !chunk) {
        if (f) {
            fclose(f);
        }
        free(chunk);
        return false;
    }
    static const char *const boundary = "espkvmXXbnd7391";
    char url[128];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendVideo", token);
    char pre[BODY_MAX + 640];
    int pn = 0;
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n",
                   boundary, chat);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"caption\"\r\n\r\n%s\r\n",
                   boundary, text);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"supports_streaming\"\r\n\r\n"
                   "true\r\n",
                   boundary);
    pn += snprintf(pre + pn, sizeof(pre) - pn,
                   "--%s\r\nContent-Disposition: form-data; name=\"video\"; "
                   "filename=\"clip.mp4\"\r\nContent-Type: video/mp4\r\n\r\n",
                   boundary);
    char post[64];
    const int pon = snprintf(post, sizeof(post), "\r\n--%s--\r\n", boundary);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        char ctype[80];
        snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=%s", boundary);
        esp_http_client_set_header(c, "Content-Type", ctype);
        if (esp_http_client_open(c, pn + (int)size + pon) == ESP_OK &&
            esp_http_client_write(c, pre, pn) == pn) {
            size_t sent = 0;
            size_t n;
            bool w = true;
            while (w && (n = fread(chunk, 1, CLIP_CHUNK, f)) > 0) {
                w = esp_http_client_write(c, chunk, (int)n) == (int)n;
                sent += n;
            }
            if (w && sent == size && esp_http_client_write(c, post, pon) == pon) {
                esp_http_client_fetch_headers(c);
                const int status = esp_http_client_get_status_code(c);
                ok = status >= 200 && status < 300;
                if (!ok) {
                    ESP_LOGW(TAG, "telegram video: HTTP %d", status);
                }
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    fclose(f);
    free(chunk);
    return ok;
}

/* --- a generic webhook ---------------------------------------------------- */

static bool webhook(const char *url, const char *title, const char *body, const char *logtail)
{
    char jt[TITLE_MAX * 2];
    char jb[BODY_MAX * 2];
    json_escape(title, jt, sizeof(jt));
    json_escape(body, jb, sizeof(jb));
    char jh[64];
    const char *host = kvm_setting_str("net_hostname");
    json_escape(host[0] ? host : "espkvm", jh, sizeof(jh));

    /* The body may carry a log tail, so the buffer is sized and heap-held. */
    const size_t cap = TITLE_MAX * 2 + BODY_MAX * 2 + 256 + (logtail ? LOG_TAIL_MAX * 2 : 0);
    char *json = malloc(cap);
    if (!json) {
        return false;
    }
    int n = snprintf(json, cap, "{\"title\":\"%s\",\"message\":\"%s\",\"device\":\"%s\"",
                     jt, jb, jh);
    if (logtail && logtail[0]) {
        char *jl = malloc(LOG_TAIL_MAX * 2);
        if (jl) {
            json_escape(logtail, jl, LOG_TAIL_MAX * 2);
            n += snprintf(json + n, cap - n, ",\"log\":\"%s\"", jl);
            free(jl);
        }
    }
    n += snprintf(json + n, cap - n, "}");

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach, /* harmless for plain HTTP */
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        free(json);
        return false;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, n);
    const esp_err_t err = esp_http_client_perform(c);
    const int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(json);
    return err == ESP_OK && status >= 200 && status < 300;
}

/* --- delivery ------------------------------------------------------------- */

/* A bot token is digits, a colon, then letters, digits, '_' and '-'. Anything
   else (a pasted sentence, a stray space) would only fail inside the URL
   parser with a message that names nothing. */
static bool token_plausible(const char *tok)
{
    for (const char *p = tok; *p; p++) {
        const char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              c == ':' || c == '_' || c == '-')) {
            return false;
        }
    }
    return strchr(tok, ':') != NULL;
}

/* --- finding chats -------------------------------------------------------- */

/* GET a Bot API method into @p buf (NUL-terminated). Returns the HTTP status,
   or -1 when nothing came back. A reply longer than @p cap is cut. */
static int tg_get(const char *token, const char *method, char *buf, size_t cap)
{
    char url[192];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", token, method);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    int status = -1;
    size_t len = 0;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        status = esp_http_client_get_status_code(c);
        while (len + 1 < cap) {
            const int r = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
            if (r <= 0) {
                break;
            }
            len += (size_t)r;
        }
    }
    buf[len] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static void find_done(const char *state, const char *error)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_find_state = state;
    s_find_error = error;
    xSemaphoreGive(s_lock);
}

/* getMe checks the token and names the bot; getUpdates lists who wrote to it.
   No offset is sent, so nothing is marked as read. */
static void find_chats(void)
{
    const char *token = kvm_setting_str("notify_tg_token");
    if (!token[0]) {
        find_done("error", "set the bot token first");
        return;
    }
    if (!token_plausible(token)) {
        find_done("error", "the Telegram token is not a bot token - paste it again");
        return;
    }
    char *buf = heap_caps_malloc(UPDATES_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *list = heap_caps_malloc(CHATS_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf || !list) {
        free(buf);
        free(list);
        find_done("error", "out of memory");
        return;
    }

    char bot[sizeof(s_find_bot)] = "";
    int status = tg_get(token, "getMe", buf, 4096);
    if (status == 401 || status == 404) {
        find_done("error", "Telegram did not accept the bot token");
    } else if (status != 200 || !tg_bot_username(buf, bot, sizeof(bot))) {
        find_done("error", "could not reach Telegram");
    } else {
        status = tg_get(token, "getUpdates?limit=100", buf, UPDATES_MAX);
        const int n = status == 200 ? tg_chats_from_updates(buf, list, CHATS_JSON_MAX, CHATS_MAX) : -1;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_find_bot, bot, sizeof(s_find_bot));
        if (n >= 0) {
            free(s_find_chats);
            s_find_chats = list;
            list = NULL;
        }
        xSemaphoreGive(s_lock);
        if (status == 409) {
            find_done("error", "the bot has a webhook set, so it cannot list its chats");
        } else if (n < 0) {
            find_done("error", "could not read the chats from Telegram");
        } else {
            ESP_LOGI(TAG, "telegram: found %d chat(s)", n);
            find_done("ok", "");
        }
    }
    free(buf);
    free(list);
}

esp_err_t kvm_notify_find_chats(void)
{
    if (!s_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = strcmp(s_find_state, "running") == 0;
    if (!busy) {
        s_find_state = "running";
        s_find_error = "";
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        return ESP_OK;
    }
    const event_t ev = {.kind = EV_FIND_CHATS};
    if (xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        find_done("error", "the device is busy sending - try again");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

size_t kvm_notify_chats_json(char *out, size_t cap)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int n = snprintf(out, cap, "{\"state\":\"%s\",\"error\":\"%s\",\"bot\":\"%s\",\"chats\":%s}",
                           s_find_state, s_find_error, s_find_bot,
                           s_find_chats ? s_find_chats : "[]");
    xSemaphoreGive(s_lock);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

static void deliver(const event_t *ev)
{
    char text[TITLE_MAX + BODY_MAX + 4];
    if (ev->body[0]) {
        snprintf(text, sizeof(text), "%s\n\n%s", ev->title, ev->body);
    } else {
        snprintf(text, sizeof(text), "%s", ev->title);
    }

    const char *token = kvm_setting_str("notify_tg_token");
    const char *chat = kvm_setting_str("notify_tg_chat");
    const char *url = kvm_setting_str("notify_url");
    bool any = false;
    bool ok = true;

    uint8_t *photo = NULL;
    size_t photo_len = 0;
    if (ev->want_photo && kvm_setting_bool("notify_snap")) {
        photo = grab_photo(&photo_len);
    }

    /* The tail of the device log, so an alert carries the context that explains
       it - which is why the noisy tags are held down, or this would be filler. */
    char *logtail = NULL;
    if (kvm_setting_bool("notify_log")) {
        logtail = malloc(LOG_TAIL_MAX);
        if (logtail) {
            kvm_log_read(logtail, LOG_TAIL_MAX); /* a small buffer keeps the newest end */
        }
    }

    const bool token_bad = token[0] && !token_plausible(token);
    if (token_bad) {
        any = true;
        ESP_LOGE(TAG, "telegram: the bot token has characters a token cannot have - paste it again");
    }
    if (token[0] && chat[0] && !token_bad) {
        any = true;
        const bool sent = (photo && photo_len)
                              ? tg_photo(token, chat, text, photo, photo_len)
                              : tg_message(token, chat, text);
        if (!sent) {
            /* A photo request can fail on its own (size, a flaky moment); fall
               back to the text so the alert still gets through. */
            ok = (photo && photo_len) ? tg_message(token, chat, text) : false;
        }
        /* The log goes as its own message: a photo caption is capped at 1024
           characters and a tail does not fit there. */
        if (logtail && logtail[0]) {
            char *msg = heap_caps_malloc(LOG_TAIL_MAX + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (msg) {
                snprintf(msg, LOG_TAIL_MAX + 16, "log:\n%s", logtail);
                (void)tg_message(token, chat, msg);
                free(msg);
            }
        }
        ESP_LOGI(TAG, "telegram: %s%s", ok ? "sent" : "failed",
                 photo && photo_len ? " (with photo)" : "");
    }
    if (url[0]) {
        any = true;
        const bool sent = webhook(url, ev->title, ev->body, logtail);
        ok = ok && sent;
        ESP_LOGI(TAG, "webhook: %s", sent ? "sent" : "failed");
    }

    free(photo);
    free(logtail);

    if (!any) {
        set_result("no channel configured (set a Telegram bot or a webhook URL)");
    } else if (token_bad) {
        set_result("the Telegram token is not a bot token - paste it again");
    } else {
        set_result(ok ? "ok" : "the last send failed - check the token, chat id or URL");
    }
}

static void deliver_clip(const event_t *ev)
{
    if (!kvm_setting_bool("notify_clip")) {
        return;
    }
    const char *token = kvm_setting_str("notify_tg_token");
    const char *chat = kvm_setting_str("notify_tg_chat");
    const char *url = kvm_setting_str("notify_url");
    struct stat st;
    const bool exists = stat(ev->path, &st) == 0;
    char text[BODY_MAX + 200];
    bool any = false, ok = true;

    if (token[0] && chat[0] && token_plausible(token) && exists) {
        any = true;
        if ((size_t)st.st_size <= TG_VIDEO_MAX) {
            snprintf(text, sizeof(text), "%s", ev->body);
            ok = tg_video(token, chat, text, ev->path, (size_t)st.st_size);
            ESP_LOGI(TAG, "telegram clip: %s (%u KB)", ok ? "sent" : "failed",
                     (unsigned)(st.st_size / 1024));
        } else {
            snprintf(text, sizeof(text), "%.160s\nSaved on the card as %.80s (%u MB, too big for Telegram).",
                     ev->body, ev->card_path, (unsigned)(st.st_size / (1024 * 1024)));
            ok = tg_message(token, chat, text);
        }
    }
    if (url[0]) {
        any = true;
        snprintf(text, sizeof(text), "%.160s - saved on the card as %.80s", ev->body, ev->card_path);
        ok = webhook(url, ev->title, text, NULL) && ok;
    }
    if (any) {
        set_result(ok ? "ok" : "the last clip did not go out - check the token, chat id or URL");
    }
}

/* --- the event poll ------------------------------------------------------- */

static void poll_events(void)
{
    /* A watched phrase appearing. The store gives an edge; we act on the rise. */
    static uint32_t s_alert_seen;
    uint32_t seq = 0;
    char phrase[SCREENTEXT_ALERT_MAX];
    const bool alerting = screentext_alert_get(phrase, sizeof(phrase), &seq);
    if (seq != s_alert_seen) {
        s_alert_seen = seq;
        if (alerting && kvm_setting_bool("notify_watch")) {
            char body[BODY_MAX];
            snprintf(body, sizeof(body), "On the screen: %.180s", phrase);
            kvm_notify_send("Screen alert", body, true);
        }
    }

    /* The screen going flat - a stop screen, a blanked output - for the same
       thirty seconds Home Assistant treats as a state, not a repaint. */
    static bool s_flat_seen;
    kvm_video_status_t v;
    capture_status_get(&v);
    const bool flat = v.signal && v.flat_ms >= 30000u;
    if (flat && !s_flat_seen && kvm_setting_bool("notify_flat")) {
        kvm_notify_send("Screen went blank", "The output has been one flat colour for a while.",
                        true);
    }
    s_flat_seen = flat;
}

static void task(void *arg)
{
    (void)arg;
    for (;;) {
        event_t ev;
        /* Wake for a queued event, or every POLL_MS to look for one. */
        bool got = xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(POLL_MS)) == pdTRUE;
        if (got && ev.kind == EV_FIND_CHATS) {
            find_chats(); /* setting up, so it works with sending switched off */
            got = false;
        }
        const bool on = kvm_setting_bool("notify_enable");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status.enabled = on;
        xSemaphoreGive(s_lock);
        if (!on) {
            continue; /* a queued event while disabled is simply dropped */
        }
        if (got && ev.kind == EV_CLIP) {
            deliver_clip(&ev);
        } else if (got) {
            deliver(&ev);
        }
        poll_events();
    }
}

void kvm_notify_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(event_t));
    strlcpy(s_status.last_result, "nothing sent yet", sizeof(s_status.last_result));
    kvm_cap_report(KVM_CAP_NOTIFY, true, NULL);
    if (xTaskCreate(task, "notify", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the notify task");
    }
}

void kvm_notify_status(kvm_notify_status_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}
