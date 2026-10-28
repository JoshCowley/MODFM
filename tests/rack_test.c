/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1 Rack: the rack on the host, through hostsim.c with FELUCCA_RACK=1 (firmware/src/eng_macro.c, eng_model.c,
 * rack_mod.c, fx.c's XMOD and GRAIN, project.c's rack store; firmware/mi). tests/run_tests.sh builds it with the C++
 * objects (tools/build_mi_host.sh).
 *   maths        firmware/mi/mi_math.cc against the host's double libm over the ranges the DSP uses
 *   MACRO        A4 = 440 Hz (0.5 %) on CSAW, MORPH, FOLD, SAW3, COMB, and A2 A5 on CSAW; each of its 43 models
 *                plays (not silent, under full scale, no DC, ends)
 *   MODEL        A4 = 440 Hz on FM and WSHP; each of its 21 models plays and ends; LPG decays by itself; a TRIG hit
 *                strikes a held voice; two parts, never two voices on one Plaits voice
 *   MOD A / B    ENV rises while held and falls after; LFO and CYCLE swing both ways; SLOPE runs once per note;
 *                TRIG hits only while the transport runs, fewer with less FILL, none at FILL 0; CHAOS adds hits
 *   XMOD         each algorithm changes the sound, stays under full scale; MIX 0 is the dry sound bit for bit; the
 *                part stays on the first core
 *   GRAIN        each mode takes the send, returns a tail after the notes, stays under full scale
 *   presets      every factory preset of MACRO and MODEL plays and ends, with its rack slots set
 *   projects     a rack project packs and unpacks with the rack's slots, GRAIN's values and MODEL
 * Exit 0 when everything passes. WAV=1: rack_<preset>.wav of every preset (listening). */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
#include "../firmware/src/project.c"

_Static_assert(FELUCCA_RACK, "build with -DFELUCCA_RACK=1");

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int16_t buf[FS * 4 * 2];

static void dry(track_t *t)
{
    t->p[P_CHOR] = t->p[P_DLY] = t->p[P_REV] = t->p[P_DIST] = 0;
    t->p[P_ITYPE] = 0;
}

static void reset_all(void)
{
    uint32_t k, i;
    int32_t o[2 * CTL];
    static int once;
    if (!once++)
        host_tracks_init();                          /* (a tempo before the first block) */
    fm1_in.notes = 0;                                /* the keys up (the key scan sees a change), the transport off */
    if (song.playing)
        transport_req = 2;
    mix_block(o, CTL);
    host_tracks_init();
    for (k = 0; k < NPART; k++) {
        for (i = 0; i < NVOICE; i++)
            memset(&trk[k].v[i], 0, sizeof trk[k].v[i]);
        dry(&trk[k]);
        trk[k].p[P_MATYP] = trk[k].p[P_MBTYP] = RM_OFF;
    }
    memset(rmod, 0, sizeof rmod);
    memset(rmod_trig, 0, sizeof rmod_trig);
    memset(brd_owner, 0, sizeof brd_owner);
    memset(mdl_owner, 0, sizeof mdl_owner);
    fm1_in.notes = 0;
}

static void set_engine(track_t *t, uint32_t e, int32_t model)
{
    host_preset(t, e, 0);
    t->p[P_E0] = (int16_t)model;
    t->p[P_E1] = 64;
    t->p[P_E2] = 64;
    t->p[P_E3] = e == ENGI_MACRO ? 0 : 64;
    t->p[P_E4] = e == ENGI_MACRO ? 0 : 64;
    t->p[P_E5] = 0;
    t->p[P_E6] = 0;
    t->p[P_E7] = 0;
    t->p[P_ATK] = 0;
    t->p[P_DEC] = 64;
    t->p[P_SUS] = 127;
    t->p[P_REL] = 10;
    t->p[P_VOICE] = V_POLY;
    t->p[P_MATYP] = t->p[P_MBTYP] = RM_OFF;
    t->p[P_M1SRC] = 0;
    dry(t);
}

