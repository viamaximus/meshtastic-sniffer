/*
 * meshtastic-sniffer: two-stage cascade DDC channelizer.
 *
 * Stage 1: per-band coarse NCO mix + decimation from SDR rate to an
 *          intermediate rate (~1-2 MHz). Multiple channels in nearby
 *          frequencies share the same band so the SDR-rate FIR work
 *          is amortised across them. Up to MAX_BANDS distinct bands.
 * Stage 2: per-channel fine NCO mix (relative to band center) + the
 *          remaining decimation down to the channel's bw_hz.
 *
 * Net effect at e.g. 20 Msps with 256 channels: stage-1 runs 4-8
 * filter passes at the SDR rate, stage-2 runs 256 passes at ~2 Msps.
 * Old single-stage architecture would have been 256 passes at 20 Msps.
 *
 * Public API matches the old single-stage version so callers are
 * unchanged: channelizer_create / add_channel / process_* / flush /
 * destroy. Per-channel callbacks (cfg.on_baseband) fire from the
 * stage-2 output into the LoRa demod.
 *
 * Copyright (c) 2026 CEMAXECUTER LLC
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "channelizer.h"
#include "options.h"

#include <complex.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__AVX2__)
#include <immintrin.h>
#endif
#if defined(_OPENMP)
#include <omp.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STAGE_FIR_TAPS    64    /* power-of-2 + AVX2-aligned for SIMD FIR */
#define MAX_STAGES        4     /* per-stage cascade depth */
#define MAX_BANDS         32    /* concurrent stage-1 band groups; sized for
                                 * full US-band coverage at all preset BWs.
                                 * Each band runs in its own OpenMP thread,
                                 * so this caps stage-1 parallelism too. */
#define INTERMEDIATE_RATE 2000000 /* target intermediate rate (Hz) */

/* Decimating FIR stage. AVX2-friendly layout:
 *  - hist[] is a 2x doubled circular buffer so the most recent NTAPS
 *    samples are always contiguous starting at write_pos -- no wrap
 *    test inside the inner loop.
 *  - taps_b[] is the same coefficient broadcast as a (re, im) pair so
 *    each AVX2 _mm256_load_ps lifts 4 complex × scalar multiplies.
 *  Memory: 64*2*8 hist + 64*2*4 taps = 1.5 KB per stage. ~1 MB across
 *  256 channels × 4 stages -- fits L2 comfortably. */
typedef struct {
    int             decimation;
    int             count;
    int             write_pos;       /* in [0, STAGE_FIR_TAPS) */
    int             pad_;
    /* Note: NO __attribute__((aligned(32))) on the arrays. The struct gets
     * heap-allocated via calloc (16-byte aligned at most), so the compiler
     * would auto-vectorize with aligned stores under a false assumption and
     * SIGSEGV. The FIR uses _mm256_loadu_ps anyway. */
    float complex   hist[2 * STAGE_FIR_TAPS];
    float           taps_b[2 * STAGE_FIR_TAPS];
} decim_stage_t;

typedef struct {
    int             active;
    double          center_freq;        /* absolute Hz */
    double          intermediate_rate;  /* Hz after stage-1 decim */
    /* Stage-1 NCO (offset from SDR center) */
    double          nco_freq;
    float complex   nco_phasor;
    float complex   nco_current;
    int             nco_renorm;
    /* Stage-1 cascaded decimation */
    decim_stage_t   stages[MAX_STAGES];
    int             num_stages;
    /* Membership */
    int             channel_indices[CHANNELIZER_MAX_CHANNELS];
    int             num_channels;
} band_state_t;

typedef struct chan_state {
    int               id;
    channel_cfg_t     cfg;
    int               band_idx;          /* index into channelizer->bands[] */
    /* Stage-2 NCO (offset from band center, runs at intermediate_rate) */
    double            nco_freq;
    float complex     nco_phasor;
    float complex     nco_current;
    int               nco_renorm;
    /* Stage-2 cascade (intermediate_rate -> bw_hz) */
    decim_stage_t     stages[MAX_STAGES];
    int               num_stages;
    /* Output batching to the demod callback */
    float complex     outbuf[CHANNELIZER_OUTBUF_SAMPLES];
    int               outbuf_count;
} chan_state_t;

struct channelizer {
    uint64_t        f_center;
    uint32_t        samp_rate;
    int             n_channels;
    chan_state_t   *channels[CHANNELIZER_MAX_CHANNELS];
    int             n_bands;
    band_state_t    bands[MAX_BANDS];
    /* Workbuf for the cs8/cf32 input batch; lets per-band threads share
     * a single int8-to-complex conversion instead of repeating it. */
    float complex  *workbuf;
    size_t          workbuf_cap;
};

