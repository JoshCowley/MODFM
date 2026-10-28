/* SPDX-License-Identifier: GPL-3.0-only
 * FM1 Rack (a Felucca fork): the rack's two modulator slots per part, MOD A and MOD B.
 *
 * Each slot is a type and four values (track parameters P_MATYP P_MA1..P_MA4, P_MBTYP P_MB1..P_MB4: stored with the
 * project, set by the RACK, MOD A and MOD B pages). Their outputs are the matrix's sources MODA and MODB (mod.c), so
 * they reach every destination the matrix has: PITCH CUT SHP AMP, PAN, the sends, the LFO's RATE, VIB, E1..E8 (the
 * engine's values), DEPTH, VEL, TIME. MACRO's DPTH also takes MOD A into TIMBRE directly.
 *   ENV    Peaks' envelope (Mutable Instruments, MIT; peaks/modulations/multistage_envelope.cc), gated while a key of
 *          the part is held: ATK DEC SUS (REL = DEC), 0..1
 *   LFO    Peaks' LFO (lfo.cc), free-running: RATE SHAPE (Peaks' shape presets); -1..1
 *   SLOPE  an AD slope started by each note: TIME (2 ms .. 10 s), SHAPE (log 0 .. linear 64 .. expo 127), SLOPE (the
 *          rise's share); 0..1. Written here in integer C after Tides' AD mode (Tides' own code needs ~55 KB of tables)
 *   CYCLE  the same slope looping: RATE (0.05 .. 100 Hz), SHAPE, SLOPE; -1..1
 *   TRIG1 TRIG2 TRIG3  Grids' drum map (GPL-3.0; grids_map.h), one of its three lanes, stepped by the transport's 1/16
 *          (silent while stopped): X Y FILL CHAOS. CHAOS adds a random amount to each step's level, as Grids' own
 *          does. A hit is a trigger envelope (an accent a full one) and strikes the part's sounding voices (rack_strike:
 *          the struck and plucked models ring on the pattern)
 * State per part (rack_mod_block runs in the part's own block, on whichever core renders it: nothing shared). */

static int32_t seq_clock16(void);                     /* (seq.c) the transport's 1/16 count, -1 stopped */
static void rack_strike(track_t *t);                  /* (engines.c) the part's sounding voices struck */

enum { RM_OFF, RM_ENV, RM_LFO, RM_SLOPE, RM_CYCLE, RM_TRIG1, RM_TRIG2, RM_TRIG3, RM_N };
static const char *const N_RMOD[RM_N] = {"OFF", "ENV", "LFO", "SLOPE", "CYCLE", "TRIG1", "TRIG2", "TRIG3"};
#define RM_UNIPOLAR(m) ((m) == RM_ENV || (m) == RM_SLOPE || (m) >= RM_TRIG1)

/* the four values' labels by type: {P1, P2, P3, P4} */
#define RD(l) {l, F_PCT, 0, 127, 64, 0, 0}
static const param_desc_t RM_D[RM_N][4] = {
    {RD("-"), RD("-"), RD("-"), RD("-")},
    {RD("ATK"), RD("DEC"), RD("SUS"), RD("-")},
    {RD("RATE"), RD("SHAP"), RD("-"), RD("-")},
    {RD("TIME"), RD("SHAP"), RD("SLOP"), RD("-")},
    {RD("RATE"), RD("SHAP"), RD("SLOP"), RD("-")},
    {RD("X"), RD("Y"), RD("FILL"), RD("CHAO")},
    {RD("X"), RD("Y"), RD("FILL"), RD("CHAO")},
    {RD("X"), RD("Y"), RD("FILL"), RD("CHAO")},
};
#undef RD
/* the label of a slot value (params.c track_desc): id is one of P_MA1..P_MA4, P_MB1..P_MB4 */
static const param_desc_t *rack_mod_desc(const track_t *t, uint32_t id)
{
    uint32_t b = id >= P_MB1, k = id - (b ? P_MB1 : P_MA1);
    uint32_t m = (uint32_t)t->p[b ? P_MBTYP : P_MATYP];
    return &RM_D[m < RM_N ? m : 0][k & 3u];
}
static int rack_mod_param(uint32_t id) { return (id >= P_MA1 && id <= P_MA4) || (id >= P_MB1 && id <= P_MB4); }

