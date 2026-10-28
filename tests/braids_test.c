/* SPDX-License-Identifier: GPL-3.0-only */
/* FM1 Rack: the BRAIDS engine (firmware/src/eng_braids.c, firmware/mi/mi_glue.cc) on the host, through hostsim.c
 * with FELUCCA_BRAIDS=1. tests/run_tests.sh builds it with the C++ objects (tools/build_mi_host.sh).
 *   tuning       A4 = 440 Hz (0.5 %) on CSAW, MORPH, FOLD, SAW3 and COMB; A2 and A5 too on CSAW (the 44.1 kHz offset)
 *   every model  each of the 43 renders a held note: not silent, under full scale, no DC; the level against ANALOG
 *   modulators   PENV rises while held and falls after; PLFO and LOOP swing both ways; TIDE runs once per note;
 *                GRID hits only while the transport runs, more with more FILL, none at FILL 0
 *   oscillators  six keys on one part: four voices (the cap); eight keys on two parts: never two voices on one
 *                oscillator, a voice whose oscillator was taken silent; every voice ends after the release
 *   presets      every factory preset plays and ends
 * Exit 0 when everything passes. */
#define main hostsim_main
#include "hostsim.c"
#undef main

#define ENGI_BRAIDS 14u
_Static_assert(FELUCCA_BRAIDS, "build with -DFELUCCA_BRAIDS=1");

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static int16_t buf[FS * 4 * 2];

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
        trk[k].p[P_CHOR] = trk[k].p[P_DLY] = trk[k].p[P_REV] = trk[k].p[P_DIST] = 0;
    }
    memset(brd_part, 0, sizeof brd_part);
    for (k = 0; k < NPART; k++)
        brd_part[k].gstep = -1;
    fm1_in.notes = 0;
}

static void set_braids(track_t *t, int32_t model)
{
    host_preset(t, ENGI_BRAIDS, 0);
    t->p[P_E0] = (int16_t)model;
    t->p[P_E1] = 64;
    t->p[P_E2] = 64;
    t->p[P_E3] = 0;
    t->p[P_E4] = 0;
    t->p[P_ATK] = 0;
    t->p[P_DEC] = 64;
    t->p[P_SUS] = 127;
    t->p[P_REL] = 10;
    t->p[P_VOICE] = V_POLY;
    t->p[P_CHOR] = t->p[P_DLY] = t->p[P_REV] = t->p[P_DIST] = 0;
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

/* the fundamental of buf[a..b), expected near f0 (within 25 %: a wrong offset of a semitone or more shows): the
 * largest autocorrelation in that range of lags, refined by parabolic interpolation */
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
        return 0;                                    /* at the edge: not in range */
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

static uint32_t busy(const track_t *t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += t->v[i].active;
    return n;
}

/* keys: key 7 = C4 (song.octave 0); A4 = key 16 */
#define KEY(k) (1u << (k))

static void test_tuning(void)
{
    static const int32_t M[] = {0, 1, 3, 9, 15};     /* CSAW MORPH FOLD SAW3 COMB: the note is their fundamental */
    static const char *const NM[] = {"CSAW", "MORPH", "FOLD", "SAW3", "COMB"};
    uint32_t k;
    for (k = 0; k < NELEM(M); k++) {
        double hz;
        reset_all();
        set_braids(&trk[0], M[k]);
        render(KEY(16), FS, FS);
        hz = pitch_of(FS / 4, FS * 3 / 4, 440.0);
        printf("tuning %-5s A4: %.2f Hz\n", NM[k], hz);
        CHECK(fabs(hz / 440.0 - 1.0) < 0.005, "%s A4 at %.2f Hz, not 440", NM[k], hz);
    }
    {   /* A2 (octave down: song.octave -2) and A5 (+1) on CSAW */
        static const int8_t OC[2] = {-2, 1};
        static const double HZ[2] = {110.0, 880.0};
        for (k = 0; k < 2; k++) {
            double hz;
            reset_all();
            set_braids(&trk[0], 0);
            song.octave = OC[k];
            render(KEY(16), FS, FS);
            hz = pitch_of(FS / 4, FS * 3 / 4, HZ[k]);
            printf("tuning CSAW %.0f Hz: %.2f Hz\n", HZ[k], hz);
            CHECK(fabs(hz / HZ[k] - 1.0) < 0.005, "CSAW %.0f at %.2f Hz", HZ[k], hz);
            song.octave = 0;
        }
    }
}

static void test_models(void)
{
    int32_t m;
    double ref, lo = 1e30, hi = 0;
    int32_t pk;
    double dc;
    for (m = 0; m < BRD_NMODEL; m++) {
        double rms;
        uint32_t keys = KEY(7);
        reset_all();
        set_braids(&trk[0], m);
        render(keys, FS / 2, FS * 3 / 2);
        stats(0, FS / 2, &rms, &pk, &dc);           /* (from the start: the struck models decay) */
        if (rms < lo) lo = rms;
        if (rms > hi) hi = rms;
        if (getenv("VERBOSE"))
            printf("model %2d %-5s rms %7.1f peak %5d dc %6.1f\n", m, N_BRD_MODEL[m], rms, pk, dc);
        CHECK(rms > 150, "model %s silent (rms %.1f)", N_BRD_MODEL[m], rms);
        CHECK(pk < 30000, "model %s near full scale (%d)", N_BRD_MODEL[m], pk);
        CHECK(fabs(dc) < 400, "model %s DC %.1f", N_BRD_MODEL[m], dc);   /* (regress.c LIM_DC) */
        CHECK(busy(&trk[0]) == 0, "model %s: a voice still active 1 s after the release", N_BRD_MODEL[m]);
    }
    reset_all();
    host_preset(&trk[0], 0, 0);                      /* ANALOG SAW LEAD, dry: the level reference */
    trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_DIST] = 0;
    trk[0].p[P_VOICE] = V_POLY;
    render(KEY(7), FS / 2, FS * 3 / 2);
    stats(0, FS / 2, &ref, &pk, &dc);
    printf("models: %d render; rms %.0f .. %.0f (ANALOG SAW LEAD %.0f)\n", BRD_NMODEL, lo, hi, ref);
    CHECK(hi < ref * 4.0, "BRAIDS much louder than ANALOG (%.0f vs %.0f)", hi, ref);
}

