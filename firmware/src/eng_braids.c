/* SPDX-License-Identifier: GPL-3.0-only
 * FM1 Rack (a Felucca fork): the BRAIDS engine, engine 14.
 *
 * Sound: Braids' macro oscillator (Mutable Instruments, MIT), 43 of its 47 models; the four wavetable models
 * (WTBL WMAP WLIN WTX4) are left out with their 33 KB of tables, which the FM-1's flash has no room for. The
 * part's own ADSR is the VCA (voice.c), so BRAIDS is SRC -> AMP; Felucca's FX, matrix and sequencer follow.
 *
 * Modulation inside the engine, one modulator per part (EDIT 2, MOD), its value to TIMBRE by DEPTH:
 *   PENV  Peaks' envelope (MIT; multistage_envelope.cc), gated while a key of the part is held: ATK DEC SUS
 *   PLFO  Peaks' LFO (MIT; lfo.cc), free-running: RATE SHAPE (Peaks' shape presets)
 *   TIDE  an AD slope started by each note: TIME (2 ms .. 10 s), SHAPE (log 0 .. linear 64 .. expo 127), SLOPE
 *         (the rise's share). After Tides' AD mode, written here in integer C: Tides v1's own code needs ~55 KB of
 *         tables (its wave bank), more than the flash has left. Not Mutable's code
 *   LOOP  the same slope looping: RATE (0.05 .. 100 Hz), SHAPE, SLOPE; bipolar
 *   GBD GSD GHH  Grids' drum map (GPL-3.0; grids_map.h), one of its three lanes, clocked by the transport's 1/16
 *         steps (silent while stopped): X Y FILL. A hit is a short envelope (an accent a full one) and strikes the
 *         part's sounding oscillators (re-excites PLUCK, BELL, DRUM, KICK, CYMB, SNARE ...)
 * The track's FLT modulation (LFO DEST / ENV DEST / the matrix's CUT) moves TIMBRE, SHP moves COLOR, as NOISE does.
 *
 * Oscillators: MI_NOSC (4) for all parts' voices (17 KB each in the pool: mi_glue.cc). note_on (core 0,
 * events_block) gives a voice the slot it had, else a free one, else the oldest's; a voice whose slot was taken
 * renders nothing until it ends. One part plays at most 4 voices (engine_t.poly); two parts with 4 each share them.
 * State: s[0] the slot + 1, s[1] s[2] the output's DC blocker (Braids' jack is AC-coupled; SQSUB, VOSIM and others
 * carry DC), all kept over a retrigger.
 *
 * Rates: Braids was designed at 96 kHz and Peaks at 48 kHz; at Felucca's 44.1 kHz Braids is retuned
 * (BRD_PITCH0, checked by tests/braids_test.c), and Peaks' times run 8.8 % long (its tables are kept as they are). */
#include "../mi/mi_glue.h"
#include "grids_map.h"

static int32_t seq_clock16(void);                    /* (seq.c) the transport's 1/16 count, -1 stopped */

/* MODEL -> braids::MacroOscillatorShape (settings.h order) */
static const uint8_t BRD_SHAPE[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,   /* CSAW .. TOY */
    17, 18, 19, 20, 21, 22, 23, 24,                             /* ZLPF .. HARM */
    25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,             /* FM .. SNARE */
    41, 42, 43, 44, 45, 46,                                     /* NOISE .. QPSK (37..40: the wavetables, out) */
};
static const char *const N_BRD_MODEL[] = {
    "CSAW", "MORPH", "SAWSQ", "FOLD", "BUZZ", "SQSUB", "SWSUB", "SQSYN", "SWSYN", "SAW3", "SQR3", "TRI3",
    "SIN3", "RING", "SWARM", "COMB", "TOY", "ZLPF", "ZPKF", "ZBPF", "ZHPF", "VOSIM", "VOWEL", "VFOF", "HARM",
    "FM", "FBFM", "WTFM", "PLUCK", "BOWED", "BLOWN", "FLUTE", "BELL", "DRUM", "KICK", "CYMB", "SNARE",
    "NOISE", "TWNQ", "CLKN", "CLOUD", "PRTCL", "QPSK"};
