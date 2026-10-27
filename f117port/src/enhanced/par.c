/* S4.3: the worker pool of the display refresh (par.h). */
#include "enhanced/par.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "enhanced/enhanced.h"

#define MAX_WORKERS 15

static int nthreads;                        /* 0 = not set up yet */
static SDL_Thread *thr[MAX_WORKERS];
static SDL_Semaphore *go[MAX_WORKERS], *done;
static struct { EnhParFn fn; void *ctx; int base, n, parts; } job;

static void band(int i, int *lo, int *hi)
{
    *lo = job.base + (int)((long long)job.n * i / job.parts);
    *hi = job.base + (int)((long long)job.n * (i + 1) / job.parts);
}

static int SDLCALL worker(void *arg)
{
    const int i = (int)(intptr_t)arg;
    for (;;) {
        SDL_WaitSemaphore(go[i]);
        int lo, hi;
        band(i + 1, &lo, &hi);
        if (lo < hi) job.fn(job.ctx, lo, hi);
        SDL_SignalSemaphore(done);
    }
    return 0;
}

static void setup(void)
{
    int want = SDL_GetNumLogicalCPUCores();
    const char *e = SDL_getenv("F117_THREADS");               /* test aid: the threads to use (1 = none) */
    if (e && *e) want = SDL_atoi(e);
    if (enh.classic) want = 1;
    if (want > MAX_WORKERS + 1) want = MAX_WORKERS + 1;
    if (want > 8) want = 8;                                    /* the bands are memory bound: more threads gain little */
    nthreads = 1;
    if (want < 2) return;
    done = SDL_CreateSemaphore(0);
    if (!done) return;
    for (int i = 0; i < want - 1; i++) {
        go[i] = SDL_CreateSemaphore(0);
        thr[i] = go[i] ? SDL_CreateThread(worker, "f117-par", (void *)(intptr_t)i) : NULL;
        if (!thr[i]) break;
        nthreads++;
    }
}

int enh_par_threads(void)
{
    if (!nthreads) setup();
    return nthreads;
}

void enh_par_for(int n, EnhParFn fn, void *ctx) { enh_par_for_from(0, n, fn, ctx); }

void enh_par_for_from(int base, int end, EnhParFn fn, void *ctx)
{
    const int n = end - base;
    if (n <= 0) return;
    if (!nthreads) setup();
    int parts = nthreads < n ? nthreads : n;
    if (parts < 2) { fn(ctx, base, end); return; }
    job.fn = fn; job.ctx = ctx; job.base = base; job.n = n; job.parts = parts;
    for (int i = 0; i < parts - 1; i++) SDL_SignalSemaphore(go[i]);
    int lo, hi;
    band(0, &lo, &hi);
    fn(ctx, lo, hi);
    for (int i = 0; i < parts - 1; i++) SDL_WaitSemaphore(done);
}