/* frames of the mix (left) with keys held for `on` frames, then released */
static uint32_t render(uint32_t keys, uint32_t on, uint32_t frames)
{
    uint32_t f, i;
    for (f = 0; f < frames; f += CTL) {
        int32_t o[2 * CTL];
        fm1_in.notes = f < on ? keys : 0u;
        mix_block(o, CTL);
        for (i = 0; i < CTL && f + i < frames; i++)
            buf[f + i] = (int16_t)(o[2 * i] > 32767 ? 32767 : o[2 * i] < -32768 ? -32768 : o[2 * i]);
    }
    return frames;
}

/* the fundamental of buf[a..b), expected near f0 (within 25 %): the largest autocorrelation in that range of lags,
 * refined by parabolic interpolation */
static double pitch_of(uint32_t a, uint32_t b, double f0)
{
    uint32_t lag, best = 0, lmin = (uint32_t)(FS / (f0 * 1.25)), lmax = (uint32_t)(FS / (f0 * 0.8)) + 1u, i;
    double bv = -1e30, c[3];
    static double r[4096];
    for (lag = lmin - 1u; lag <= lmax + 1u && lag < 4096; lag++) {
        double s = 0;
        for (i = a; i + lag < b; i++)
            s += (double)buf[i] * buf[i + lag];
        r[lag] = s / (double)(b - a - lag);
    }
    for (lag = lmin; lag <= lmax && lag < 4095; lag++)
        if (r[lag] > bv) {
            bv = r[lag];
            best = lag;
        }
    if (!best || best == lmin || best == lmax)
        return 0;
    c[0] = r[best - 1];
    c[1] = bv;
    c[2] = r[best + 1];
    {
        double d = c[0] - 2 * c[1] + c[2], off = d != 0 ? 0.5 * (c[0] - c[2]) / d : 0;
        return FS / (best + off);
    }
}

static void stats(uint32_t a, uint32_t b, double *rms, int32_t *peak, double *dc)
{
    double s = 0, m = 0;
    int32_t pk = 0;
    uint32_t i;
    for (i = a; i < b; i++) {
        int32_t v = buf[i];
        s += (double)v * v;
        m += v;
        if (v > pk) pk = v;
        if (-v > pk) pk = -v;
    }
    *rms = sqrt(s / (b - a));
    *peak = pk;
    *dc = m / (b - a);
}
static double rms_of(uint32_t a, uint32_t b)
{
    double r, dc;
    int32_t pk;
    stats(a, b, &r, &pk, &dc);
    return r;
}

static uint32_t busy(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].active;
    return n;
}

/* keys: key 7 = C4 (song.octave 0); A4 = key 16 */
#define KEY(k) (1u << (k))

/* ---------------------------------------------------------------- maths */
static void test_math(void)
{
    double e_sin = 0, e_cos = 0, e_exp = 0, e_log = 0, e_sqrt = 0, e_pow = 0, e_tanh = 0, e_atan = 0, e_fl = 0;
    int i;
    for (i = 0; i < 200000; i++) {
        float x = (float)((i - 100000) * 0.0005);    /* -50 .. 50 */
        float u = (float)(i * 0.00005 + 1e-4);       /* 0 .. 10 */
        volatile float vx = x, vu = u;               /* (no constant folding by the host compiler) */
        double d;
        d = fabs(sinf(vx) - sin(x));
        if (d > e_sin) e_sin = d;
        d = fabs(cosf(vx) - cos(x));
        if (d > e_cos) e_cos = d;
        if (x > -40 && x < 40) {
            d = fabs(expf(vx) / exp(x) - 1.0);
            if (d > e_exp) e_exp = d;
        }
        d = fabs(logf(vu) - log(u));
        if (d > e_log) e_log = d;
        d = fabs(sqrtf(vu) / sqrt(u) - 1.0);
        if (d > e_sqrt) e_sqrt = d;
        d = fabs(powf(vu, 0.37f) / pow(u, 0.37f) - 1.0);
        if (d > e_pow) e_pow = d;
        d = fabs(tanhf(vx * 0.1f) - tanh(x * 0.1f));
        if (d > e_tanh) e_tanh = d;
        d = fabs(atanf(vx) - atan(x));
        if (d > e_atan) e_atan = d;
        d = fabs(floorf(vx * 3.3f) - floor((double)(x * 3.3f)));
        if (d > e_fl) e_fl = d;
    }
    printf("maths: sin %.1e cos %.1e exp %.1e (rel) log %.1e sqrt %.1e (rel) pow %.1e (rel) tanh %.1e atan %.1e "
           "floor %.0f\n", e_sin, e_cos, e_exp, e_log, e_sqrt, e_pow, e_tanh, e_atan, e_fl);
    CHECK(e_sin < 2e-5 && e_cos < 2e-5, "sinf / cosf off by %.1e / %.1e", e_sin, e_cos);
    CHECK(e_exp < 2e-6 && e_pow < 4e-6, "expf / powf off by %.1e / %.1e", e_exp, e_pow);
    CHECK(e_log < 2e-6 && e_sqrt < 2e-7, "logf / sqrtf off by %.1e / %.1e", e_log, e_sqrt);
    CHECK(e_tanh < 2e-6 && e_atan < 2e-6 && e_fl == 0, "tanhf / atanf / floorf off");
}