#define BRD_NMODEL ((int16_t)NELEM(BRD_SHAPE))
_Static_assert(NELEM(BRD_SHAPE) == NELEM(N_BRD_MODEL), "BRAIDS: a name per model");

enum { BRM_OFF, BRM_PENV, BRM_PLFO, BRM_TIDE, BRM_LOOP, BRM_GBD, BRM_GSD, BRM_GHH };
static const char *const N_BRD_MOD[] = {"OFF", "PENV", "PLFO", "TIDE", "LOOP", "GBD", "GSD", "GHH"};

/* EDIT 2's labels follow MOD (brd_desc): {P1, P2, P3} */
static const param_desc_t BRD_D_ENV[3] = {{"ATK", F_PCT, 0, 127, 0, 0, 0}, {"DEC", F_PCT, 0, 127, 0, 0, 0},
                                          {"SUS", F_PCT, 0, 127, 0, 0, 0}};
static const param_desc_t BRD_D_LFO[3] = {{"RATE", F_PCT, 0, 127, 0, 0, 0}, {"SHAP", F_PCT, 0, 127, 0, 0, 0},
                                          {"-", F_PCT, 0, 127, 0, 0, 0}};
static const param_desc_t BRD_D_TIDE[3] = {{"TIME", F_PCT, 0, 127, 0, 0, 0}, {"SHAP", F_PCT, 0, 127, 0, 0, 0},
                                           {"SLOP", F_PCT, 0, 127, 0, 0, 0}};
static const param_desc_t BRD_D_GRID[3] = {{"X", F_PCT, 0, 127, 0, 0, 0}, {"Y", F_PCT, 0, 127, 0, 0, 0},
                                           {"FILL", F_PCT, 0, 127, 0, 0, 0}};
static const param_desc_t *brd_desc(const track_t *t, uint32_t k)
{
    int32_t mod = t->p[P_E4];
    const param_desc_t *d;
    if (k < 5u)
        return 0;
    d = mod == BRM_PENV ? BRD_D_ENV : mod == BRM_PLFO ? BRD_D_LFO : mod >= BRM_GBD ? BRD_D_GRID : BRD_D_TIDE;
    return &d[k - 5u];
}

/* Braids' pitch (1/128 semitone) of Felucca's pitch16 0 (MIDI note 0): Braids' tables are for 96 kHz, so at FS
 * the note is lower by 12 log2(96000 / FS) semitones, put back here (tests/braids_test.c: A4 = 440 Hz) */
#ifndef BRD_PITCH0
#define BRD_PITCH0 1724                      /* 12 x 128 x log2(96000 / 44100) = 1724.5 */
#endif
_Static_assert(FS == 44100, "BRAIDS: BRD_PITCH0 is for 44.1 kHz");
#define BRD_GAIN 176                                 /* Q8: Braids' full scale to Felucca's engine level */
#define BRD_CHUNK 16u                                /* render chunks (<= MI_BLOCK) */

/* ---------------------------------------------------------------- oscillator slots --- */
static voice_t *brd_owner[MI_NOSC];
static uint32_t brd_age[MI_NOSC], brd_clock;
static uint8_t brd_ready;                            /* the slots' Init has run (the first note) */

static int brd_held(uint32_t k)                      /* slot k sounds for its voice */
{
    const voice_t *o = brd_owner[k];
    return o && o->active && o->s[0] == (int32_t)(k + 1u);
}

/* ---------------------------------------------------------------- the parts' modulators --- */
typedef struct {
    int32_t val;                                     /* the modulator's value, Q15 (bipolar or 0..32767) */
    uint32_t ph;                                     /* TIDE / LOOP phase */
    uint8_t run, trig, mode;                         /* TIDE running, a note started this block, MOD last block */
    int32_t gstep;                                   /* GRID: the last 1/16 count seen */
} brd_part_t;
static brd_part_t brd_part[NPART];

/* 2^(x / 256) in Q16, x >= 0 (a 17-entry table of one octave, interpolated) */
static uint32_t brd_exp2(uint32_t x)
{
    static const uint32_t E[17] = {0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009,
                                   44682, 49562, 54658, 59979, 65536};
    uint32_t f = x & 255u, i = f >> 4, m = 65536u + E[i] + (((E[i + 1u] - E[i]) * (f & 15u)) >> 4);
    uint32_t o = x >> 8;
    return o >= 15u ? 0xFFFFFFFFu : m << o;
}

