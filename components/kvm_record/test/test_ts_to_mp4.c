/*
 * SPDX-FileCopyrightText: 2026 ESP-KVM contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * A stream made by ts_mux, turned into an MP4, and the MP4 read back box by
 * box: index before data, every frame in the table at its length, the
 * keyframes marked, the chapters there. The real decoder check is done with
 * ffmpeg when the code changes; this keeps the structure honest in CI.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ts_mux.h"
#include "ts_to_mp4.h"

static int failures;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            failures++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                            \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t *b;
    size_t len, cap, pos;
} mem_t;

static void mem_put(mem_t *m, const uint8_t *p, size_t n)
{
    if (m->len + n > m->cap) {
        m->cap = (m->len + n) * 2;
        m->b = realloc(m->b, m->cap);
    }
    memcpy(m->b + m->len, p, n);
    m->len += n;
}

static mem_t ts, mp4;
static void on_packet(void *u, const uint8_t *p) { (void)u; mem_put(&ts, p, TS_PACKET); }
static size_t rd(void *io, uint8_t *b, size_t n)
{
    mem_t *m = io;
    size_t k = m->len - m->pos < n ? m->len - m->pos : n;
    memcpy(b, m->b + m->pos, k);
    m->pos += k;
    return k;
}
static bool rw(void *io) { ((mem_t *)io)->pos = 0; return true; }
static bool wr(void *io, const uint8_t *d, size_t n) { (void)io; mem_put(&mp4, d, n); return true; }

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* The first box of @p type inside [p, p+n), searching into containers. */
static const uint8_t *find(const uint8_t *p, size_t n, const char *type, size_t *len)
{
    static const char *const containers[] = {"moov", "trak", "mdia", "minf", "stbl", "udta"};
    size_t i = 0;
    while (i + 8 <= n) {
        const uint32_t sz = be32(p + i);
        if (sz < 8 || i + sz > n) {
            return NULL;
        }
        if (!memcmp(p + i + 4, type, 4)) {
            *len = sz;
            return p + i;
        }
        for (size_t c = 0; c < 6; c++) {
            if (!memcmp(p + i + 4, containers[c], 4)) {
                const uint8_t *r = find(p + i + 8, sz - 8, type, len);
                if (r) {
                    return r;
                }
            }
        }
        i += sz;
    }
    return NULL;
}

int main(void)
{
    ts_mux_t m;
    ts_mux_init(&m, on_packet, NULL);
    /* 50 frames at 25 fps; an IDR with SPS and PPS every 10. Sizes vary. */
    static const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x28, 0xda, 0x01};
    static const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
    size_t expect_data = 0;
    for (int i = 0; i < 50; i++) {
        uint8_t au[4096];
        size_t n = 0;
        const bool idr = i % 10 == 0;
        if (idr) {
            memcpy(au + n, sps, sizeof(sps));
            n += sizeof(sps);
            memcpy(au + n, pps, sizeof(pps));
            n += sizeof(pps);
        }
        const size_t body = 100 + (size_t)i * 37;
        au[n++] = 0;
        au[n++] = 0;
        au[n++] = 1;
        au[n++] = idr ? 0x65 : 0x41;
        for (size_t k = 1; k < body; k++) {
            au[n++] = (uint8_t)(0x10 + k % 200); /* no accidental start codes */
        }
        expect_data += 4 + body; /* length prefix + the slice NAL */
        /* Two dashcam segments joined: the second starts its own muxer and
         * its own clock at frame 30, and the timing must not notice. */
        if (i == 30) {
            ts_mux_init(&m, on_packet, NULL);
        }
        ts_mux_frame(&m, au, n, (uint64_t)(i < 30 ? i : i - 30) * 3600, idr);
    }

    mp4_io_t io = {rd, rw, wr, &ts};
    const mp4_chapter_t ch[] = {{0, "Before"}, {1000000, "Event"}, {99000000, "After the end"}};
    char err[128] = "";
    CHECK(ts_to_mp4(&io, 1920, 1080, ch, 3, err, sizeof(err)));
    if (err[0]) {
        printf("  error: %s\n", err);
    }

    /* ftyp, moov, mdat - in that order. */
    CHECK(mp4.len > 16 && !memcmp(mp4.b + 4, "ftyp", 4));
    const size_t moov_at = be32(mp4.b);
    CHECK(!memcmp(mp4.b + moov_at + 4, "moov", 4));
    const size_t mdat_at = moov_at + be32(mp4.b + moov_at);
    CHECK(!memcmp(mp4.b + mdat_at + 4, "mdat", 4));
    CHECK(be32(mp4.b + mdat_at) == expect_data + 8);
    CHECK(mdat_at + 8 + expect_data == mp4.len);

    size_t len;
    const uint8_t *stsz = find(mp4.b, mp4.len, "stsz", &len);
    CHECK(stsz && be32(stsz + 16) == 50);
    const uint8_t *stss = find(mp4.b, mp4.len, "stss", &len);
    CHECK(stss && be32(stss + 12) == 5 && be32(stss + 16) == 1 && be32(stss + 20) == 11);
    const uint8_t *stco = find(mp4.b, mp4.len, "stco", &len);
    CHECK(stco && be32(stco + 16) == mdat_at + 8);
    const uint8_t *stts = find(mp4.b, mp4.len, "stts", &len);
    CHECK(stts && be32(stts + 12) == 1 && be32(stts + 16) == 50 && be32(stts + 20) == 3600);
    /* avcC sits inside stsd/avc1, behind their fixed fields: look for its tag. */
    const uint8_t *avcc = NULL;
    for (size_t i = 4; i + 4 < moov_at + be32(mp4.b + moov_at) && !avcc; i++) {
        if (!memcmp(mp4.b + i, "avcC", 4)) {
            avcc = mp4.b + i - 4;
        }
    }
    CHECK(avcc && avcc[9] == 0x42 && avcc[11] == 0x28);
    const uint8_t *chpl = find(mp4.b, mp4.len, "chpl", &len);
    CHECK(chpl && chpl[16] == 2); /* the chapter past the end is left out */
    /* The first sample is the IDR slice, length-prefixed, without SPS/PPS/AUD. */
    CHECK(be32(mp4.b + mdat_at + 8) == 100 && mp4.b[mdat_at + 12] == 0x65);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("ts_to_mp4: index first, 50 samples, 5 keyframes, chapters, joined segments - all as expected\n");
    return 0;
}