/* ---- FIR design (Blackman-windowed sinc) ---- */
static void design_lowpass(float *taps, int ntaps, double cutoff_norm)
{
    int M = ntaps - 1;
    double sum = 0.0;
    for (int i = 0; i < ntaps; ++i) {
        double n = i - M / 2.0;
        double h = (fabs(n) < 1e-10)
                 ? 2.0 * cutoff_norm
                 : sin(2.0 * M_PI * cutoff_norm * n) / (M_PI * n);
        double w = 0.42 - 0.5 * cos(2.0 * M_PI * i / M)
                        + 0.08 * cos(4.0 * M_PI * i / M);
        taps[i] = (float)(h * w);
        sum += taps[i];
    }
    if (sum > 0.0)
        for (int i = 0; i < ntaps; ++i) taps[i] /= (float)sum;
}

/* Build the broadcast-tap layout for the FIR. The window is applied newest-
 * first by walking hist[write_pos .. write_pos + NTAPS), so taps must be
 * stored in REVERSE order (taps[NTAPS-1-k] for the k-th oldest sample). */
static void install_taps(decim_stage_t *st, double cutoff)
{
    float taps[STAGE_FIR_TAPS];
    design_lowpass(taps, STAGE_FIR_TAPS, cutoff);
    for (int k = 0; k < STAGE_FIR_TAPS; ++k) {
        float t = taps[STAGE_FIR_TAPS - 1 - k];
        st->taps_b[2*k]     = t;
        st->taps_b[2*k + 1] = t;
    }
}

/* Plan a cascade of decimators that multiplies to `total`, each <=
 * max_per_stage. Returns the number of stages used. */
static int plan_decimation(int total, int max_per_stage,
                           int *decim_out, int max_stages)
{
    int n = 0;
    int remaining = total;
    while (remaining > 1 && n < max_stages) {
        if (remaining <= max_per_stage) {
            decim_out[n++] = remaining;
            remaining = 1;
        } else {
            int best = 2;
            for (int d = max_per_stage; d >= 2; --d) {
                if (remaining % d == 0) { best = d; break; }
            }
            decim_out[n++] = best;
            remaining /= best;
        }
    }
    if (remaining > 1 && n < max_stages)
        decim_out[n++] = remaining;
    return n;
}

/* Largest divisor of n that is <= limit. */
static int largest_factor_leq(int n, int limit)
{
    if (n <= 1) return 1;
    int best = 1;
    for (int d = 2; d * d <= n; ++d) {
        if (n % d != 0) continue;
        if (d <= limit && d > best) best = d;
        int other = n / d;
        if (other <= limit && other > best) best = other;
    }
    if (n <= limit && n > best) best = n;
    return best;
}

/* Decimating FIR step. Writes the new sample into BOTH halves of the
 * doubled history (so the contiguous NTAPS window starting at write_pos
 * always represents the most recent samples), then if a decimated output
 * is due, runs the broadcast-tap dot product. AVX2 path lifts 4 complex
 * MACs per FMA; falls back to scalar otherwise. Returns 1 if *out was
 * written, 0 if the sample was just buffered. */
static inline int decim_stage_process(decim_stage_t *st,
                                      float complex in,
                                      float complex *out)
{
    st->hist[st->write_pos] = in;
    st->hist[st->write_pos + STAGE_FIR_TAPS] = in;
    st->write_pos = (st->write_pos + 1) & (STAGE_FIR_TAPS - 1);
    if (++st->count < st->decimation) return 0;
    st->count = 0;

    const float *h = (const float *)&st->hist[st->write_pos];
    const float *t = st->taps_b;

#if defined(__AVX2__)
    __m256 acc0 = _mm256_setzero_ps();
    __m256 acc1 = _mm256_setzero_ps();
    /* 16 iterations × 8 floats = 128 floats = 64 complex × 2 (taps duplicated).
     * Use unaligned loads since calloc()'d struct hosts may not be 32-byte
     * aligned even though the members are alignas(32). */
    for (int k = 0; k < 2 * STAGE_FIR_TAPS; k += 16) {
        __m256 hv0 = _mm256_loadu_ps(h + k);
        __m256 tv0 = _mm256_loadu_ps(t + k);
        acc0 = _mm256_fmadd_ps(hv0, tv0, acc0);
        __m256 hv1 = _mm256_loadu_ps(h + k + 8);
        __m256 tv1 = _mm256_loadu_ps(t + k + 8);
        acc1 = _mm256_fmadd_ps(hv1, tv1, acc1);
    }
    __m256 acc = _mm256_add_ps(acc0, acc1);
    /* Horizontal-add: pack 4 (re, im) pairs into a single complex. */
    __m128 lo = _mm256_castps256_ps128(acc);
    __m128 hi = _mm256_extractf128_ps(acc, 1);
    __m128 sum128 = _mm_add_ps(lo, hi);              /* (a0,b0,a1,b1) */
    __m128 shuf   = _mm_movehl_ps(sum128, sum128);   /* (a1,b1,a1,b1) */
    __m128 final2 = _mm_add_ps(sum128, shuf);        /* (a0+a1, b0+b1, ...) */
    *out = final2[0] + I * final2[1];
#else
    float complex acc = 0.0f + 0.0f * I;
    const float complex *hc = (const float complex *)h;
    for (int k = 0; k < STAGE_FIR_TAPS; ++k)
        acc += hc[k] * t[2 * k];
    *out = acc;
#endif
    return 1;
}

