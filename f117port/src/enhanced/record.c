/* Video capture aid (F117_RECORD=DIR), see record.h. */
#include "enhanced/record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int on = -1;
static FILE *f_raw, *f_idx, *f_wav;
static unsigned long long raw_off;
static unsigned long long wav_frames;
static double t_from = -1, t_to = -1, fps;
static long long last_slot = -1;
static double t_host;            /* the last host present: its time and presentation size (the --fps-test pictures are written with them) */
static int vw_host, vh_host;
static bool sub_on;              /* --fps-test pictures are being written: the host presents are not */

static void wav_header(FILE *f, unsigned long long frames)
{
    const unsigned rate = 49716, data = (unsigned)(frames * 4);
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    const unsigned riff = 36 + data;
    for (int i = 0; i < 4; i++) h[4 + i] = (unsigned char)(riff >> (8 * i));
    memcpy(h + 8, "WAVEfmt ", 8);
    const unsigned v[] = { 16, 1 | (2u << 16), rate, rate * 4, 4 | (16u << 16) };
    for (int k = 0; k < 5; k++)
        for (int i = 0; i < 4; i++) h[16 + 4 * k + i] = (unsigned char)(v[k] >> (8 * i));
    memcpy(h + 36, "data", 4);
    for (int i = 0; i < 4; i++) h[40 + i] = (unsigned char)(data >> (8 * i));
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

bool enh_record_on(void)
{
    if (on >= 0) return on == 1;
    const char *d = getenv("F117_RECORD");
    on = 0;
    if (!d || !*d) return false;
    char p[1024];
    snprintf(p, sizeof p, "%s/frames.raw", d); f_raw = fopen(p, "wb");
    snprintf(p, sizeof p, "%s/frames.txt", d); f_idx = fopen(p, "w");
    snprintf(p, sizeof p, "%s/audio.wav", d);  f_wav = fopen(p, "wb");
    if (!f_raw || !f_idx || !f_wav) { fprintf(stderr, "F117_RECORD: cannot write into %s\n", d); return false; }
    wav_header(f_wav, 0);
    if (getenv("F117_RECORD_FROM")) t_from = atof(getenv("F117_RECORD_FROM"));
    if (getenv("F117_RECORD_TO")) t_to = atof(getenv("F117_RECORD_TO"));
    if (getenv("F117_RECORD_FPS")) fps = atof(getenv("F117_RECORD_FPS"));
    on = 1;
    atexit(enh_record_close);
    return true;
}

void enh_record_frame(const u32 *px, int w, int h, int vw, int vh, double t)
{
    if (!enh_record_on() || !px) return;
    t_host = t; vw_host = vw; vh_host = vh;
    if (sub_on) return;
    if ((t_from >= 0 && t < t_from) || (t_to >= 0 && t > t_to)) return;
    if (fps > 0) {                                    /* one picture per 1/fps slot: the first one presented in it */
        const long long slot = (long long)(t * fps);
        if (slot == last_slot) return;
        last_slot = slot;
    }
    const size_t n = (size_t)w * (size_t)h;
    fwrite(px, 4, n, f_raw);
    fprintf(f_idx, "%.6f %d %d %d %d %llu\n", t, w, h, vw, vh, raw_off);
    raw_off += 4ull * n;
}

void enh_record_sub(const u32 *px, int w, int h, double alpha)
{
    if (!enh_record_on() || !px || !vw_host) return;
    sub_on = true;
    if ((t_from >= 0 && t_host < t_from) || (t_to >= 0 && t_host > t_to)) return;
    const size_t n = (size_t)w * (size_t)h;
    fwrite(px, 4, n, f_raw);
    fprintf(f_idx, "%.6f %d %d %d %d %llu %.4f\n", t_host, w, h, vw_host, vh_host, raw_off, alpha);
    raw_off += 4ull * n;
}

void enh_record_audio(const s16 *stereo, int frames)
{
    if (!enh_record_on() || frames <= 0) return;
    fwrite(stereo, 4, (size_t)frames, f_wav);
    wav_frames += (unsigned long long)frames;
}

void enh_record_close(void)
{
    if (on != 1) return;
    on = 0;
    wav_header(f_wav, wav_frames);
    fclose(f_wav); fclose(f_raw); fclose(f_idx);
    f_wav = f_raw = f_idx = NULL;
}