/* ---------------------------------------------------------------- MACRO */
static void test_macro(void)
{
    static const int32_t M[] = {0, 1, 3, 9, 15};
    static const char *const NM[] = {"CSAW", "MORPH", "FOLD", "SAW3", "COMB"};
    uint32_t k;
    int32_t m, pk;
    double lo = 1e30, hi = 0, dc;
    for (k = 0; k < NELEM(M); k++) {
        double hz;
        reset_all();
        set_engine(&trk[0], ENGI_MACRO, M[k]);
        render(KEY(16), FS, FS);
        hz = pitch_of(FS / 4, FS * 3 / 4, 440.0);
        printf("MACRO tuning %-5s A4: %.2f Hz\n", NM[k], hz);
        CHECK(fabs(hz / 440.0 - 1.0) < 0.005, "MACRO %s A4 at %.2f Hz, not 440", NM[k], hz);
    }
    {
        static const int8_t OC[2] = {-2, 1};
        static const double HZ[2] = {110.0, 880.0};
        for (k = 0; k < 2; k++) {
            double hz;
            reset_all();
            set_engine(&trk[0], ENGI_MACRO, 0);
            song.octave = OC[k];
            render(KEY(16), FS, FS);
            hz = pitch_of(FS / 4, FS * 3 / 4, HZ[k]);
            printf("MACRO tuning CSAW %.0f Hz: %.2f Hz\n", HZ[k], hz);
            CHECK(fabs(hz / HZ[k] - 1.0) < 0.005, "MACRO CSAW %.0f at %.2f Hz", HZ[k], hz);
            song.octave = 0;
        }
    }
    for (m = 0; m < BRD_NMODEL; m++) {
        double rms;
        reset_all();
        set_engine(&trk[0], ENGI_MACRO, m);
        render(KEY(7), FS / 2, FS * 3 / 2);
        stats(0, FS / 2, &rms, &pk, &dc);
        if (rms < lo) lo = rms;
        if (rms > hi) hi = rms;
        if (getenv("VERBOSE"))
            printf("MACRO %2d %-5s rms %7.1f peak %5d dc %6.1f\n", m, N_BRD_MODEL[m], rms, pk, dc);
        CHECK(rms > 150, "MACRO %s silent (rms %.1f)", N_BRD_MODEL[m], rms);
        CHECK(pk < 30000, "MACRO %s near full scale (%d)", N_BRD_MODEL[m], pk);
        CHECK(fabs(dc) < 400, "MACRO %s DC %.1f", N_BRD_MODEL[m], dc);
        CHECK(busy(&trk[0]) == 0, "MACRO %s: a voice still active 1 s after the release", N_BRD_MODEL[m]);
    }
    printf("MACRO: %d models render; rms %.0f .. %.0f\n", BRD_NMODEL, lo, hi);
}

