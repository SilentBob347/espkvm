/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ts_to_mp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS 188
#define PID_VIDEO 0x0100
#define TIMESCALE 90000u

/* ---- reading the transport stream --------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t len;
    size_t cap;
    int64_t pts;
} au_buf_t;

typedef struct {
    const mp4_io_t *io;
    uint8_t pkt[TS];
    au_buf_t cur;   /* the access unit being gathered */
    au_buf_t ready; /* the last one handed out */
    bool open;
    bool eof;
} ts_reader_t;

static bool au_append(au_buf_t *a, const uint8_t *p, size_t n)
{
    if (a->len + n > a->cap) {
        /* Small steps from a small start: on a board with a megabyte of PSRAM
         * left, a doubled buffer is the allocation that fails. */
        size_t cap = a->cap ? a->cap + a->cap / 2 : 64 * 1024;
        while (cap < a->len + n) {
            cap *= 2;
        }
        uint8_t *grown = realloc(a->b, cap);
        if (!grown) {
            return false;
        }
        a->b = grown;
        a->cap = cap;
    }
    memcpy(a->b + a->len, p, n);
    a->len += n;
    return true;
}

/* The gathered access unit becomes the ready one; the old ready buffer is reused. */
static void hand_out(ts_reader_t *r)
{
    const au_buf_t t = r->ready;
    r->ready = r->cur;
    r->cur = t;
    r->cur.len = 0;
}

static void reader_free(ts_reader_t *r)
{
    free(r->cur.b);
    free(r->ready.b);
}

static bool read_packet(ts_reader_t *r)
{
    size_t have = 0;
    while (have < TS) {
        const size_t n = r->io->read(r->io->io, r->pkt + have, TS - have);
        if (!n) {
            break;
        }
        have += n;
    }
    return have == TS && r->pkt[0] == 0x47;
}

/* The next access unit in r->ready. 1 = got one, 0 = end, -1 = out of memory. */
static int next_au(ts_reader_t *r)
{
    for (;;) {
        if (r->eof || !read_packet(r)) {
            r->eof = true;
            if (r->open) {
                r->open = false;
                hand_out(r);
                return 1;
            }
            return 0;
        }
        const uint8_t *p = r->pkt;
        const int pid = ((p[1] & 0x1f) << 8) | p[2];
        if (pid != PID_VIDEO || !(p[3] & 0x10)) {
            continue;
        }
        size_t at = 4;
        if (p[3] & 0x20) {
            at = 5 + p[4];
            if (at >= TS) {
                continue;
            }
        }
        if (!(p[1] & 0x40)) {
            if (r->open && !au_append(&r->cur, p + at, TS - at)) {
                return -1;
            }
            continue;
        }
        /* A PES packet starts: whatever was gathered before it is complete. */
        const uint8_t *h = p + at;
        if (TS - at < 14 || h[0] || h[1] || h[2] != 1) {
            continue;
        }
        int64_t pts = 0;
        if (h[7] & 0x80) {
            pts = ((int64_t)(h[9] & 0x0e) << 29) | ((int64_t)h[10] << 22) |
                  ((int64_t)(h[11] & 0xfe) << 14) | ((int64_t)h[12] << 7) | (h[13] >> 1);
        }
        const size_t payload = at + 9 + h[8];
        const bool had = r->open;
        if (had) {
            hand_out(r);
        }
        r->open = true;
        r->cur.pts = pts;
        if (payload < TS && !au_append(&r->cur, p + payload, TS - payload)) {
            return -1;
        }
        if (had) {
            return 1;
        }
    }
}

/* ---- H.264 --------------------------------------------------------------------- */

typedef void (*nal_fn)(void *user, const uint8_t *nal, size_t len);

/* Call @p fn for each NAL unit in an Annex-B access unit. */
static void each_nal(const uint8_t *b, size_t n, nal_fn fn, void *user)
{
    size_t i = 0, start = SIZE_MAX;
    while (i + 3 <= n) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                if (end > start && b[end - 1] == 0) {
                    end--; /* the leading zero of a four-byte start code */
                }
                fn(user, b + start, end - start);
            }
            i += 3;
            start = i;
        } else {
            i++;
        }
    }
    if (start != SIZE_MAX && start < n) {
        fn(user, b + start, n - start);
    }
}

