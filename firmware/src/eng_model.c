/* SPDX-License-Identifier: GPL-3.0-only
 * FM1 Rack (a Felucca fork): MODEL, engine 15: Plaits' macro oscillator (Mutable Instruments, Emilie Gillet, MIT),
 * 21 of its 24 models (its wavetable model and two of the three DX7 banks are left out, for the flash).
 *
 * EDIT 1 (MODEL): ENGN HARM TIMB MRPH (Plaits' model and its three knobs). EDIT 2 (LPG): DCAY (the internal
 * envelope's and the low-pass gate's decay), LPG (OFF: the part's ADSR is the VCA, Plaits' gate held open; else
 * the gate's colour, pinged by each note: the drums and plucks decay by themselves), TENV (the internal envelope
 * into TIMBRE and MORPH, bipolar), AUX (out .. aux: Plaits' second output, each model's variant).
 * Each note is a trigger (the drums, the speech, the strings and the modal resonator strike on it); a TRIG hit of
 * MOD A / B strikes the sounding voices again (rack_strike). The track's FLT modulation moves TIMBRE, SHP moves
 * MORPH; the matrix reaches every value.
 *
 * Voices: MI_NMODEL (2) Plaits voices for all parts (27 KB each in the pool: mi_glue_fp.cc), handed out as MACRO's
 * oscillators are (eng_macro.c): a part plays at most 2 voices at once. State: s[0] the slot + 1; s[1] a strike
 * waiting for the next render.
 * Rate: Plaits is tuned for 48 kHz; the glue retunes it for 44.1 kHz (tests/rack_test.c: A4 = 440 Hz). Plaits'
 * float code runs on the first core only (fx.c rack_core0). */

/* ENGN -> Plaits' engine number (voice.cc's order) */
static const uint8_t MDL_ENGINE[] = {8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 0, 1, 2, 5, 6, 7};
static const char *const N_MDL_ENGINE[] = {"VA", "WSHP", "FM", "GRAIN", "ADDTV", "CHORD", "SPCH", "SWARM",
                                           "NOISE", "PRTCL", "STRNG", "MODAL", "BDRUM", "SDRUM", "HIHAT", "VAVCF",
                                           "PHDST", "6OP", "TERRN", "SMACH", "CHIP"};
#define MDL_NENGINE ((int16_t)NELEM(MDL_ENGINE))
_Static_assert(NELEM(MDL_ENGINE) == NELEM(N_MDL_ENGINE), "MODEL: a name per model");
#define MDL_GAIN 176                                 /* Q8: Plaits' full scale to Felucca's engine level (MACRO's) */
#define MDL_CHUNK 16u

static voice_t *mdl_owner[MI_NMODEL];
static uint32_t mdl_age[MI_NMODEL], mdl_clock;

static int mdl_held(uint32_t k)
{
    const voice_t *o = mdl_owner[k];
    return o && o->active && o->s[0] == (int32_t)(k + 1u);
}

static void mdl_note_on(track_t *t, voice_t *v)
{
    uint32_t k = MI_NMODEL, i, old = 0xFFFFFFFFu;
    (void)t;
    if (v->s[0] >= 1 && v->s[0] <= (int32_t)MI_NMODEL && mdl_owner[v->s[0] - 1] == v)
        k = (uint32_t)v->s[0] - 1u;                  /* a retrigger: its own */
    for (i = 0; k == MI_NMODEL && i < MI_NMODEL; i++)
        if (!mdl_held(i))
            k = i;
    for (i = 0; k == MI_NMODEL && i < MI_NMODEL; i++)
        if (mdl_age[i] < old) {
            old = mdl_age[i];
            k = i;
        }
    if (k >= MI_NMODEL)
        k = 0;
    if (mdl_owner[k] != v) {
        if (mdl_owner[k] && mdl_owner[k]->s[0] == (int32_t)(k + 1u))
            mdl_owner[k]->s[0] = 0;                  /* the voice it was taken from: silent from now on */
        mdl_owner[k] = v;                            /* (the voice keeps its state: a new note on it is a trigger) */
    }
    mdl_age[k] = ++mdl_clock;
    v->s[0] = (int32_t)(k + 1u);
    v->s[1] = 1;                                     /* the trigger */
}
static void mdl_strike(const track_t *t)
{
    uint32_t i;
    for (i = 0; i < MI_NMODEL; i++)
        if (mdl_held(i) && mdl_owner[i] >= t->v && mdl_owner[i] < t->v + NVOICE)
            mdl_owner[i]->s[1] = 1;
}

static inline uint16_t mdl_q15(int32_t x) { return (uint16_t)(clamp(x, 0, 127 << 8) * 258 >> 8); }