/* the slope: phase ph (Q32) through rise / fall (SLOPE: the rise's share) and the curve (SHAPE), Q15 0..32767 */
static int32_t brd_slope(uint32_t ph, int32_t shape, int32_t slope)
{
    int32_t p16 = (int32_t)(ph >> 16), r16 = clamp(slope, 1, 126) * 516, u, c;
    u = p16 < r16 ? p16 * 32767 / r16 : 32767 - (p16 - r16) * 32767 / (65536 - r16);
    c = shape - 64;
    if (c > 0)                                       /* towards u^2: slow start, fast end (expo) */
        u += ((((u * u) >> 15) - u) * c) / 63;
    else if (c < 0)                                  /* towards 1 - (1 - u)^2 (log) */
        u += (((32767 - (((32767 - u) * (32767 - u)) >> 15)) - u) * -c) / 64;
    return clamp(u, 0, 32767);
}

static int32_t brd_grid_level(uint32_t inst, uint32_t step, uint32_t x, uint32_t y)
{
    uint32_t i = x >> 6, j = y >> 6, off = inst * 32u + step, xb = (x << 2) & 255u, yb = (y << 2) & 255u;
    uint32_t a = GRIDS_NODE[GRIDS_MAP[i][j]][off], b = GRIDS_NODE[GRIDS_MAP[i + 1u][j]][off];
    uint32_t c = GRIDS_NODE[GRIDS_MAP[i][j + 1u]][off], d = GRIDS_NODE[GRIDS_MAP[i + 1u][j + 1u]][off];
    uint32_t ab = (a * (255u - xb) + b * xb) >> 8, cd = (c * (255u - xb) + d * xb) >> 8;
    return (int32_t)((ab * (255u - yb) + cd * yb) >> 8);
}

static void brd_block(track_t *t)
{
    uint32_t p = (uint32_t)(t - trk), i, gate = 0;
    brd_part_t *b;
    int32_t mod, p1, p2, p3;
    if (p >= NPART)
        return;
    b = &brd_part[p];
    mod = t->p[P_E4];
    p1 = t->p[P_E5];
    p2 = t->p[P_E6];
    p3 = t->p[P_E7];
    if (mod != b->mode) {                            /* a new modulator starts from rest */
        b->mode = (uint8_t)mod;
        b->val = 0;
        b->ph = 0;
        b->run = 0;
        b->gstep = -1;
        if (mod == BRM_PENV || mod == BRM_PLFO)
            mi_peaks_init(p);
    }
    for (i = 0; i < NVOICE; i++)
        gate |= t->v[i].gate;
    switch (mod) {
    case BRM_PENV:
        /* ATK DEC 0..127 -> Peaks' times 0..65535 (its release = DEC), SUS -> its level 0..32767 */
        b->val = mi_peaks_block(p, 0, (uint32_t)p1 * 516u, (uint32_t)p2 * 516u, (uint32_t)p3 * 258u, gate, CTL);
        break;
    case BRM_PLFO:
        b->val = mi_peaks_block(p, 1, (uint32_t)p1 * 516u, (uint32_t)p2 * 516u, 0, 0, CTL);
        break;
    case BRM_TIDE: {                                  /* 2 ms .. ~10 s: 88 * 2^(TIME * 12.3 / 127) samples */
        uint32_t len = (88u * (brd_exp2((uint32_t)p1 * 3150u / 127u) >> 8)) >> 8, inc;
        inc = 0xFFFFFFFFu / (len > CTL ? len : CTL) * CTL;   /* a block's worth */
        if (b->trig) {
            b->trig = 0;
            b->run = 1;
            b->ph = 0;
        }
        if (b->run) {
            if (0xFFFFFFFFu - b->ph < inc) {         /* past the end: rest at 0 */
                b->run = 0;
                b->ph = 0;
            } else {
                b->ph += inc;
            }
        }
        b->val = b->run ? brd_slope(b->ph, p2, p3) : 0;
        break;
    }
    case BRM_LOOP: {                                  /* 0.05 .. 100 Hz: 0.05 Hz = 4870 a sample (Q32 at 44.1 kHz) */
        uint32_t inc = (4870u * (brd_exp2((uint32_t)p1 * 2816u / 127u) >> 8)) >> 8;
        b->ph += inc * CTL;
        b->val = brd_slope(b->ph, p2, p3) * 2 - 32767;
        break;
    }
    case BRM_GBD:
    case BRM_GSD:
    case BRM_GHH: {
        int32_t c = seq_clock16();
        b->val = (b->val * 253) >> 8;                /* ~70 ms trigger envelopes */
        if (c >= 0 && c != b->gstep) {
            uint32_t step = (uint32_t)c & 31u;
            int32_t lvl = brd_grid_level((uint32_t)(mod - BRM_GBD), step, (uint32_t)p1 * 2u, (uint32_t)p2 * 2u);
            b->gstep = c;
            if (lvl > 255 - p3 * 2) {                /* Grids: a hit over the FILL threshold, accent above 192 */
                b->val = lvl > 192 ? 32767 : 22000;
                for (i = 0; i < MI_NOSC; i++)
                    if (brd_held(i) && brd_owner[i] >= t->v && brd_owner[i] < t->v + NVOICE)
                        mi_osc_strike(i);
            }
        } else if (c < 0) {
            b->gstep = -1;
        }
        break;
    }
    default:
        b->val = 0;
        break;
    }
}

