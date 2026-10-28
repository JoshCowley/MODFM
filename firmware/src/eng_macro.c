/* SPDX-License-Identifier: GPL-3.0-only
 * FM1 Rack (a Felucca fork): MACRO, engine 14: Braids' macro oscillator (Mutable Instruments, Emilie Gillet, MIT),
 * 43 of its 47 models (the four wavetable models are left out with their 33 KB of tables).
 *
 * EDIT 1 (OSC): MODL TIMB COLR, DPTH (MOD A into TIMBRE). EDIT 2 (TONE): BITS RATE (the bit and sample-rate crusher of
 * Braids' own firmware), SIGN (its signature waveshaper), DRFT (a slow random wander of the pitch, per voice).
 * The track's FLT modulation (LFO DEST, ENV DEST, the matrix's CUT) moves TIMBRE, SHP moves COLOR. The rack's
 * modulators (rack_mod.c) reach any value through the matrix; the part's ADSR is the VCA (voice.c).
 *
 * Oscillators: MI_NOSC (4) for all parts' voices (17 KB each in the pool: mi_glue.cc). note_on (core 0,
 * events_block) gives a voice the slot it had, else a free one, else the oldest's; a voice whose slot was taken
 * renders nothing until it ends. One part plays at most 4 voices (engine_t.poly); two parts with 4 each share them.
 * State: s[0] the slot + 1; s[1] s[2] the DC blocker (Braids' jack is AC-coupled; SQSUB, VOSIM and others carry
 * DC); s[3] the drift, s[4] its random numbers; s[5] s[6] the sample hold (RATE); all kept over a retrigger.
 *
 * Rate: Braids was designed at 96 kHz; at Felucca's 44.1 kHz it is retuned (BRD_PITCH0, tests/rack_test.c). */
#include "../mi/mi_glue.h"
#include "grids_map.h"
#include "rack_mod.c"           /* MOD A, MOD B */

/* MODL -> braids::MacroOscillatorShape (settings.h order) */
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
_Static_assert(NELEM(BRD_SHAPE) == NELEM(N_BRD_MODEL), "MACRO: a name per model");
static const char *const N_BRD_BITS[] = {"16", "12", "8", "6", "4", "3", "2"};
static const uint8_t BRD_BITS[7] = {16, 12, 8, 6, 4, 3, 2};
static const char *const N_BRD_RATE[] = {"1/1", "1/2", "1/3", "1/6", "1/12"};
static const uint8_t BRD_HOLD[5] = {1, 2, 3, 6, 12};

/* Braids' pitch (1/128 semitone) of Felucca's pitch16 0 (MIDI note 0): Braids' tables are for 96 kHz, so at FS
 * the note is lower by 12 log2(96000 / FS) semitones, put back here (tests/rack_test.c: A4 = 440 Hz) */
#ifndef BRD_PITCH0
#define BRD_PITCH0 1724                      /* 12 x 128 x log2(96000 / 44100) = 1724.5 */
#endif
_Static_assert(FS == 44100, "MACRO: BRD_PITCH0 is for 44.1 kHz");
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

/* ---------------------------------------------------------------- the voice --- */
static void brd_note_on(track_t *t, voice_t *v)
{
    uint32_t k = MI_NOSC, i, old = 0xFFFFFFFFu;
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
    if (!v->active) {                                /* a new voice: the blocker, drift and hold from rest */
        v->s[1] = v->s[2] = v->s[3] = v->s[5] = v->s[6] = 0;
        v->s[4] = (int32_t)((v->note + 1u) * 0x9E3779B9u ^ v->age * 0x85EBCA6Bu);
    }
    mi_osc_shape(k, BRD_SHAPE[clamp(t->p[P_E0], 0, BRD_NMODEL - 1)]);
    mi_osc_strike(k);
}
/* the part's sounding oscillators struck again (rack_mod.c: a TRIG hit) */
static void brd_strike(const track_t *t)
{
    uint32_t i;
    for (i = 0; i < MI_NOSC; i++)
        if (brd_held(i) && brd_owner[i] >= t->v && brd_owner[i] < t->v + NVOICE)
            mi_osc_strike(i);
}

static void brd_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t slot = v->s[0] - 1, pitch, timb, colr, x1 = v->s[1], y1 = v->s[2], drift = v->s[3], held = v->s[6];
    int32_t sign = p[P_E6], drft = p[P_E7], cnt = v->s[5];
    uint32_t i = 0, bits = BRD_BITS[clamp(p[P_E4], 0, 6)], hold = BRD_HOLD[clamp(p[P_E5], 0, 4)];
    uint32_t mask = bits >= 16u ? 0xFFFFFFFFu : ~((1u << (16u - bits)) - 1u);
    int16_t buf[BRD_CHUNK];
    if (slot < 0 || slot >= (int32_t)MI_NOSC || brd_owner[slot] != v)
        return;                                      /* its oscillator was taken: silent until it ends */
    if (drft) {                                      /* DRFT: a random walk of the pitch, pulled back to 0 */
        int32_t r = (int32_t)noise32(&v->s[4]) >> 20;   /* (dsp.c: a step of -2048..2047) */
        drift += r - (drift >> 7);
    }
    pitch = m->pitch16 * 8 + BRD_PITCH0 + ((m->fine * 277) >> 9) + (((drift >> 4) * drft) >> 7);
    timb = (p[P_E1] << 8) + m->cutoff + ((rack_mod_val(t, 0) * p[P_E3]) >> 6);   /* DPTH 64: MOD A's full range */
    colr = (p[P_E2] << 8) + m->shape - (64 << 8);
    timb = clamp(timb, 0, 127 << 8) * 258 >> 8;      /* 0..127 << 8 -> 0..32767 */
    colr = clamp(colr, 0, 127 << 8) * 258 >> 8;
    mi_osc_shape((uint32_t)slot, BRD_SHAPE[clamp(p[P_E0], 0, BRD_NMODEL - 1)]);
    while (i < n) {
        uint32_t c = n - i < BRD_CHUNK ? n - i : BRD_CHUNK, j;
        mi_osc_render((uint32_t)slot, pitch, timb, colr, buf, c);
        for (j = 0; j < c; j++) {
            int32_t x;
            if (--cnt <= 0) {                        /* RATE: hold every n-th sample; BITS: fewer bits */
                cnt = (int32_t)hold;
                held = (int32_t)((uint32_t)(int32_t)buf[j] & mask);
            }
            x = (held * BRD_GAIN) >> 8;
            if (sign)                                /* SIGN: towards x |x| (Braids' signature shaper, x 1.6) */
                x += (((((x * (x < 0 ? -x : x)) >> 15) * 205 >> 7) - x) * sign) / 127;
            y1 = shl(x - x1, 8) + y1 - (y1 >> 8);    /* DC blocker, ~27 Hz: y = x - x1 + (1 - 1/256) y1 (y Q8) */
            x1 = x;
            out[i + j] += voice_amp(y1 >> 8, m, i + j);
        }
        i += c;
    }
    v->s[1] = x1;
    v->s[2] = y1;
    v->s[3] = drift;
    v->s[5] = cnt;
    v->s[6] = held;
}