/* ---- Band creation / lookup ---- */

static int init_band(band_state_t *b, double center_freq,
                     double samp_rate, int s1_decim)
{
    memset(b, 0, sizeof(*b));
    b->active = 1;
    b->center_freq = center_freq;
    if (s1_decim < 1) s1_decim = 1;
    b->intermediate_rate = samp_rate / s1_decim;

    int decims[MAX_STAGES];
    b->num_stages = plan_decimation(s1_decim, 16, decims, MAX_STAGES);
    double rate = samp_rate;
    for (int i = 0; i < b->num_stages; ++i) {
        b->stages[i].decimation = decims[i];
        b->stages[i].count = 0;
        b->stages[i].write_pos = 0;
        memset(b->stages[i].hist, 0, sizeof(b->stages[i].hist));
        install_taps(&b->stages[i], 0.4 / decims[i]);
        rate /= decims[i];
    }
    return 0;
}

/* Find an existing band that already absorbs this frequency, or create
 * a new one. The band must use the *same* s1_decim so all its channels
 * agree on intermediate_rate. */
static int find_or_create_band(channelizer_t *c, double freq,
                               int channel_slot, int s1_decim)
{
    double inter_rate = (double)c->samp_rate / (double)s1_decim;
    /* Allow channels within ±40% of the intermediate band to share. */
    double tol = inter_rate * 0.4;

    for (int b = 0; b < c->n_bands; ++b) {
        band_state_t *bd = &c->bands[b];
        if (!bd->active) continue;
        if (fabs(bd->intermediate_rate - inter_rate) > inter_rate * 0.01) continue;
        if (fabs(bd->center_freq - freq) <= tol &&
            bd->num_channels < CHANNELIZER_MAX_CHANNELS) {
            bd->channel_indices[bd->num_channels++] = channel_slot;
            return b;
        }
    }
    if (c->n_bands >= MAX_BANDS) {
        if (verbose)
            fprintf(stderr, "channelizer: hit MAX_BANDS=%d, can't add more clusters\n", MAX_BANDS);
        return -1;
    }
    int bidx = c->n_bands++;
    band_state_t *bd = &c->bands[bidx];
    if (init_band(bd, freq, (double)c->samp_rate, s1_decim) != 0) {
        --c->n_bands;
        return -1;
    }
    /* Stage-1 NCO mixes band center down to DC at the SDR rate. */
    bd->nco_freq = freq - (double)c->f_center;
    extern double ppm_correction;
    bd->nco_freq -= (double)c->f_center * ppm_correction * 1e-6;
    double phase_inc = -2.0 * M_PI * bd->nco_freq / (double)c->samp_rate;
    bd->nco_phasor  = (float complex)(cos(phase_inc) + I * sin(phase_inc));
    bd->nco_current = 1.0f + 0.0f * I;
    bd->channel_indices[bd->num_channels++] = channel_slot;
    if (verbose) {
        fprintf(stderr,
                "channelizer: new band %d  center=%.3f MHz  s1_decim=%d  "
                "inter_rate=%.0f Hz\n",
                bidx, freq / 1e6, s1_decim, bd->intermediate_rate);
    }
    return bidx;
}

/* ---- Public API ---- */

channelizer_t *channelizer_create(uint64_t f_center, uint32_t samp_rate)
{
    channelizer_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->f_center  = f_center;
    c->samp_rate = samp_rate;
    return c;
}

