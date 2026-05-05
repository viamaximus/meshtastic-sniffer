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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define STAGE_FIR_TAPS    63
#define MAX_STAGES        4     /* per-stage cascade depth */
#define MAX_BANDS         8     /* concurrent stage-1 band groups */
#define INTERMEDIATE_RATE 2000000 /* target intermediate rate (Hz) */

typedef struct {
    int             decimation;
    int             count;
    float           taps[STAGE_FIR_TAPS];
    float complex   hist[STAGE_FIR_TAPS];
    int             hist_idx;
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

/* Decimating-FIR step: returns 1 if a sample was emitted to *out. */
static inline int decim_stage_process(decim_stage_t *st,
                                      float complex in,
                                      float complex *out)
{
    st->hist[st->hist_idx] = in;
    st->hist_idx = (st->hist_idx + 1) % STAGE_FIR_TAPS;
    if (++st->count < st->decimation) return 0;
    st->count = 0;
    float complex acc = 0;
    int idx = st->hist_idx;
    for (int t = 0; t < STAGE_FIR_TAPS; ++t) {
        idx = idx ? idx - 1 : STAGE_FIR_TAPS - 1;
        acc += st->hist[idx] * st->taps[t];
    }
    *out = acc;
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
        b->stages[i].hist_idx = 0;
        memset(b->stages[i].hist, 0, sizeof(b->stages[i].hist));
        double cutoff = 0.4 / decims[i];
        design_lowpass(b->stages[i].taps, STAGE_FIR_TAPS, cutoff);
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
        s->stages[i].hist_idx = 0;
        memset(s->stages[i].hist, 0, sizeof(s->stages[i].hist));
        double cutoff = half_bw / stage_in_rate;
        if (cutoff > 0.49) cutoff = 0.49;   /* safety: stay below Nyquist */
        design_lowpass(s->stages[i].taps, STAGE_FIR_TAPS, cutoff);
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

/* Run one wideband sample through stage-1 (per-band) and feed each
 * decimated stage-1 output through stage-2 (per-channel) cascades. */
static inline void process_one_sample(channelizer_t *c, float complex x)
{
    int n_bands = c->n_bands;
    for (int b = 0; b < n_bands; ++b) {
        band_state_t *bd = &c->bands[b];
        if (!bd->active) continue;
        /* Stage-1 NCO mix to band center. */
        float complex y = x * bd->nco_current;
        bd->nco_current *= bd->nco_phasor;
        if (++bd->nco_renorm >= 1024) {
            bd->nco_renorm = 0;
            float mag = cabsf(bd->nco_current);
            if (mag > 0.0f) bd->nco_current /= mag;
        }
        /* Stage-1 cascade. */
        int produced = 1;
        for (int i = 0; i < bd->num_stages && produced; ++i) {
            float complex out;
            produced = decim_stage_process(&bd->stages[i], y, &out);
            y = out;
        }
        if (!produced) continue;
        /* For every stage-1 output sample, push through every channel
         * in this band (stage-2). */
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
            for (int i = 0; i < s->num_stages && p2; ++i) {
                float complex out;
                p2 = decim_stage_process(&s->stages[i], z, &out);
                z = out;
            }
            if (p2) emit_to_channel(s, z);
        }
    }
}

void channelizer_process_int8(channelizer_t *c, const int8_t *iq, size_t n)
{
    if (!c) return;
    const float scale = 1.0f / 127.0f;
    for (size_t i = 0; i < n; ++i) {
        float complex x = (float)iq[2*i] * scale + I * (float)iq[2*i + 1] * scale;
        process_one_sample(c, x);
    }
}

void channelizer_process_float(channelizer_t *c, const float complex *iq, size_t n)
{
    if (!c) return;
    for (size_t i = 0; i < n; ++i)
        process_one_sample(c, iq[i]);
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
    free(c);
}