/* {MODL, TIMB, COLR, DPTH, BITS, RATE, SIGN, DRFT} (MODL: N_BRD_MODEL's index) */
static const preset_t MACRO_PRESETS[] = {
    {"CSAW", {0, 40, 70, 0, 0, 0, 0, 10}, {2, 70, 100, 45}, 0, 0, FX(0, 20, 30, 30), PAT(4)},
    {"PLUCK", {28, 70, 64, 24, 0, 0, 0, 0}, {0, 85, 0, 60}, 0, 0, FX(0, 10, 45, 35), PAT(3)},
    {"BELLS", {32, 64, 50, 0, 0, 0, 0, 0}, {0, 95, 0, 90}, 0, 0, FX(0, 20, 30, 70), PAT(7)},
    {"VOWEL", {22, 50, 30, 40, 0, 0, 0, 20}, {25, 70, 110, 70}, 0, 0, FX(0, 40, 20, 60), PAT(5)},
    {"FM BASS", {25, 55, 40, 30, 0, 0, 20, 0}, {0, 60, 60, 25}, 0, 1, FX(5, 0, 10, 10), PAT(2)},
    {"SWARM", {14, 80, 64, 20, 0, 0, 0, 30}, {70, 90, 115, 90}, 0, 0, FX(0, 50, 20, 70), PAT(5)},
    /* the VCA held open: the hits strike the drum, its own decay is the sound */
    {"TRIG DRUM", {33, 60, 40, 50, 0, 0, 0, 0}, {0, 0, 127, 40}, 0, 0, FX(0, 0, 20, 30), PAT(8)},
    {"CLOUD", {40, 64, 64, 30, 0, 0, 0, 0}, {40, 90, 100, 90}, 0, 0, FX(0, 30, 30, 80), PAT(5)},
    {"LOFI", {9, 50, 50, 0, 3, 2, 40, 0}, {0, 70, 80, 40}, 0, 0, FX(10, 10, 30, 20), PAT(13)},
};
/* the rack slots each preset sets (rack_preset): MOD A {TYPE, 1..4}, MOD B {TYPE, 1..4}, then one matrix slot
 * {SRC, DST, AMT} (mod.c's MS_* / MD_* numbers: MODB 12; DST 13 = E2 COLR, 2 = CUT, 1 = PITCH) */
static const int8_t MACRO_RACK[][13] = {
    {RM_CYCLE, 30, 64, 64, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_ENV, 0, 50, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_OFF, 0, 0, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_CYCLE, 40, 64, 64, 0, RM_LFO, 20, 40, 0, 0, 12, 13, 30},
    {RM_ENV, 0, 40, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_LFO, 20, 30, 0, 0, RM_CYCLE, 10, 64, 64, 0, 12, 13, 25},
    {RM_TRIG1, 64, 64, 90, 10, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_SLOPE, 90, 40, 20, 0, RM_LFO, 15, 64, 0, 0, 12, 1, 4},
    {RM_OFF, 0, 0, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
};
_Static_assert(NELEM(MACRO_RACK) == NELEM(MACRO_PRESETS), "MACRO: a rack row per preset");

static const engine_t ENG_MACRO = {
    .name = "MACRO",
    .page_title = {"OSC", "TONE"},
    .edit = {
        {"MODL", F_ENUM, 0, (int16_t)(NELEM(BRD_SHAPE) - 1u), 0, N_BRD_MODEL, 0},
        {"TIMB", F_PCT, 0, 127, 64, 0, 0},
        {"COLR", F_PCT, 0, 127, 64, 0, 0},
        {"DPTH", F_BIPCT, -64, 63, 0, 0, 0},         /* MOD A -> TIMBRE */
        {"BITS", F_ENUM, 0, 6, 0, N_BRD_BITS, 0},
        {"RATE", F_ENUM, 0, 4, 0, N_BRD_RATE, 0},
        {"SIGN", F_PCT, 0, 127, 0, 0, 0},
        {"DRFT", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = MACRO_PRESETS,
    .npresets = NELEM(MACRO_PRESETS),
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .poly = MI_NOSC,
    .keep = 0x7F,                /* the slot, the blocker, the drift, the hold */
    .note_on = brd_note_on,
    .render = brd_render,
};