/* ---------------------------------------------------------------- the voice --- */
static void brd_note_on(track_t *t, voice_t *v)
{
    uint32_t k = MI_NOSC, i, p = (uint32_t)(t - trk), old = 0xFFFFFFFFu;
    if (!brd_ready) {
        for (i = 0; i < MI_NOSC; i++)
            mi_osc_init(i);
        brd_ready = 1;
    }
    if (v->s[0] >= 1 && v->s[0] <= (int32_t)MI_NOSC && brd_owner[v->s[0] - 1] == v)
        k = (uint32_t)v->s[0] - 1u;                  /* a retrigger: its own */
    for (i = 0; k == MI_NOSC && i < MI_NOSC; i++)
        if (!brd_held(i))
            k = i;
    for (i = 0; k == MI_NOSC && i < MI_NOSC; i++)    /* all sounding: the oldest */
        if (brd_age[i] < old) {
            old = brd_age[i];
            k = i;
        }
    if (k >= MI_NOSC)
        k = 0;
    if (brd_owner[k] != v) {
        if (brd_owner[k] && brd_owner[k]->s[0] == (int32_t)(k + 1u))
            brd_owner[k]->s[0] = 0;                  /* the voice it was taken from: silent from now on */
        mi_osc_init(k);
        brd_owner[k] = v;
    }
    brd_age[k] = ++brd_clock;
    v->s[0] = (int32_t)(k + 1u);
    if (!v->active)
        v->s[1] = v->s[2] = 0;                       /* (a new voice: the blocker from rest) */
    mi_osc_shape(k, BRD_SHAPE[clamp(t->p[P_E0], 0, BRD_NMODEL - 1)]);
    mi_osc_strike(k);
    if (p < NPART)
        brd_part[p].trig = 1;                        /* TIDE starts */
}