static void test_mods(void)
{
    uint32_t f, b;
    int32_t mn, mx, first = 0, held, after;
    track_t *t = &trk[0];
    /* PENV */
    reset_all();
    set_braids(t, 0);
    t->p[P_E4] = BRM_PENV;
    t->p[P_E5] = 10;
    t->p[P_E6] = 60;
    t->p[P_E7] = 64;
    render(KEY(7), FS / 2, FS / 2);
    held = brd_part[0].val;
    render(0, 0, FS);
    after = brd_part[0].val;
    printf("PENV: %d held (SUS 64), %d 1 s after\n", held, after);
    CHECK(held > 12000 && held < 22000, "PENV held at %d, not near SUS 64", held);
    CHECK(after < 500, "PENV %d after the release", after);
    /* PLFO, LOOP: both ways */
    for (b = 0; b < 2; b++) {
        reset_all();
        set_braids(t, 0);
        t->p[P_E4] = b ? BRM_LOOP : BRM_PLFO;
        t->p[P_E5] = 70;
        t->p[P_E6] = 64;
        t->p[P_E7] = 64;
        mn = 32767;
        mx = -32768;
        for (f = 0; f < 2 * FS; f += CTL) {
            int32_t o[2 * CTL];
            fm1_in.notes = KEY(7);
            mix_block(o, CTL);
            if (brd_part[0].val < mn) mn = brd_part[0].val;
            if (brd_part[0].val > mx) mx = brd_part[0].val;
        }
        printf("%s: %d .. %d\n", b ? "LOOP" : "PLFO", mn, mx);
        CHECK(mn < -20000 && mx > 20000, "%s swings %d .. %d", b ? "LOOP" : "PLFO", mn, mx);
    }
    /* TIDE: once a note, TIME 40 (~17 ms .. ): over and back to 0 within 0.5 s */
    reset_all();
    set_braids(t, 0);
    t->p[P_E4] = BRM_TIDE;
    t->p[P_E5] = 60;
    t->p[P_E6] = 64;
    t->p[P_E7] = 20;
    mx = 0;
    for (f = 0; f < FS; f += CTL) {
        int32_t o[2 * CTL];
        fm1_in.notes = f < FS / 10 ? KEY(7) : 0u;
        mix_block(o, CTL);
        if (brd_part[0].val > mx) mx = brd_part[0].val;
        if (f == FS / 2) first = brd_part[0].val;
    }
    printf("TIDE: peak %d, %d at 0.5 s\n", mx, first);
    CHECK(mx > 30000, "TIDE peaks at %d", mx);
    CHECK(first == 0, "TIDE still at %d 0.5 s after its note", first);
    /* GRID: hits with the transport only */
    for (b = 0; b < 4; b++) {
        static const int16_t FILL[4] = {127, 64, 0, 127};
        uint32_t hits = 0, play = b < 3;
        int32_t prev = 0;
        reset_all();
        set_braids(t, 34);                           /* KICK */
        t->p[P_E4] = BRM_GBD;
        t->p[P_E5] = 64;
        t->p[P_E6] = 64;
        t->p[P_E7] = FILL[b];
        t->p[P_SLEN] = 16;
        transport_req = play ? 1u : 0u;
        for (f = 0; f < 4 * FS; f += CTL) {
            int32_t o[2 * CTL];
            fm1_in.notes = KEY(7);                   /* a held note: the hits strike it */
            mix_block(o, CTL);
            if (brd_part[0].val > prev + 10000) hits++;
            prev = brd_part[0].val;
        }
        if (play) {
            transport_req = 2;
            render(0, 0, CTL);
        }
        printf("GRID BD FILL %d %s: %u hits in 4 s\n", FILL[b], play ? "playing" : "stopped", hits);
        if (b == 0) { CHECK(hits >= 8, "GRID FILL 127: %u hits", hits); first = (int32_t)hits; }
        if (b == 1) CHECK(hits > 0 && (int32_t)hits <= first, "GRID FILL 64: %u hits (127: %d)", hits, first);
        if (b == 2) CHECK(hits == 0, "GRID FILL 0: %u hits", hits);
        if (b == 3) CHECK(hits == 0, "GRID stopped: %u hits", hits);
    }
}

