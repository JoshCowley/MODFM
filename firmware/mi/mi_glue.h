/* SPDX-License-Identifier: GPL-3.0-only
 * FM1 Rack (a Felucca fork): the C side of the Mutable Instruments code (mi_glue.cc, C++).
 *
 * The C++ is compiled on its own (tools/mi_build.py) and linked with felucca.o; this header is all the firmware sees
 * of it, integers only. Braids and Peaks are integer code (mi_glue.cc); Plaits, Clouds and Warps are float, built for
 * the FM-1's FPU (mi_glue_fp.cc), and run on the first core only (fx.c rack_core0).
 *
 *   MI_NOSC Braids oscillators (braids::MacroOscillator, 17 KB each, in the pool) shared by the parts' voices:
 *   eng_macro.c hands them out at note-on (always on core 0: events_block), a render touches only its own.
 *   MI_NPEAKS Peaks modulators (one envelope and one LFO each), two per part.
 *   MI_NMODEL Plaits voices (plaits::Voice, 11 KB, and its engines' 16 KB of shared memory each): eng_model.c.
 *   MI_NXMOD Warps oscillators, the carriers of the XMOD insert (one per track): fx.c.
 *   GRAIN: one Clouds granular processor (mono, low fidelity), the bus where the reverb was: fx.c. */
#ifndef MI_GLUE_H
#define MI_GLUE_H
#include <stdint.h>

#define MI_NOSC 4u
#define MI_NPEAKS 8u             /* two per part (rack_mod.c MOD A, MOD B) */
#define MI_BLOCK 24u            /* the largest render: Braids' own block (macro_oscillator.h temp_buffer_) */
#define MI_NSHAPE 47u
#define MI_NMODEL 2u
#define MI_NXMOD 4u
#define MI_GRAIN_BLOCK 32u      /* the largest GRAIN block (clouds kMaxBlockSize) */
#define MI_NENGINE 24u          /* Plaits 1.2's engines (voice.cc order) */           /* braids::MacroOscillatorShape 0 .. DIGITAL_MODULATION (the user's 47 models) */

#ifdef __cplusplus
extern "C" {
#endif
void mi_osc_init(uint32_t k);                       /* oscillator k: as at power-on */
void mi_osc_shape(uint32_t k, uint32_t shape);      /* braids::MacroOscillatorShape (a change strikes) */
void mi_osc_strike(uint32_t k);                     /* re-excite the struck / plucked models */
/* n <= MI_BLOCK samples of oscillator k into out (overwritten): pitch in Braids' units (1/128 semitone; see
 * eng_macro.c BRD_PITCH0 for the 44.1 kHz offset), timbre and color 0..32767 */
void mi_osc_render(uint32_t k, int32_t pitch, int32_t timbre, int32_t color, int16_t *out, uint32_t n);

void mi_peaks_init(uint32_t k);
/* one block of n samples of part k's Peaks modulator, its last value returned (-32768..32767; ENV 0..32767):
 * lfo 0: the envelope (ATTACK a, DECAY d 0..65535, SUSTAIN level s 0..32767; release = d; gate held while a key is),
 *     1: the LFO (RATE a, SHAPE d: Peaks' shape presets; s and gate unused) */
int32_t mi_peaks_block(uint32_t k, uint32_t lfo, uint32_t a, uint32_t d, uint32_t s, uint32_t gate, uint32_t n);

/* MODEL (Plaits). Values 0..32767 (bipolar ones -32768..32767); note in 1/256 semitone (MIDI note << 8) */
typedef struct {
    int32_t note;                /* the pitch, MIDI note << 8 */
    uint16_t engine;             /* 0 .. MI_NENGINE - 1 */
    uint16_t harmonics, timbre, morph, decay, colour;
    int16_t tenv;                /* the internal envelope into TIMBRE and MORPH (bipolar) */
    int16_t fenv;                /* ... into the pitch (bipolar) */
    uint8_t lpg;                 /* 1: Plaits' low-pass gate pinged by each note (else open: the part's ADSR) */
    uint8_t gate;                /* a key held */
    uint8_t strike;              /* a new note or a hit: the gate falls for a block first */
} mi_model_t;
void mi_model_init(uint32_t k);
/* n samples of Plaits voice k: out and aux (each -32768..32767) */
void mi_model_render(uint32_t k, const mi_model_t *p, int16_t *out, int16_t *aux, uint32_t n);

/* XMOD (Warps' cross-modulation): the block b (a voice's full scale 32768) modulated by carrier k at note (MIDI << 8),
 * algo 0..32767 (XFADE FOLD ARING DRING XOR COMP, morphed between), timbre 0..32767, carrier 0..7 (CARR) */
void mi_xmod_init(uint32_t k);
void mi_xmod_block(uint32_t k, int32_t *b, uint32_t n, int32_t note, int32_t algo, int32_t timbre, uint32_t carrier);

/* GRAIN (Clouds): in, mono, a voice's scale (int32) -> out (wet only); n <= MI_GRAIN_BLOCK. mode 0 GRAN, 1 STRETCH,
 * 2 LOOP; the values 0..32767 (pitch -32768..32767: -2 .. +2 octaves) */
typedef struct {
    uint8_t mode, freeze;
    uint16_t position, size, density, texture, feedback, reverb, spread;
    int16_t pitch;
} mi_grain_t;
void mi_grain_init(void);
void mi_grain_block(const mi_grain_t *p, const int32_t *in, int32_t *out, uint32_t n);
#ifdef __cplusplus
}
#endif
#endif