/* ---------------------------------------------------------------- MODEL */
static int32_t mdl_index(int32_t plaits)                /* ENGN of a Plaits engine number */
{
    int32_t i;
    for (i = 0; i < MDL_NENGINE; i++)
        if (MDL_ENGINE[i] == plaits)
            return i;
    return 0;
}
static void test_model(void)
{
    static const int32_t PE[] = {10, 9};                 /* FM (TIMB 0: a sine), WSHP */
    static const char *const NM[] = {"FM", "WSHP"};
    uint32_t k, f, bad = 0, i, j;
    int32_t m, pk;
    double lo = 1e30, hi = 0, dc, rms;
    for (k = 0; k < NELEM(PE); k++) {
        double hz;
        reset_all();
        set_engine(&trk[0], ENGI_MODEL, mdl_index(PE[k]));
        trk[0].p[P_E2] = 0;
        trk[0].p[P_E1] = PE[k] == 10 ? 32 : 0;            /* FM: ratio 1 */
        render(KEY(16), FS, FS);
        hz = pitch_of(FS / 4, FS * 3 / 4, 440.0);
        printf("MODEL tuning %-3s A4: %.2f Hz\n", NM[k], hz);
        CHECK(fabs(hz / 440.0 - 1.0) < 0.005, "MODEL %s A4 at %.2f Hz, not 440", NM[k], hz);
    }
    for (m = 0; m < MDL_NENGINE; m++) {
        reset_all();
        set_engine(&trk[0], ENGI_MODEL, m);
        render(KEY(7), FS / 2, FS * 3 / 2);
        stats(0, FS / 2, &rms, &pk, &dc);
        if (rms < lo) lo = rms;
        if (rms > hi) hi = rms;
        if (getenv("VERBOSE"))
            printf("MODEL %2d %-6s rms %7.1f peak %5d dc %6.1f\n", m, N_MDL_ENGINE[m], rms, pk, dc);
        CHECK(rms > 100, "MODEL %s silent (rms %.1f)", N_MDL_ENGINE[m], rms);
        CHECK(pk < 32000, "MODEL %s at full scale (%d)", N_MDL_ENGINE[m], pk);
        CHECK(fabs(dc) < 600, "MODEL %s DC %.1f", N_MDL_ENGINE[m], dc);
        CHECK(busy(&trk[0]) == 0, "MODEL %s: a voice still active 1 s after the release", N_MDL_ENGINE[m]);
    }
    printf("MODEL: %d models render; rms %.0f .. %.0f\n", MDL_NENGINE, lo, hi);
    /* LPG: a held note decays by itself (STRNG, LPG on, DCAY short) */
    reset_all();
    set_engine(&trk[0], ENGI_MODEL, mdl_index(19));
    trk[0].p[P_E4] = 30;
    trk[0].p[P_E5] = 64;
    render(KEY(7), FS * 2, FS * 2);
    {
        double a = rms_of(0, FS / 4), b = rms_of(FS * 3 / 2, FS * 2);
        printf("MODEL LPG: rms %.0f at the start, %.0f after 1.5 s held\n", a, b);
        CHECK(a > 300 && b < a / 8, "MODEL LPG: %.0f -> %.0f (no decay)", a, b);
        /* TRIG1 hits with the transport (FILL full) strike it again */
        reset_all();
        set_engine(&trk[0], ENGI_MODEL, mdl_index(19));
        trk[0].p[P_E4] = 30;
        trk[0].p[P_E5] = 64;
        trk[0].p[P_MATYP] = RM_TRIG1;
        trk[0].p[P_MA1] = 64;
        trk[0].p[P_MA2] = 64;
        trk[0].p[P_MA3] = 127;
        trk[0].p[P_SLEN] = 16;
        transport_req = 1;
        render(KEY(7), FS * 2, FS * 2);
        transport_req = 2;
        b = rms_of(FS * 3 / 2, FS * 2);
        printf("MODEL strikes: rms %.0f after 1.5 s held with TRIG1 hits\n", b);
        CHECK(b > a / 6, "MODEL: TRIG hits did not strike (rms %.0f)", b);
        render(0, 0, FS);
    }
    reset_all();
    set_engine(&trk[0], ENGI_MODEL, 0);
    set_engine(&trk[1], ENGI_MODEL, mdl_index(20));
    CHECK((rack_core0() & 3u) == 3u, "MODEL parts are not kept on the first core");
    for (f = 0; f < FS; f += CTL) {
        int32_t o[2 * CTL];
        fm1_in.notes = f < FS / 2 ? KEY(7) | KEY(11) | KEY(14) : 0u;
        if (f == FS / 10 / CTL * CTL)
            for (k = 0; k < 2; k++)
                trk_note_on(&trk[1], 48 + k * 4, 100);
        if (f == FS / 2 / CTL * CTL)
            for (k = 0; k < 2; k++)
                trk_note_off(&trk[1], 48 + k * 4);
        mix_block(o, CTL);
        for (i = 0; i < MI_NMODEL; i++)
            for (j = i + 1; j < MI_NMODEL; j++)
                if (mdl_held(i) && mdl_held(j) && mdl_owner[i] == mdl_owner[j])
                    bad++;
        for (k = 0; k < 2; k++)
            for (i = 0; i < NVOICE; i++) {
                const voice_t *v = &trk[k].v[i];
                if (v->active && v->s[0] && mdl_owner[v->s[0] - 1] != v)
                    bad++;
            }
    }
    render(0, 0, FS * 2);
    printf("MODEL two parts: %u slot faults; voices left %u + %u\n", bad, busy(&trk[0]), busy(&trk[1]));
    CHECK(bad == 0, "MODEL: %u slot faults", bad);
    CHECK(busy(&trk[0]) + busy(&trk[1]) == 0, "MODEL: voices still active after the release");
}

