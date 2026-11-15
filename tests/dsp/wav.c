/*
 * wav.c - minimal RIFF/WAVE I/O (see wav.h).
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "wav.h"

static uint32_t rd_u32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t rd_u16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static void wr_u32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static void wr_u16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

void wav_free(WavData *w)
{
    if (!w) return;
    free(w->data);
    memset(w, 0, sizeof(*w));
}

int wav_read(const char *path, WavData *w)
{
    memset(w, 0, sizeof(*w));
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "wav: cannot open %s\n", path); return -1; }
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) {
        fprintf(stderr, "wav: %s is not a RIFF/WAVE file\n", path);
        fclose(f);
        return -1;
    }
    int fmt_tag = 0, ch = 0, rate = 0, bits = 0, block = 0, have_fmt = 0;
    unsigned char *raw = NULL;
    uint32_t raw_len = 0;
    for (;;) {
        unsigned char ck[8];
        if (fread(ck, 1, 8, f) != 8) break;
        uint32_t len = rd_u32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            unsigned char fb[64];
            uint32_t take = len < sizeof fb ? len : (uint32_t)sizeof fb;
            if (len < 16 || fread(fb, 1, take, f) != take) break;
            if (len > take) fseek(f, (long)(len - take), SEEK_CUR);
            fmt_tag = rd_u16(fb);
            ch = rd_u16(fb + 2);
            rate = (int)rd_u32(fb + 4);
            block = rd_u16(fb + 12);
            bits = rd_u16(fb + 14);
            if (fmt_tag == 0xFFFE && take >= 26) fmt_tag = rd_u16(fb + 24); /* sub-format GUID data1 */
            have_fmt = 1;
        } else if (!memcmp(ck, "data", 4) && have_fmt) {
            raw = (unsigned char *)malloc(len ? len : 1);
            if (!raw) break;
            raw_len = (uint32_t)fread(raw, 1, len, f);
            break;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
        if (!memcmp(ck, "fmt ", 4) && (len & 1)) fseek(f, 1, SEEK_CUR);
    }
    fclose(f);
    if (!have_fmt || !raw) { fprintf(stderr, "wav: %s has no fmt/data chunk\n", path); free(raw); return -1; }
    int bps = bits / 8;
    int ok = ch > 0 && rate > 0 && block == ch * bps &&
             ((fmt_tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
              (fmt_tag == 3 && (bits == 32 || bits == 64)));
    if (!ok) { fprintf(stderr, "wav: unsupported format tag %d, %d bits\n", fmt_tag, bits); free(raw); return -1; }
    size_t frames = raw_len / (uint32_t)block;
    float *d = (float *)malloc(frames * (size_t)ch * sizeof(float) + 1);
    if (!d) { free(raw); return -1; }
    const unsigned char *p = raw;
    for (size_t i = 0; i < frames * (size_t)ch; ++i, p += bps) {
        float v;
        if (fmt_tag == 3 && bits == 32) { memcpy(&v, p, 4); }
        else if (fmt_tag == 3) { double dv; memcpy(&dv, p, 8); v = (float)dv; }
        else if (bits == 8) v = (float)((int)p[0] - 128) / 128.0f;
        else if (bits == 16) v = (float)(int16_t)rd_u16(p) / 32768.0f;
        else if (bits == 24) { int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; v = (float)s / 8388608.0f; }
        else v = (float)((double)(int32_t)rd_u32(p) / 2147483648.0);
        d[i] = v;
    }
    free(raw);
    w->channels = ch;
    w->rate = rate;
    w->bits = bits;
    w->is_float = fmt_tag == 3;
    w->frames = frames;
    w->data = d;
    return 0;
}

static uint32_t g_dither = 22222u;
static double dither_u(void) { g_dither = g_dither * 1664525u + 1013904223u; return (double)g_dither / 4294967296.0; }

int wav_write(const char *path, const float *data, size_t frames, int channels, int rate, int format)
{
    int bits = format == WAV_PCM16 ? 16 : format == WAV_PCM24 ? 24 : 32;
    int tag = format == WAV_F32 ? 3 : 1, bps = bits / 8;
    size_t n = frames * (size_t)channels, bytes = n * (size_t)bps;
    if (bytes > 0xFFFFFF00u) { fprintf(stderr, "wav: output too large\n"); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "wav: cannot create %s\n", path); return -1; }
    unsigned char h[44];
    memcpy(h, "RIFF", 4); wr_u32(h + 4, (uint32_t)(36 + bytes)); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); wr_u32(h + 16, 16); wr_u16(h + 20, (uint16_t)tag); wr_u16(h + 22, (uint16_t)channels);
    wr_u32(h + 24, (uint32_t)rate); wr_u32(h + 28, (uint32_t)(rate * channels * bps));
    wr_u16(h + 32, (uint16_t)(channels * bps)); wr_u16(h + 34, (uint16_t)bits);
    memcpy(h + 36, "data", 4); wr_u32(h + 40, (uint32_t)bytes);
    int err = fwrite(h, 1, 44, f) != 44;
    unsigned char buf[4096 * 3];
    size_t k = 0;
    for (size_t i = 0; i < n && !err; ++i) {
        float v = data[i];
        if (!isfinite(v)) v = 0.0f;
        if (format == WAV_F32) { memcpy(buf + k, &v, 4); k += 4; }
        else {
            double scale = format == WAV_PCM16 ? 32768.0 : 8388608.0;
            double s = (double)v * scale + (dither_u() - dither_u());
            double lo = -scale, hi = scale - 1.0;
            s = floor(s + 0.5);
            if (s < lo) s = lo;
            if (s > hi) s = hi;
            int32_t q = (int32_t)s;
            buf[k++] = (unsigned char)q;
            buf[k++] = (unsigned char)(q >> 8);
            if (format == WAV_PCM24) buf[k++] = (unsigned char)(q >> 16);
        }
        if (k + 4 > sizeof buf) { err |= fwrite(buf, 1, k, f) != k; k = 0; }
    }
    if (k && !err) err |= fwrite(buf, 1, k, f) != k;
    if (fclose(f) != 0) err = 1;
    if (err) fprintf(stderr, "wav: write error on %s\n", path);
    return err ? -1 : 0;
}