typedef struct {
    size_t size; /* the sample, length-prefixed */
    bool idr;
    uint8_t *sps, *pps;
    size_t sps_len, pps_len;
    const mp4_io_t *io; /* writing pass only */
    bool ok;
} nal_scan_t;

static bool keep_nal(uint8_t type)
{
    /* The access unit delimiter and parameter sets do not go in the samples:
     * the parameter sets live in the sample description instead. */
    return type != 9 && type != 7 && type != 8;
}

static void scan_nal(void *user, const uint8_t *nal, size_t len)
{
    nal_scan_t *s = user;
    if (!len) {
        return;
    }
    const uint8_t type = nal[0] & 0x1f;
    if (type == 5) {
        s->idr = true;
    }
    if (type == 7 && !s->sps && len >= 4) {
        s->sps = malloc(len);
        if (s->sps) {
            memcpy(s->sps, nal, len);
            s->sps_len = len;
        }
    }
    if (type == 8 && !s->pps) {
        s->pps = malloc(len);
        if (s->pps) {
            memcpy(s->pps, nal, len);
            s->pps_len = len;
        }
    }
    if (keep_nal(type)) {
        s->size += 4 + len;
    }
}

static void write_nal(void *user, const uint8_t *nal, size_t len)
{
    nal_scan_t *s = user;
    if (!len || !s->ok || !keep_nal(nal[0] & 0x1f)) {
        return;
    }
    const uint8_t hdr[4] = {(uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len};
    s->ok = s->io->write(s->io->io, hdr, 4) && s->io->write(s->io->io, nal, len);
}

/* ---- boxes --------------------------------------------------------------------- */

typedef struct {
    uint8_t *b;
    size_t len;
    size_t cap;
    bool bad;
} buf_t;

static void put(buf_t *o, const uint8_t *p, size_t n)
{
    if (o->bad) {
        return;
    }
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n) {
            cap *= 2;
        }
        uint8_t *g = realloc(o->b, cap);
        if (!g) {
            o->bad = true;
            return;
        }
        o->b = g;
        o->cap = cap;
    }
    memcpy(o->b + o->len, p, n);
    o->len += n;
}

static void u8(buf_t *o, uint8_t v) { put(o, &v, 1); }
static void u16(buf_t *o, uint16_t v) { const uint8_t b[2] = {v >> 8, v}; put(o, b, 2); }
static void u32(buf_t *o, uint32_t v)
{
    const uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    put(o, b, 4);
}
static void u64(buf_t *o, uint64_t v) { u32(o, (uint32_t)(v >> 32)); u32(o, (uint32_t)v); }
static void zeros(buf_t *o, size_t n) { while (n--) u8(o, 0); }

/* Open a box: its size is filled in by box_end. */
static size_t box(buf_t *o, const char *type)
{
    const size_t at = o->len;
    u32(o, 0);
    put(o, (const uint8_t *)type, 4);
    return at;
}

static void box_end(buf_t *o, size_t at)
{
    if (o->bad) {
        return;
    }
    const uint32_t n = (uint32_t)(o->len - at);
    o->b[at] = n >> 24;
    o->b[at + 1] = n >> 16;
    o->b[at + 2] = n >> 8;
    o->b[at + 3] = n;
}

static void full(buf_t *o, uint8_t version, uint32_t flags)
{
    u8(o, version);
    u8(o, flags >> 16);
    u8(o, flags >> 8);
    u8(o, flags);
}

static void matrix(buf_t *o)
{
    static const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
    for (int i = 0; i < 9; i++) {
        u32(o, m[i]);
    }
}

/* ---- the conversion ------------------------------------------------------------ */

typedef struct {
    uint32_t *size;
    uint32_t *delta; /* duration of each sample, 90 kHz */
    uint8_t *sync;
    size_t count;
    size_t cap;
} samples_t;