/* ---------------------------------------------------------------- MOD A / B */
static void test_mods(void)
{
    uint32_t f, b;
    int32_t mn, mx, first = 0, held, after, h64 = 0;
    track_t *t = &trk[0];
    reset_all();
    set_engine(t, ENGI_MACRO, 0);
    t->p[P_MATYP] = RM_ENV;
    t->p[P_MA1] = 10;
    t->p[P_MA2] = 60;
    t->p[P_MA3] = 64;
    render(KEY(7), FS / 2, FS / 2);
    held = rmod[0][0].val;
    render(0, 0, FS);
    after = rmod[0][0].val;
    printf("ENV: %d held (SUS 64), %d 1 s after\n", held, after);
    CHECK(held > 12000 && held < 22000, "ENV held at %d, not near SUS 64", held);
    CHECK(after < 500, "ENV %d after the release", after);
    for (b = 0; b < 2; b++) {                         /* LFO (MOD A), CYCLE (MOD B): both ways */
        uint32_t base = b ? P_MB1 : P_MA1;
        reset_all();
        set_engine(t, ENGI_MACRO, 0);
        t->p[b ? P_MBTYP : P_MATYP] = b ? RM_CYCLE : RM_LFO;
        t->p[base] = 70;
        t->p[base + 1u] = 64;
        t->p[base + 2u] = 64;
        mn = 32767;
        mx = -32768;
        for (f = 0; f < 2 * FS; f += CTL) {
            int32_t o[2 * CTL];
            fm1_in.notes = KEY(7);
            mix_block(o, CTL);
            if (rmod[0][b].val < mn) mn = rmod[0][b].val;
            if (rmod[0][b].val > mx) mx = rmod[0][b].val;
        }
        printf("%s: %d .. %d\n", b ? "CYCLE (B)" : "LFO (A)", mn, mx);
        CHECK(mn < -20000 && mx > 20000, "%s swings %d .. %d", b ? "CYCLE" : "LFO", mn, mx);
    }
    reset_all();
    set_engine(t, ENGI_MACRO, 0);
    t->p[P_MATYP] = RM_SLOPE;
    t->p[P_MA1] = 60;
    t->p[P_MA2] = 64;
    t->p[P_MA3] = 20;
    mx = 0;
    for (f = 0; f < FS; f += CTL) {
        int32_t o[2 * CTL];
        fm1_in.notes = f < FS / 10 ? KEY(7) : 0u;
        mix_block(o, CTL);
        if (rmod[0][0].val > mx) mx = rmod[0][0].val;
        if (f == FS / 2) first = rmod[0][0].val;
    }
    printf("SLOPE: peak %d, %d at 0.5 s\n", mx, first);
    CHECK(mx > 30000, "SLOPE peaks at %d", mx);
    CHECK(first == 0, "SLOPE still at %d 0.5 s after its note", first);
    for (b = 0; b < 5; b++) {                         /* TRIG1: FILL 127, 64, 0 playing; stopped; FILL 64 + CHAOS */
        static const int16_t FILL[5] = {127, 64, 0, 127, 64};
        uint32_t hits = 0, play = b != 3;
        int32_t prev = 0;
        reset_all();
        set_engine(t, ENGI_MACRO, 34);
        t->p[P_MATYP] = RM_TRIG1;
        t->p[P_MA1] = 64;
        t->p[P_MA2] = 64;
        t->p[P_MA3] = FILL[b];
        t->p[P_MA4] = b == 4 ? 127 : 0;
        t->p[P_SLEN] = 16;
        transport_req = play ? 1u : 0u;
        for (f = 0; f < 4 * FS; f += CTL) {
            int32_t o[2 * CTL];
            fm1_in.notes = KEY(7);
            mix_block(o, CTL);
            if (rmod[0][0].val > prev + 10000) hits++;
            prev = rmod[0][0].val;
        }
        if (play) {
            transport_req = 2;
            render(0, 0, CTL);
        }
        printf("TRIG1 FILL %d%s %s: %u hits in 4 s\n", FILL[b], b == 4 ? " CHAOS 127" : "", play ? "playing" : "stopped",
               hits);
        if (b == 0) { CHECK(hits >= 8, "TRIG FILL 127: %u hits", hits); first = (int32_t)hits; }
        if (b == 1) { CHECK(hits > 0 && (int32_t)hits <= first, "TRIG FILL 64: %u hits (127: %d)", hits, first); h64 = (int32_t)hits; }
        if (b == 2) CHECK(hits == 0, "TRIG FILL 0: %u hits", hits);
        if (b == 3) CHECK(hits == 0, "TRIG stopped: %u hits", hits);
        if (b == 4) CHECK((int32_t)hits >= h64, "CHAOS took hits away (%u, %d without)", hits, h64);
    }
}