typedef struct {
    int32_t val;                                      /* the output, Q15 */
    uint32_t ph;                                      /* SLOPE / CYCLE phase */
    int32_t gstep;                                    /* TRIG: the last 1/16 count seen */
    uint32_t rng;                                     /* TRIG: CHAOS's random numbers */
    uint8_t run, mode;                                /* SLOPE running; the type of the block before */
} rmod_t;
static rmod_t rmod[NPART][2];
static uint8_t rmod_trig[NPART];                      /* a note started (bit 0 A, 1 B): set at note-on, core 0 */

/* the output of slot k (0 A, 1 B) of part t, Q15 (matrix sources MODA MODB, mod.c) */
static int32_t rack_mod_val(const track_t *t, uint32_t k)
{
    uint32_t p = (uint32_t)(t - trk);
    return p < NPART ? rmod[p][k & 1u].val : 0;
}
static uint8_t rmod_note[NPART];                      /* the part's last note-on (XMOD's carrier), 0 = none yet */
/* a note-on on part t (voice.c trk_note_on, core 0): SLOPE starts; XMOD's carrier goes to the note */
static void rack_mod_note(const track_t *t, uint32_t note)
{
    uint32_t p = (uint32_t)(t - trk);
    if (p < NPART) {
        rmod_trig[p] = 3u;
        rmod_note[p] = (uint8_t)(note & 127u);
    }
}
/* part t's last note, MIDI note << 8 (C3 before the first) */
static int32_t rack_mod_lastnote(const track_t *t)
{
    uint32_t p = (uint32_t)(t - trk);
    return (p < NPART && rmod_note[p] ? rmod_note[p] : 48) << 8;
}

/* 2^(x / 256) in Q16, x >= 0 (one octave's table, interpolated) */
static uint32_t rm_exp2(uint32_t x)
{
    static const uint32_t E[17] = {0, 2902, 5932, 9096, 12400, 15850, 19454, 23216, 27146, 31249, 35534, 40009,
                                   44682, 49562, 54658, 59979, 65536};
    uint32_t f = x & 255u, i = f >> 4, m = 65536u + E[i] + (((E[i + 1u] - E[i]) * (f & 15u)) >> 4);
    uint32_t o = x >> 8;
    return o >= 15u ? 0xFFFFFFFFu : m << o;
}
/* the slope at phase ph (Q32): rise / fall (SLOPE, the rise's share) and the curve (SHAPE); Q15 0..32767 */
static int32_t rm_slope(uint32_t ph, int32_t shape, int32_t slope)
{
    int32_t p16 = (int32_t)(ph >> 16), r16 = clamp(slope, 1, 126) * 516, u, c;
    u = p16 < r16 ? p16 * 32767 / r16 : 32767 - (p16 - r16) * 32767 / (65536 - r16);
    c = shape - 64;
    if (c > 0)                                        /* towards u^2: slow start, fast end (expo) */
        u += ((((u * u) >> 15) - u) * c) / 63;
    else if (c < 0)                                   /* towards 1 - (1 - u)^2 (log) */
        u += (((32767 - (((32767 - u) * (32767 - u)) >> 15)) - u) * -c) / 64;
    return clamp(u, 0, 32767);
}
/* Grids' drum map: lane inst's level at step (0..31) for X Y 0..255 (grids/pattern_generator.cc ReadDrumMap) */
static int32_t rm_grid_level(uint32_t inst, uint32_t step, uint32_t x, uint32_t y)
{
    uint32_t i = x >> 6, j = y >> 6, off = inst * 32u + step, xb = (x << 2) & 255u, yb = (y << 2) & 255u;
    uint32_t a = GRIDS_NODE[GRIDS_MAP[i][j]][off], b = GRIDS_NODE[GRIDS_MAP[i + 1u][j]][off];
    uint32_t c = GRIDS_NODE[GRIDS_MAP[i][j + 1u]][off], d = GRIDS_NODE[GRIDS_MAP[i + 1u][j + 1u]][off];
    uint32_t ab = (a * (255u - xb) + b * xb) >> 8, cd = (c * (255u - xb) + d * xb) >> 8;
    return (int32_t)((ab * (255u - yb) + cd * yb) >> 8);
}