static void test_slots(void)
{
    uint32_t f, k, i, j, bad = 0;
    reset_all();
    set_braids(&trk[0], 0);
    render(KEY(7) | KEY(9) | KEY(11) | KEY(12) | KEY(14) | KEY(16), FS / 2, FS / 2);
    printf("six keys, one part: %u voices\n", busy(&trk[0]));
    CHECK(busy(&trk[0]) <= MI_NOSC, "%u voices on one part (cap %u)", busy(&trk[0]), MI_NOSC);
    /* two parts: four keys each (part 2 gets MIDI-less key input: the selected track; play it by trk_note_on) */
    reset_all();
    set_braids(&trk[0], 0);
    set_braids(&trk[1], 28);
    for (f = 0; f < FS; f += CTL) {
        int32_t o[2 * CTL];
        fm1_in.notes = f < FS / 2 ? KEY(7) | KEY(11) | KEY(14) | KEY(18) : 0u;
        if (f == FS / 10)
            for (k = 0; k < 4; k++)
                trk_note_on(&trk[1], 48 + k * 4, 100);
        if (f == FS / 2)
            for (k = 0; k < 4; k++)
                trk_note_off(&trk[1], 48 + k * 4);
        mix_block(o, CTL);
        for (i = 0; i < MI_NOSC; i++)                /* one owner per oscillator, and it points back */
            for (j = i + 1; j < MI_NOSC; j++)
                if (brd_held(i) && brd_held(j) && brd_owner[i] == brd_owner[j])
                    bad++;
        for (k = 0; k < 2; k++)
            for (i = 0; i < NVOICE; i++) {
                const voice_t *v = &trk[k].v[i];
                if (v->active && v->s[0] && brd_owner[v->s[0] - 1] != v)
                    bad++;                           /* a stale slot number (it must have been cleared) */
            }
    }
    render(0, 0, FS * 2);
    printf("two parts, 8 keys: %u slot faults; voices left %u + %u\n", bad, busy(&trk[0]), busy(&trk[1]));
    CHECK(bad == 0, "%u slot faults", bad);
    CHECK(busy(&trk[0]) + busy(&trk[1]) == 0, "voices still active after the release");
}

static void test_presets(void)
{
    uint32_t k;
    for (k = 0; k < ENG_BRAIDS.npresets; k++) {
        double rms, dc;
        int32_t pk;
        reset_all();
        host_preset(&trk[0], ENGI_BRAIDS, k);
        trk[0].p[P_CHOR] = trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_DIST] = 0;
        trk[0].p[P_SLEN] = 16;
        if (ENG_BRAIDS.presets[k].e[4] >= BRM_GBD)
            transport_req = 1;
        render(KEY(7) | KEY(11), FS, FS);
        stats(0, FS, &rms, &pk, &dc);
        transport_req = 2;
        render(0, 0, FS * 3);
        printf("preset %-9s rms %6.0f peak %5d\n", ENG_BRAIDS.presets[k].name, rms, pk);
        CHECK(rms > 150 && pk < 30000, "preset %s: rms %.0f peak %d", ENG_BRAIDS.presets[k].name, rms, pk);
        CHECK(busy(&trk[0]) == 0, "preset %s: voices left", ENG_BRAIDS.presets[k].name);
    }
}

int main(void)
{
    if (getenv("WAV")) {                             /* WAV=model: 2 s of C4 into braids_<model>.wav (listening) */
        char name[64];
        FILE *w;
        uint32_t i;
        reset_all();
        set_braids(&trk[0], atoi(getenv("WAV")));
        render(KEY(7), FS * 3 / 2, FS * 2);
        snprintf(name, sizeof name, "braids_%s.wav", N_BRD_MODEL[atoi(getenv("WAV")) % BRD_NMODEL]);
        w = fopen(name, "wb");
        wav_hdr(w, FS * 2);
        for (i = 0; i < FS * 2; i++)
            wav_put(w, buf[i], buf[i]);
        fclose(w);
        return 0;
    }
    printf("BRAIDS: %u oscillators; delay line %u samples\n", MI_NOSC, DLY_LEN);
    test_tuning();
    test_models();
    test_mods();
    test_slots();
    test_presets();
    printf(fails ? "braids_test: %d FAILED\n" : "braids_test: all passed\n", fails);
    return fails != 0;
}