static void mdl_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    int32_t slot = v->s[0] - 1, aux = p[P_E7];
    uint32_t i = 0;
    mi_model_t q;
    int16_t o[MDL_CHUNK], a[MDL_CHUNK];
    if (slot < 0 || slot >= (int32_t)MI_NMODEL || mdl_owner[slot] != v)
        return;                                      /* its voice was taken: silent until it ends */
    q.note = m->pitch16 * 16 + (m->fine >> 4);
    q.engine = MDL_ENGINE[clamp(p[P_E0], 0, MDL_NENGINE - 1)];
    q.harmonics = mdl_q15(p[P_E1] << 8);
    q.timbre = mdl_q15((p[P_E2] << 8) + m->cutoff);
    q.morph = mdl_q15((p[P_E3] << 8) + m->shape - (64 << 8));
    q.decay = mdl_q15(p[P_E4] << 8);
    q.colour = mdl_q15(p[P_E5] << 8);
    q.lpg = p[P_E5] > 0;
    q.tenv = (int16_t)(clamp(p[P_E6], -64, 63) * 512);
    q.fenv = 0;
    q.gate = v->gate != 0;
    q.strike = v->s[1] != 0;
    v->s[1] = 0;
    while (i < n) {
        uint32_t c = n - i < MDL_CHUNK ? n - i : MDL_CHUNK, j;
        mi_model_render((uint32_t)slot, &q, o, a, c);
        q.strike = 0;
        for (j = 0; j < c; j++) {
            int32_t x = o[j] + (((a[j] - o[j]) * aux) >> 7);   /* AUX: out .. aux */
            out[i + j] += voice_amp((x * MDL_GAIN) >> 8, m, i + j);
        }
        i += c;
    }
}

/* {ENGN, HARM, TIMB, MRPH, DCAY, LPG, TENV, AUX} (ENGN: N_MDL_ENGINE's index) */
static const preset_t MODEL_PRESETS[] = {
    {"VA PAD", {0, 50, 60, 64, 64, 0, 0, 0}, {60, 90, 110, 80}, 0, 0, FX(0, 40, 20, 70), PAT(5)},
    {"FM KEYS", {2, 40, 70, 40, 60, 0, 20, 0}, {0, 70, 70, 40}, 0, 0, FX(0, 20, 30, 40), PAT(7)},
    {"CHORDS", {5, 64, 50, 60, 64, 0, 0, 0}, {20, 80, 100, 60}, 0, 0, FX(0, 30, 30, 60), PAT(5)},
    {"SPEECH", {6, 40, 64, 64, 50, 40, 0, 0}, {0, 0, 127, 30}, 0, 0, FX(0, 0, 30, 30), PAT(3)},
    {"STRING", {10, 64, 60, 50, 70, 60, 0, 0}, {0, 0, 127, 50}, 0, 0, FX(0, 0, 40, 50), PAT(3)},
    {"MODAL", {11, 50, 40, 60, 80, 70, 0, 0}, {0, 0, 127, 70}, 0, 0, FX(0, 20, 30, 70), PAT(7)},
    {"KICK", {12, 40, 64, 40, 60, 0, 0, 0}, {0, 0, 127, 20}, 0, 1, FX(0, 0, 0, 10), PAT(8)},
    {"6OP BELL", {17, 30, 64, 64, 64, 50, 10, 0}, {0, 0, 127, 70}, 0, 0, FX(0, 20, 30, 70), PAT(7)},
    {"TERRAIN", {18, 40, 64, 40, 64, 0, 30, 0}, {30, 90, 110, 70}, 0, 0, FX(0, 30, 30, 60), PAT(5)},
};
/* the rack slots each preset sets (rack_preset; MACRO_RACK's layout) */
static const int8_t MODEL_RACK[][13] = {
    {RM_CYCLE, 25, 64, 64, 0, RM_LFO, 15, 40, 0, 0, 12, 14, 30},   /* MOD B -> E3 MRPH */
    {RM_ENV, 0, 40, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_LFO, 20, 30, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_TRIG2, 64, 40, 70, 20, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_TRIG3, 40, 80, 80, 10, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_SLOPE, 70, 40, 20, 0, RM_CYCLE, 10, 64, 64, 0, 12, 14, 25},
    {RM_TRIG1, 64, 64, 90, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_OFF, 0, 0, 0, 0, RM_OFF, 0, 0, 0, 0, 0, 0, 0},
    {RM_CYCLE, 15, 64, 64, 0, RM_LFO, 10, 64, 0, 0, 12, 14, 40},
};
_Static_assert(NELEM(MODEL_RACK) == NELEM(MODEL_PRESETS), "MODEL: a rack row per preset");

static const engine_t ENG_MODEL = {
    .name = "MODEL",
    .page_title = {"MODEL", "LPG"},
    .edit = {
        {"ENGN", F_ENUM, 0, (int16_t)(NELEM(MDL_ENGINE) - 1u), 0, N_MDL_ENGINE, 0},
        {"HARM", F_PCT, 0, 127, 64, 0, 0},
        {"TIMB", F_PCT, 0, 127, 64, 0, 0},
        {"MRPH", F_PCT, 0, 127, 64, 0, 0},
        {"DCAY", F_PCT, 0, 127, 64, 0, 0},
        {"LPG", F_PCT, 0, 127, 0, 0, 0},             /* 0: OFF (the ADSR); else the gate's colour */
        {"TENV", F_BIPCT, -64, 63, 0, 0, 0},
        {"AUX", F_PCT, 0, 127, 0, 0, 0},
    },
    .presets = MODEL_PRESETS,
    .npresets = NELEM(MODEL_PRESETS),
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .poly = MI_NMODEL,
    .keep = 0x03,
    .note_on = mdl_note_on,
    .render = mdl_render,
};
