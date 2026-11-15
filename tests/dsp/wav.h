/*
 * wav.h - minimal RIFF/WAVE reader and writer for the libe1dsp tools and tests.
 * Reads PCM 8/16/24/32-bit, IEEE float 32/64 and WAVE_FORMAT_EXTENSIBLE variants of those.
 * Writes float32, PCM16 or PCM24 (with TPDF dither for PCM).
 */
#ifndef E1_WAV_H
#define E1_WAV_H

#include <stddef.h>

typedef struct WavData {
    int     channels;
    int     rate;
    int     bits;        /* source bit depth */
    int     is_float;    /* source was IEEE float */
    size_t  frames;
    float  *data;        /* interleaved, frames * channels */
} WavData;

enum { WAV_F32 = 0, WAV_PCM16 = 16, WAV_PCM24 = 24 };

int  wav_read(const char *path, WavData *w);           /* 0 on success, prints a reason otherwise */
int  wav_write(const char *path, const float *data, size_t frames, int channels, int rate, int format);
void wav_free(WavData *w);

#endif