int channelizer_add_channel(channelizer_t *c, const channel_cfg_t *cfg)
{
    if (!c || !cfg) return -1;
    if (c->n_channels >= CHANNELIZER_MAX_CHANNELS) return -1;
    if (cfg->bw_hz <= 0 || c->samp_rate == 0) return -1;
    if ((uint32_t)cfg->bw_hz > c->samp_rate) return -1;
    if (c->samp_rate % (uint32_t)cfg->bw_hz != 0) {
        if (verbose)
            fprintf(stderr, "channelizer: non-integer decimation: rate=%u bw=%d\n",
                    c->samp_rate, cfg->bw_hz);
        return -1;
    }

    int slot = c->n_channels;
    chan_state_t *s = calloc(1, sizeof(*s));
    if (!s) return -1;
    s->id  = slot;
    s->cfg = *cfg;
    if (s->cfg.os_factor <= 0) s->cfg.os_factor = 1;

    /* Total decim = samp_rate / (os_factor * bw_hz). os_factor=1 emits at
     * exactly bw_hz (legacy LoRa demod input). os_factor>=2 leaves room
     * for the LoRa demod's fractional-STO interpolation -- needed to
     * lock real-radio captures with sub-sample timing offsets. */
    uint32_t out_rate = (uint32_t)s->cfg.os_factor * (uint32_t)cfg->bw_hz;
    if (out_rate > c->samp_rate) { free(s); return -1; }
    if (c->samp_rate % out_rate != 0) {
        if (verbose)
            fprintf(stderr, "channelizer: non-integer total decim: rate=%u out_rate=%u\n",
                    c->samp_rate, out_rate);
        free(s);
        return -1;
    }
    int total_decim = (int)(c->samp_rate / out_rate);
    int max_s1 = (int)(c->samp_rate / INTERMEDIATE_RATE);
    if (max_s1 < 1) max_s1 = 1;
    int s1_decim = largest_factor_leq(total_decim, max_s1);
    int s2_decim = total_decim / s1_decim;
    if (s2_decim < 1) s2_decim = 1;

    /* Find or create the stage-1 band that owns this frequency. */
    int bidx = find_or_create_band(c, (double)cfg->f_hz, slot, s1_decim);
    if (bidx < 0) { free(s); return -1; }
    s->band_idx = bidx;
    band_state_t *bd = &c->bands[bidx];

    /* Stage-2 NCO: shift channel from band-relative to DC at intermediate rate. */
    s->nco_freq = (double)cfg->f_hz - bd->center_freq;
    double phase_inc = -2.0 * M_PI * s->nco_freq / bd->intermediate_rate;
    s->nco_phasor  = (float complex)(cos(phase_inc) + I * sin(phase_inc));
    s->nco_current = 1.0f + 0.0f * I;

    /* Stage-2 decimation cascade. */
    int decims[MAX_STAGES];
    s->num_stages = plan_decimation(s2_decim, 16, decims, MAX_STAGES);
    /* Each stage filters at the LoRa half-bandwidth in absolute terms
     * (bw_hz/2 Hz), which keeps the chirp edges intact across the
     * cascade. cutoff in stage-input-normalized units = bw/(2*rate).
     * Using 0.4/decim instead would alias the signal edges off the
     * last stage when the output rate equals bw_hz. */
    double stage_in_rate = bd->intermediate_rate;
    double half_bw = (double)cfg->bw_hz * 0.5;
    for (int i = 0; i < s->num_stages; ++i) {
        s->stages[i].decimation = decims[i];
        s->stages[i].count = 0;
        s->stages[i].write_pos = 0;
        memset(s->stages[i].hist, 0, sizeof(s->stages[i].hist));
        double cutoff = half_bw / stage_in_rate;
        if (cutoff > 0.49) cutoff = 0.49;   /* safety: stay below Nyquist */
        install_taps(&s->stages[i], cutoff);
        stage_in_rate /= decims[i];
    }

    int new_id = c->n_channels;
    c->channels[new_id] = s;
    __atomic_store_n(&c->n_channels, new_id + 1, __ATOMIC_RELEASE);

    if (verbose) {
        fprintf(stderr,
                "channelizer ch%-3d: %.3f MHz  band=%d  s1=%d  s2=%d  "
                "BW=%dkHz SF%d CR4/%d\n",
                slot, cfg->f_hz / 1e6, bidx, s1_decim, s2_decim,
                cfg->bw_hz / 1000, cfg->sf, cfg->cr);
    }
    return slot;
}

int channelizer_num_channels(const channelizer_t *c)
{
    return c ? c->n_channels : 0;
}

/* Push one stage-2 sample to a channel's output buffer; flush to demod
 * callback when full. */
static inline void emit_to_channel(chan_state_t *s, float complex x)
{
    s->outbuf[s->outbuf_count++] = x;
    if (s->outbuf_count == CHANNELIZER_OUTBUF_SAMPLES) {
        if (s->cfg.on_baseband)
            s->cfg.on_baseband(s->id, s->outbuf, (size_t)s->outbuf_count, s->cfg.user);
        s->outbuf_count = 0;
    }
}

