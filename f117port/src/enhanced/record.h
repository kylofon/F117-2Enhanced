/* Video capture aid (F117_RECORD=DIR; port/notes/video/make_video.py).
 *
 * Every presented picture (the composed canvas, as the host presents it) is appended to DIR/frames.raw (XRGB8888, rows top down) with one line
 * per picture in DIR/frames.txt: "t w h vw vh offset" (t = seconds on the host clock, w x h the canvas, vw x vh its logical presentation size,
 * offset in frames.raw). The OPL chip is rendered tick by tick into DIR/audio.wav (stereo s16 at the chip's 49 716 Hz; sample n is at
 * n / 49716 s on the same clock). Headless with the virtual clock (F117_VCLOCK=1) the capture is deterministic and not bound to real time.
 * F117_RECORD_FROM / F117_RECORD_TO (seconds): only pictures in that span are written (the audio is always whole). Off by default; it never
 * changes what is simulated or drawn. F117_RECORD_FPS=F: at most one picture per 1/F s slot (the first presented in it; the host presents
 * up to once per tick, 70 a second, even when only a lamp changed). With --fps-test K (headless, no display clock) the K refresh pictures of each step
 * are written instead of the host presents (enh_record_sub), at the time of the last host present with a 7th column, the refresh's alpha: the encoder
 * spreads them over the step (port/notes/video/smooth_clips.py). */
#pragma once

#include "types.h"

bool enh_record_on(void);
void enh_record_frame(const u32 *px, int w, int h, int vw, int vh, double t);
void enh_record_sub(const u32 *px, int w, int h, double alpha);   /* a --fps-test refresh picture */
void enh_record_audio(const s16 *stereo, int frames);
void enh_record_close(void);
