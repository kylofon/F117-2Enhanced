/* S4.3: a small worker pool for the per-pixel stages of a display refresh (the compositor, the S3.5 / S3.6 per-pixel pass ends).
 *
 * enh_par_for(n, fn, ctx) splits 0..n-1 into contiguous bands and runs fn(ctx, lo, hi) on the workers and the calling thread, returning when every band
 * is done. fn must only read shared state and write its own band (rows of the canvas): the result is the same as fn(ctx, 0, n) on one thread.
 * --classic, --threads 1 or a single core: everything runs on the calling thread. */
#pragma once

typedef void (*EnhParFn)(void *ctx, int lo, int hi);

void enh_par_for(int n, EnhParFn fn, void *ctx);
void enh_par_for_from(int lo, int hi, EnhParFn fn, void *ctx);   /* the same over lo..hi - 1 */
int  enh_par_threads(void);                 /* the threads a call uses (1 = no workers) */