static void brd_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t slot = v->s[0] - 1, pitch, timb, colr, depth = p[P_E3], modv = 0, x1 = v->s[1], y1 = v->s[2];
    uint32_t i = 0, k;
    int16_t buf[BRD_CHUNK];
    if (slot < 0 || slot >= (int32_t)MI_NOSC || brd_owner[slot] != v)
        return;                                      /* its oscillator was taken: silent until it ends */
    k = (uint32_t)(t - trk);
    if (k < NPART)
        modv = brd_part[k].val;
    pitch = m->pitch16 * 8 + BRD_PITCH0 + ((m->fine * 277) >> 9);   /* fine: 1/4096 of the increment */
    timb = (p[P_E1] << 8) + m->cutoff + ((modv * depth) >> 6);   /* DEPTH 64: the modulator's full range */
    colr = (p[P_E2] << 8) + m->shape - (64 << 8);
    timb = clamp(timb, 0, 127 << 8) * 258 >> 8;      /* 0..127 << 8 -> 0..32767 */
    colr = clamp(colr, 0, 127 << 8) * 258 >> 8;
    mi_osc_shape((uint32_t)slot, BRD_SHAPE[clamp(p[P_E0], 0, BRD_NMODEL - 1)]);
    while (i < n) {
        uint32_t c = n - i < BRD_CHUNK ? n - i : BRD_CHUNK, j;
        mi_osc_render((uint32_t)slot, pitch, timb, colr, buf, c);
        for (j = 0; j < c; j++) {                    /* DC blocker, ~27 Hz: y = x - x1 + (1 - 1/256) y1 (y Q8) */
            int32_t x = (buf[j] * BRD_GAIN) >> 8;
            y1 = shl(x - x1, 8) + y1 - (y1 >> 8);
            x1 = x;
            out[i + j] += voice_amp(y1 >> 8, m, i + j);
        }
        i += c;
    }
    v->s[1] = x1;
    v->s[2] = y1;
}

/* {MODEL, TIMB, COLR, DEPTH, MOD, P1, P2, P3} (MODEL: N_BRD_MODEL's index) */
static const preset_t BRAIDS_PRESETS[] = {
    {"CSAW", {0, 40, 70, 0, BRM_OFF, 64, 64, 64}, {2, 70, 100, 45}, 0, 0, FX(0, 20, 30, 30), PAT(4)},
    {"PLUCK", {28, 70, 64, 24, BRM_PENV, 0, 50, 0}, {0, 85, 0, 60}, 0, 0, FX(0, 10, 45, 35), PAT(3)},
    {"BELLS", {32, 64, 50, 0, BRM_OFF, 64, 64, 64}, {0, 95, 0, 90}, 0, 0, FX(0, 20, 30, 70), PAT(7)},
    {"VOWEL", {22, 50, 30, 40, BRM_LOOP, 40, 64, 64}, {25, 70, 110, 70}, 0, 0, FX(0, 40, 20, 60), PAT(5)},
    {"FM BASS", {25, 55, 40, 30, BRM_PENV, 0, 40, 0}, {0, 60, 60, 25}, 0, 1, FX(5, 0, 10, 10), PAT(2)},
    {"SWARM", {14, 80, 64, 20, BRM_PLFO, 20, 30, 0}, {70, 90, 115, 90}, 0, 0, FX(0, 50, 20, 70), PAT(5)},
    /* the VCA held open: the hits strike the drum, its own decay is the sound */
    {"GRID DRUM", {33, 60, 40, 50, BRM_GBD, 64, 64, 90}, {0, 0, 127, 40}, 0, 0, FX(0, 0, 20, 30), PAT(8)},
    {"CLOUD", {40, 64, 64, 30, BRM_TIDE, 90, 40, 20}, {40, 90, 100, 90}, 0, 0, FX(0, 30, 30, 80), PAT(5)},
};

static const engine_t ENG_BRAIDS = {
    .name = "BRAIDS",
    .page_title = {"OSC", "MOD"},
    .edit = {
        {"MODL", F_ENUM, 0, (int16_t)(NELEM(BRD_SHAPE) - 1u), 0, N_BRD_MODEL, 0},
        {"TIMB", F_PCT, 0, 127, 64, 0, 0},
        {"COLR", F_PCT, 0, 127, 64, 0, 0},
        {"DPTH", F_BIPCT, -64, 63, 0, 0, 0},         /* MOD -> TIMBRE */
        {"MOD", F_ENUM, 0, 7, 0, N_BRD_MOD, 0},
        {"RATE", F_PCT, 0, 127, 64, 0, 0},           /* P1..P3: their labels follow MOD (brd_desc) */
        {"SHAP", F_PCT, 0, 127, 64, 0, 0},
        {"SLOP", F_PCT, 0, 127, 64, 0, 0},
    },
    .presets = BRAIDS_PRESETS,
    .npresets = NELEM(BRAIDS_PRESETS),
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .poly = MI_NOSC,
    .keep = 0x07,                /* the oscillator slot, the DC blocker */
    .note_on = brd_note_on,
    .render = brd_render,
    .desc = brd_desc,
    .block = brd_block,
};