/* ---------------------------------------------------------------- XMOD */
static void test_xmod(void)
{
    uint32_t a, i;
    double ref, r, dc;
    int32_t pk;
    static int16_t dry_buf[FS / 2];
    reset_all();
    set_engine(&trk[0], ENGI_MACRO, 0);
    render(KEY(7), FS / 2, FS / 2);
    memcpy(dry_buf, buf, sizeof dry_buf);
    ref = rms_of(0, FS / 2);
    reset_all();                                     /* MIX 0: the INSERT does not run (fx.c track_insert) */
    set_engine(&trk[0], ENGI_MACRO, 0);
    trk[0].p[P_ITYPE] = IT_XMOD;
    trk[0].p[P_IMIX] = 0;
    {
        uint32_t f, ran = 0;
        for (f = 0; f < FS / 2; f += CTL) {
            int32_t o[2 * CTL];
            fm1_in.notes = KEY(7);
            mix_block(o, CTL);
            ran += trk[0].ins_run || ins[0].w;
        }
        printf("XMOD MIX 0: the insert ran in %u blocks\n", ran);
        CHECK(ran == 0, "XMOD at MIX 0 ran (%u blocks)", ran);
    }
    for (a = 0; a < 6; a++) {
        double diff = 0;
        reset_all();
        set_engine(&trk[0], ENGI_MACRO, 0);
        trk[0].p[P_ITYPE] = IT_XMOD;
        trk[0].p[P_IMIX] = 127;
        trk[0].p[P_IA] = (int16_t)(a * 127 / 5);
        trk[0].p[P_IB] = 80;
        trk[0].p[P_IC] = (int16_t)(a % 4);
        render(KEY(7), FS / 2, FS / 2);
        stats(FS / 10, FS / 2, &r, &pk, &dc);
        for (i = FS / 10; i < FS / 2; i++)
            diff += fabs((double)buf[i] - dry_buf[i]);
        diff /= FS / 2 - FS / 10;
        printf("XMOD ALGO %3d CARR %s: rms %6.0f (dry %6.0f) peak %5d, differs by %.0f\n", trk[0].p[P_IA],
               N_XCARR[a % 4], r, ref, pk, diff);
        CHECK(r > 100 && pk < 32767, "XMOD ALGO %d: rms %.0f peak %d", trk[0].p[P_IA], r, pk);
        CHECK(diff > 50, "XMOD ALGO %d did nothing (%.0f)", trk[0].p[P_IA], diff);
        CHECK((rack_core0() & 1u) != 0, "an XMOD part is not kept on the first core");
    }
}