static bool sample_add(samples_t *s, uint32_t size, bool sync)
{
    if (s->count == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096;
        uint32_t *a = realloc(s->size, cap * sizeof(uint32_t));
        if (!a) return false;
        s->size = a;
        uint32_t *d = realloc(s->delta, cap * sizeof(uint32_t));
        if (!d) return false;
        s->delta = d;
        uint8_t *y = realloc(s->sync, cap);
        if (!y) return false;
        s->sync = y;
        s->cap = cap;
    }
    s->size[s->count] = size;
    s->delta[s->count] = 0;
    s->sync[s->count] = sync;
    s->count++;
    return true;
}

bool ts_to_mp4(const mp4_io_t *io, uint32_t width, uint32_t height, const mp4_chapter_t *chapters,
               int chapter_count, char *err, size_t err_cap)
{
    samples_t sm = {0};
    nal_scan_t params = {0};
    buf_t head = {0};
    ts_reader_t rd = {.io = io};
    bool ok = false;
    uint64_t data_bytes = 0;
    int64_t first_pts = -1, prev_pts = -1;

    /* Pass one: the sample table. */
    int got;
    while ((got = next_au(&rd)) == 1) {
        const uint8_t *au = rd.ready.b;
        const size_t au_len = rd.ready.len;
        const int64_t pts = rd.ready.pts;
        nal_scan_t s = {.sps = params.sps, .pps = params.pps,
                        .sps_len = params.sps_len, .pps_len = params.pps_len};
        each_nal(au, au_len, scan_nal, &s);
        params.sps = s.sps;
        params.sps_len = s.sps_len;
        params.pps = s.pps;
        params.pps_len = s.pps_len;
        if (!s.size) {
            continue;
        }
        if (!sm.count && !s.idr) {
            continue; /* a file has to start on a picture that decodes on its own */
        }
        if (first_pts < 0) {
            first_pts = pts;
        }
        if (sm.count) {
            /* Times go back where a clip joins two segments, each counted from
             * its own start: the frame there lasts as long as the one before. */
            const int64_t d = pts - prev_pts;
            const uint32_t before = sm.count > 1 ? sm.delta[sm.count - 2] : TIMESCALE / 25;
            sm.delta[sm.count - 1] = (d > 0 && d < TIMESCALE * 10) ? (uint32_t)d : before;
        }
        prev_pts = pts;
        if (!sample_add(&sm, (uint32_t)s.size, s.idr)) {
            got = -1;
            break;
        }
        data_bytes += s.size;
    }
    if (got < 0) {
        snprintf(err, err_cap, "out of memory reading the recording");
        goto out;
    }
    if (!sm.count || !params.sps || !params.pps) {
        snprintf(err, err_cap, "the recording has no complete H.264 picture");
        goto out;
    }
    sm.delta[sm.count - 1] = sm.count > 1 ? sm.delta[sm.count - 2] : TIMESCALE / 25;
    if (data_bytes + 8 > 0xffffffffull) {
        snprintf(err, err_cap, "too long for one MP4 here (over 4 GB)");
        goto out;
    }
    uint64_t duration = 0;
    for (size_t i = 0; i < sm.count; i++) {
        duration += sm.delta[i];
    }

    /* The index, written before the samples so the file plays as it arrives. */
    size_t b = box(&head, "ftyp");
    put(&head, (const uint8_t *)"isom", 4);
    u32(&head, 0x200);
    put(&head, (const uint8_t *)"isomiso2avc1mp41", 16);
    box_end(&head, b);

    const size_t moov = box(&head, "moov");
    b = box(&head, "mvhd");
    full(&head, 0, 0);
    u32(&head, 0);
    u32(&head, 0);
    u32(&head, 1000);
    u32(&head, (uint32_t)(duration * 1000 / TIMESCALE));
    u32(&head, 0x10000);
    u16(&head, 0x100);
    zeros(&head, 10);
    matrix(&head);
    zeros(&head, 24);
    u32(&head, 2);
    box_end(&head, b);

    const size_t trak = box(&head, "trak");
    b = box(&head, "tkhd");
    full(&head, 0, 3);
    u32(&head, 0);
    u32(&head, 0);
    u32(&head, 1);
    u32(&head, 0);
    u32(&head, (uint32_t)(duration * 1000 / TIMESCALE));
    zeros(&head, 8);
    u16(&head, 0);
    u16(&head, 0);
    u16(&head, 0);
    u16(&head, 0);
    matrix(&head);
    u32(&head, width << 16);
    u32(&head, height << 16);
    box_end(&head, b);

    const size_t mdia = box(&head, "mdia");
    b = box(&head, "mdhd");
    full(&head, 0, 0);
    u32(&head, 0);
    u32(&head, 0);
    u32(&head, TIMESCALE);
    u32(&head, (uint32_t)duration);
    u16(&head, 0x55c4); /* "und" */
    u16(&head, 0);
    box_end(&head, b);
    b = box(&head, "hdlr");
    full(&head, 0, 0);
    u32(&head, 0);
    put(&head, (const uint8_t *)"vide", 4);
    zeros(&head, 12);
    put(&head, (const uint8_t *)"ESP-KVM\0", 8);
    box_end(&head, b);

    const size_t minf = box(&head, "minf");
    b = box(&head, "vmhd");
    full(&head, 0, 1);
    zeros(&head, 8);
    box_end(&head, b);
    const size_t dinf = box(&head, "dinf");
    const size_t dref = box(&head, "dref");
    full(&head, 0, 0);
    u32(&head, 1);
    b = box(&head, "url ");
    full(&head, 0, 1);
    box_end(&head, b);
    box_end(&head, dref);
    box_end(&head, dinf);

    const size_t stbl = box(&head, "stbl");
    const size_t stsd = box(&head, "stsd");
    full(&head, 0, 0);
    u32(&head, 1);
    const size_t avc1 = box(&head, "avc1");
    zeros(&head, 6);
    u16(&head, 1);
    zeros(&head, 16);
    u16(&head, (uint16_t)width);
    u16(&head, (uint16_t)height);
    u32(&head, 0x480000);
    u32(&head, 0x480000);
    u32(&head, 0);
    u16(&head, 1);
    zeros(&head, 32);
    u16(&head, 0x18);
    u16(&head, 0xffff);
    b = box(&head, "avcC");
    u8(&head, 1);
    u8(&head, params.sps[1]);
    u8(&head, params.sps[2]);
    u8(&head, params.sps[3]);
    u8(&head, 0xff); /* four-byte NAL lengths */
    u8(&head, 0xe1); /* one SPS */
    u16(&head, (uint16_t)params.sps_len);
    put(&head, params.sps, params.sps_len);
    u8(&head, 1);
    u16(&head, (uint16_t)params.pps_len);
    put(&head, params.pps, params.pps_len);
    box_end(&head, b);
    box_end(&head, avc1);
    box_end(&head, stsd);

    /* Durations, run-length coded. */
    const size_t stts = box(&head, "stts");
    full(&head, 0, 0);
    const size_t stts_count_at = head.len;
    u32(&head, 0);
    uint32_t runs = 0;
    for (size_t i = 0; i < sm.count;) {
        size_t j = i;
        while (j < sm.count && sm.delta[j] == sm.delta[i]) {
            j++;
        }
        u32(&head, (uint32_t)(j - i));
        u32(&head, sm.delta[i]);
        runs++;
        i = j;
    }
    if (!head.bad) {
        head.b[stts_count_at] = runs >> 24;
        head.b[stts_count_at + 1] = runs >> 16;
        head.b[stts_count_at + 2] = runs >> 8;
        head.b[stts_count_at + 3] = runs;
    }
    box_end(&head, stts);

    const size_t stss = box(&head, "stss");
    full(&head, 0, 0);
    uint32_t syncs = 0;
    for (size_t i = 0; i < sm.count; i++) {
        syncs += sm.sync[i];
    }
    u32(&head, syncs);
    for (size_t i = 0; i < sm.count; i++) {
        if (sm.sync[i]) {
            u32(&head, (uint32_t)(i + 1));
        }
    }
    box_end(&head, stss);

    b = box(&head, "stsc");
    full(&head, 0, 0);
    u32(&head, 1);
    u32(&head, 1);
    u32(&head, (uint32_t)sm.count);
    u32(&head, 1);
    box_end(&head, b);

    b = box(&head, "stsz");
    full(&head, 0, 0);
    u32(&head, 0);
    u32(&head, (uint32_t)sm.count);
    for (size_t i = 0; i < sm.count; i++) {
        u32(&head, sm.size[i]);
    }
    box_end(&head, b);

    const size_t stco = box(&head, "stco");
    full(&head, 0, 0);
    u32(&head, 1);
    const size_t offset_at = head.len;
    u32(&head, 0); /* filled in once the header's length is known */
    box_end(&head, stco);
    box_end(&head, stbl);
    box_end(&head, minf);
    box_end(&head, mdia);
    box_end(&head, trak);

    /* Chapters, the way Nero wrote them and VLC, mpv and ffmpeg read them. */
    if (chapter_count > 0 && chapters[0].at_us < (int64_t)(duration * 1000000 / TIMESCALE)) {
        const size_t udta = box(&head, "udta");
        b = box(&head, "chpl");
        full(&head, 1, 0);
        u32(&head, 0);
        /* Only chapters inside the video: a cut-off recording can be shorter
         * than the moments it was meant to hold. */
        const int64_t length_us = (int64_t)(duration * 1000000 / TIMESCALE);
        int n = 0;
        for (int i = 0; i < chapter_count && n < 255; i++) {
            n += chapters[i].at_us < length_us;
        }
        u8(&head, (uint8_t)n);
        for (int i = 0, kept = 0; i < chapter_count && kept < n; i++) {
            if (chapters[i].at_us >= length_us) {
                continue;
            }
            kept++;
            int64_t t = chapters[i].at_us;
            if (t < 0) {
                t = 0;
            }
            u64(&head, (uint64_t)t * 10); /* 100 ns units */
            const char *title = chapters[i].title ? chapters[i].title : "";
            size_t tl = strlen(title);
            if (tl > 255) {
                tl = 255;
            }
            u8(&head, (uint8_t)tl);
            put(&head, (const uint8_t *)title, tl);
        }
        box_end(&head, b);
        box_end(&head, udta);
    }
    box_end(&head, moov);

    if (head.bad) {
        snprintf(err, err_cap, "out of memory building the MP4 index");
        goto out;
    }
    const uint32_t data_at = (uint32_t)(head.len + 8);
    head.b[offset_at] = data_at >> 24;
    head.b[offset_at + 1] = data_at >> 16;
    head.b[offset_at + 2] = data_at >> 8;
    head.b[offset_at + 3] = data_at;

    const uint8_t mdat[8] = {(uint8_t)((data_bytes + 8) >> 24), (uint8_t)((data_bytes + 8) >> 16),
                             (uint8_t)((data_bytes + 8) >> 8), (uint8_t)(data_bytes + 8),
                             'm', 'd', 'a', 't'};
    if (!io->write(io->io, head.b, head.len) || !io->write(io->io, mdat, 8)) {
        snprintf(err, err_cap, "could not write the MP4");
        goto out;
    }

    /* Pass two: the samples, in the same order the table counted them. */
    if (!io->rewind(io->io)) {
        snprintf(err, err_cap, "could not read the recording again");
        goto out;
    }
    reader_free(&rd);
    rd = (ts_reader_t){.io = io};
    size_t written = 0;
    bool started = false;
    while ((got = next_au(&rd)) == 1 && written < sm.count) {
        const uint8_t *au = rd.ready.b;
        const size_t au_len = rd.ready.len;
        nal_scan_t probe = {.sps = params.sps, .pps = params.pps,
                            .sps_len = params.sps_len, .pps_len = params.pps_len};
        each_nal(au, au_len, scan_nal, &probe);
        if (!probe.size || (!started && !probe.idr)) {
            continue;
        }
        started = true;
        nal_scan_t w = {.io = io, .ok = true};
        each_nal(au, au_len, write_nal, &w);
        if (!w.ok) {
            snprintf(err, err_cap, "could not write the MP4");
            goto out;
        }
        written++;
    }
    if (written != sm.count) {
        snprintf(err, err_cap, "the recording changed while it was converted");
        goto out;
    }
    ok = true;

out:
    reader_free(&rd);
    free(sm.size);
    free(sm.delta);
    free(sm.sync);
    free(params.sps);
    free(params.pps);
    free(head.b);
    return ok;
}