/* Process one full wideband-input batch through a single band's stage-1
 * cascade and that band's stage-2 per-channel cascades. This is the
 * OpenMP work unit: one thread per band, each thread sweeping the entire
 * input batch for ITS band. Bands have fully independent state, so no
 * synchronization is needed across threads.
 *
 * NOTE: chan_state's nco_current/renorm/stages/outbuf are read+written
 * here. Each channel belongs to exactly one band, so parallelism over
 * bands keeps channel state thread-local. */
static void process_band_batch(channelizer_t *c, band_state_t *bd,
                               const float complex *iq, size_t n)
{
    if (!bd->active) return;
    for (size_t i = 0; i < n; ++i) {
        /* Stage-1 NCO mix to band center. */
        float complex y = iq[i] * bd->nco_current;
        bd->nco_current *= bd->nco_phasor;
        if (++bd->nco_renorm >= 1024) {
            bd->nco_renorm = 0;
            float mag = cabsf(bd->nco_current);
            if (mag > 0.0f) bd->nco_current /= mag;
        }
        /* Stage-1 cascade. */
        int produced = 1;
        for (int s1 = 0; s1 < bd->num_stages && produced; ++s1) {
            float complex out;
            produced = decim_stage_process(&bd->stages[s1], y, &out);
            y = out;
        }
        if (!produced) continue;
        /* Stage-2: for every stage-1 output sample, push through every
         * channel in this band. */
        for (int ci = 0; ci < bd->num_channels; ++ci) {
            int idx = bd->channel_indices[ci];
            chan_state_t *s = c->channels[idx];
            if (!s) continue;
            float complex z = y * s->nco_current;
            s->nco_current *= s->nco_phasor;
            if (++s->nco_renorm >= 1024) {
                s->nco_renorm = 0;
                float mag = cabsf(s->nco_current);
                if (mag > 0.0f) s->nco_current /= mag;
            }
            int p2 = 1;
            for (int s2 = 0; s2 < s->num_stages && p2; ++s2) {
                float complex out;
                p2 = decim_stage_process(&s->stages[s2], z, &out);
                z = out;
            }
            if (p2) emit_to_channel(s, z);
        }
    }
}

/* Ensure the workbuf has at least `n` complex slots. Single producer, so
 * realloc-up-only is safe between batches. */
static int ensure_workbuf(channelizer_t *c, size_t n)
{
    if (c->workbuf_cap >= n) return 0;
    float complex *nb = realloc(c->workbuf, n * sizeof(float complex));
    if (!nb) return -1;
    c->workbuf = nb;
    c->workbuf_cap = n;
    return 0;
}

void channelizer_process_int8(channelizer_t *c, const int8_t *iq, size_t n)
{
    if (!c || n == 0) return;
    if (ensure_workbuf(c, n) < 0) return;
    const float scale = 1.0f / 127.0f;
    for (size_t i = 0; i < n; ++i) {
        c->workbuf[i] = (float)iq[2*i] * scale + I * (float)iq[2*i + 1] * scale;
    }
    int n_bands = c->n_bands;
#if defined(_OPENMP)
    #pragma omp parallel for schedule(dynamic, 1)
#endif
    for (int b = 0; b < n_bands; ++b) {
        process_band_batch(c, &c->bands[b], c->workbuf, n);
    }
}

void channelizer_process_float(channelizer_t *c, const float complex *iq, size_t n)
{
    if (!c || n == 0) return;
    int n_bands = c->n_bands;
#if defined(_OPENMP)
    #pragma omp parallel for schedule(dynamic, 1)
#endif
    for (int b = 0; b < n_bands; ++b) {
        process_band_batch(c, &c->bands[b], iq, n);
    }
}

void channelizer_flush(channelizer_t *c)
{
    if (!c) return;
    int n_ch = __atomic_load_n(&c->n_channels, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n_ch; ++i) {
        chan_state_t *s = c->channels[i];
        if (!s || s->outbuf_count == 0) continue;
        if (s->cfg.on_baseband)
            s->cfg.on_baseband(s->id, s->outbuf, (size_t)s->outbuf_count, s->cfg.user);
        s->outbuf_count = 0;
    }
}

void channelizer_destroy(channelizer_t *c)
{
    if (!c) return;
    for (int i = 0; i < c->n_channels; ++i) {
        if (c->channels[i]) free(c->channels[i]);
    }
    free(c->workbuf);
    free(c);
}