static void rm_slot(track_t *t, uint32_t p, uint32_t k, uint32_t gate, uint32_t trig)
{
    const int16_t *q = t->p + (k ? P_MBTYP : P_MATYP);
    rmod_t *r = &rmod[p][k];
    int32_t m = q[0], p1 = q[1], p2 = q[2], p3 = q[3], p4 = q[4];
    uint32_t peaks = p * 2u + k;                      /* (mi_glue.h MI_NPEAKS: two per part) */
    if (m != r->mode) {                               /* a new type starts from rest */
        r->mode = (uint8_t)m;
        r->val = 0;
        r->ph = 0;
        r->run = 0;
        r->gstep = -1;
        r->rng = 0x9E3779B9u * (peaks + 1u);
        if (m == RM_ENV || m == RM_LFO)
            mi_peaks_init(peaks);
    }
    switch (m) {
    case RM_ENV:
        r->val = mi_peaks_block(peaks, 0, (uint32_t)p1 * 516u, (uint32_t)p2 * 516u, (uint32_t)p3 * 258u, gate, CTL);
        break;
    case RM_LFO:
        r->val = mi_peaks_block(peaks, 1, (uint32_t)p1 * 516u, (uint32_t)p2 * 516u, 0, 0, CTL);
        break;
    case RM_SLOPE: {                                  /* 2 ms .. ~10 s: 88 * 2^(TIME * 12.3 / 127) samples */
        uint32_t len = (88u * (rm_exp2((uint32_t)p1 * 3150u / 127u) >> 8)) >> 8;
        uint32_t inc = 0xFFFFFFFFu / (len > CTL ? len : CTL) * CTL;   /* a block's worth */
        if (trig) {
            r->run = 1;
            r->ph = 0;
        }
        if (r->run) {
            if (0xFFFFFFFFu - r->ph < inc) {          /* past the end: rest at 0 */
                r->run = 0;
                r->ph = 0;
            } else {
                r->ph += inc;
            }
        }
        r->val = r->run ? rm_slope(r->ph, p2, p3) : 0;
        break;
    }
    case RM_CYCLE: {                                  /* 0.05 .. 100 Hz: 0.05 Hz = 4870 a sample (Q32 at 44.1 kHz) */
        uint32_t inc = (4870u * (rm_exp2((uint32_t)p1 * 2816u / 127u) >> 8)) >> 8;
        r->ph += inc * CTL;
        r->val = rm_slope(r->ph, p2, p3) * 2 - 32767;
        break;
    }
    case RM_TRIG1:
    case RM_TRIG2:
    case RM_TRIG3: {
        int32_t c = seq_clock16();
        r->val = (r->val * 253) >> 8;                 /* ~70 ms trigger envelopes */
        if (c >= 0 && c != r->gstep) {
            uint32_t step = (uint32_t)c & 31u;
            int32_t lvl = rm_grid_level((uint32_t)(m - RM_TRIG1), step, (uint32_t)p1 * 2u, (uint32_t)p2 * 2u);
            r->gstep = c;
            r->rng = r->rng * 1664525u + 1013904223u;
            lvl += (int32_t)(((r->rng >> 24) * (uint32_t)p4 * 2u) >> 10);   /* CHAOS: up to +63 (Grids: random /4) */
            if (lvl > 255)
                lvl = 255;
            if (lvl > 255 - p3 * 2) {                 /* a hit over the FILL threshold, an accent above 192 */
                r->val = lvl > 192 ? 32767 : 22000;
                rack_strike(t);
            }
        } else if (c < 0) {
            r->gstep = -1;
        }
        break;
    }
    default:
        r->val = 0;
        break;
    }
}

/* once per block of part t, before the matrix reads its sources (fx.c mix_part) */
static void rack_mod_block(track_t *t)
{
    uint32_t p = (uint32_t)(t - trk), i, gate = 0, tr;
    if (p >= NPART)
        return;
    for (i = 0; i < NVOICE; i++)
        gate |= t->v[i].gate;
    tr = rmod_trig[p];
    rmod_trig[p] = 0;
    rm_slot(t, p, 0, gate, tr & 1u);
    rm_slot(t, p, 1, gate, tr & 2u);
}