/* ---------------------------------------------------------------- GRAIN */
static void grain_setup(uint32_t md, int16_t send)
{
    reset_all();
    set_engine(&trk[0], ENGI_MACRO, 28);              /* PLUCK */
    trk[0].p[P_REV] = send;
    song.g[G_RTYPE] = (int16_t)md;
    song.g[G_DRLVL] = 20;                              /* POS: recent */
    song.g[G_RSIZE] = 70;
    song.g[G_DRREV] = 100;                             /* DENS: many grains */
    song.g[G_RDAMP] = 64;
    song.g[G_ENGGO] = 20;
    song.g[G_SYNC] = 30;
    song.g[G_MIDI] = 0;
}
static void test_grain(void)
{
    uint32_t md, i, f;
    static int16_t none_buf[FS * 2];
    for (md = 0; md < 3; md++) {                      /* GRAN STRCH LOOP: the send returns, and stays a while after */
        double none, send, tail, dc, r, wet = 0;
        int32_t pk;
        grain_setup(md, 0);
        render(KEY(7) | KEY(11), FS / 2, FS * 2);
        memcpy(none_buf, buf, sizeof none_buf);
        none = rms_of(FS / 2 + FS / 8, FS);
        grain_setup(md, 127);
        render(KEY(7) | KEY(11), FS / 2, FS * 2);
        stats(0, FS * 2, &r, &pk, &dc);
        for (i = 0; i < FS / 2; i++)
            wet += ((double)buf[i] - none_buf[i]) * ((double)buf[i] - none_buf[i]);
        send = sqrt(wet / (FS / 2));                  /* the return while playing */
        tail = rms_of(FS / 2 + FS / 8, FS);
        printf("GRAIN %-5s: the return %.0f rms while playing; %.0f after (%.0f without the send); peak %d\n",
               N_RTYPE[md], send, tail, none, pk);
        CHECK(send > 200, "GRAIN %s returns little (%.0f)", N_RTYPE[md], send);
        CHECK(pk < 32767, "GRAIN %s clips (%d)", N_RTYPE[md], pk);
        CHECK(fabs(dc) < 600, "GRAIN %s DC %.0f", N_RTYPE[md], dc);
        if (md != 1)                                  /* (STRCH plays at the write head: its tail is the input's) */
            CHECK(tail > none + 20, "GRAIN %s: nothing after the notes (%.0f, %.0f without)", N_RTYPE[md], tail, none);
    }
    /* FRZ: what GRAN took in plays on after the notes, as long as it is held, and stops being written */
    grain_setup(0, 127);
    for (f = 0; f < FS * 3; f += CTL) {
        int32_t o[2 * CTL];
        fm1_in.notes = f < FS / 2 ? KEY(7) | KEY(11) : 0u;
        if (f >= FS / 2)
            song.g[G_RTYPE] = 3;                      /* FRZ as the notes end */
        mix_block(o, CTL);
        for (i = 0; i < CTL && f + i < FS * 3; i++)
            buf[f + i] = (int16_t)(o[2 * i] > 32767 ? 32767 : o[2 * i] < -32768 ? -32768 : o[2 * i]);
    }
    if (getenv("VERBOSE"))
        for (f = 0; f < FS * 3; f += FS / 4)
            printf("  FRZ %.2f s: %.0f\n", (double)f / FS, rms_of(f, f + FS / 4));
    {
        double late = rms_of(FS * 2, FS * 3);
        printf("GRAIN FRZ: %.0f rms 1.5 .. 2.5 s after the notes (MODE %d)\n", late, song.g[G_RTYPE]);
        CHECK(late > 30, "GRAIN FRZ: the frozen buffer does not play (%.0f)", late);
    }
    song.g[G_RTYPE] = 0;
}

/* ---------------------------------------------------------------- presets */
static void test_presets(uint32_t e)
{
    const engine_t *en = ENGINES[e];
    uint32_t k, i;
    for (k = 0; k < en->npresets; k++) {
        double rms, dc;
        int32_t pk;
        reset_all();
        host_preset(&trk[0], e, k);
        trk[0].p[P_SLEN] = 16;
        transport_req = 1;                           /* (the TRIG slots need it) */
        render(KEY(7) | KEY(11), FS, FS);
        stats(0, FS, &rms, &pk, &dc);
        if (getenv("WAV")) {
            char name[64];
            FILE *w;
            snprintf(name, sizeof name, "rack_%s_%s.wav", en->name, en->presets[k].name);
            for (i = 0; name[i]; i++)
                if (name[i] == ' ') name[i] = '_';
            w = fopen(name, "wb");
            wav_hdr(w, FS);
            for (i = 0; i < FS; i++)
                wav_put(w, buf[i], buf[i]);
            fclose(w);
        }
        transport_req = 2;
        render(0, 0, FS * 4);
        printf("%s preset %-9s rms %6.0f peak %5d (MOD A %s, MOD B %s)\n", en->name, en->presets[k].name, rms, pk,
               N_RMOD[trk[0].p[P_MATYP] % RM_N], N_RMOD[trk[0].p[P_MBTYP] % RM_N]);
        CHECK(rms > 150 && pk < 32000, "%s preset %s: rms %.0f peak %d", en->name, en->presets[k].name, rms, pk);
        CHECK(busy(&trk[0]) == 0, "%s preset %s: voices left", en->name, en->presets[k].name);
    }
}

/* ---------------------------------------------------------------- projects */
static void test_project(void)
{
    static project_t a, c;
    static project_store_t st;
    int ok;
    memset(&a, 0, sizeof a);
    a.magic = PROJ_MAGIC; a.size = sizeof a; a.parts = NPART; a.phys = PROJ_PHYS;
    chain_defaults(&a.chain);
    memcpy(a.g, song.g, sizeof a.g);
    a.g[G_DRLVL] = 55;                                /* GRAIN POS */
    a.g[G_MIDI] = -7;                                 /* GRAIN PTCH */
    a.t[0].engine = ENGI_MODEL;
    a.t[0].p[P_MATYP] = RM_TRIG2;
    a.t[0].p[P_MA4] = 99;
    a.t[0].p[P_MBTYP] = RM_CYCLE;
    a.t[0].p[P_MB3] = 77;
    a.t[0].p[P_ITYPE] = IT_XMOD;
    a.t[0].p[P_E7] = 42;
    a.mch[2] = 5;
    a.sum = proj_sum(&a);
    ok = proj_pack(&st, &a);
    CHECK(ok, "a rack project does not pack");
    ok = ok && proj_import(&c, &st, sizeof st);
    CHECK(ok, "a rack project does not load");
    CHECK(ok && c.t[0].p[P_MATYP] == RM_TRIG2 && c.t[0].p[P_MA4] == 99 && c.t[0].p[P_MB3] == 77 &&
          c.t[0].p[P_ITYPE] == IT_XMOD && c.t[0].p[P_E7] == 42 && c.t[0].engine == ENGI_MODEL &&
          c.g[G_DRLVL] == 55 && c.g[G_MIDI] == -7 && c.mch[2] == 5,
          "a rack project's slots, engine, GRAIN or MIDI channels changed in the store");
    CHECK(st.raw[66] == P_COUNT && P_COUNT > PROJ_NP_FELUCCA, "the store's np (%u)", st.raw[66]);
    printf("project: a rack project packs and loads with its slots (np %u)\n", st.raw[66]);
}

int main(void)
{
    printf("FM1 RACK: %u MACRO oscillators, %u MODEL voices; delay line %u samples\n", MI_NOSC, MI_NMODEL, DLY_LEN);
    test_math();
    test_macro();
    test_model();
    test_mods();
    test_xmod();
    test_grain();
    test_presets(ENGI_MACRO);
    test_presets(ENGI_MODEL);
    test_project();
    printf(fails ? "rack_test: %d FAILED\n" : "rack_test: all passed\n", fails);
    return fails != 0;
}
