/* SPDX-License-Identifier: GPL-3.0-only
 * FM1 Rack (a Felucca fork): the C side of the Mutable Instruments code (mi_glue.cc, C++).
 *
 * The C++ is compiled on its own (tools/build.py, MI_SOURCES) and linked with felucca.o; this header is all the
 * firmware sees of it. Everything is integer: Braids and Peaks were written for chips without an FPU.
 *
 *   MI_NOSC Braids oscillators (braids::MacroOscillator, 17 KB each, in the pool) shared by the parts' voices:
 *   eng_braids.c hands them out at note-on (always on core 0: events_block), a render touches only its own.
 *   MI_NPEAKS Peaks modulators (one envelope and one LFO each), one per part. */
#ifndef MI_GLUE_H
#define MI_GLUE_H
#include <stdint.h>

#define MI_NOSC 4u
#define MI_NPEAKS 4u
#define MI_BLOCK 24u            /* the largest render: Braids' own block (macro_oscillator.h temp_buffer_) */
#define MI_NSHAPE 47u           /* braids::MacroOscillatorShape 0 .. DIGITAL_MODULATION (the user's 47 models) */

#ifdef __cplusplus
extern "C" {
#endif
void mi_osc_init(uint32_t k);                       /* oscillator k: as at power-on */
void mi_osc_shape(uint32_t k, uint32_t shape);      /* braids::MacroOscillatorShape (a change strikes) */
void mi_osc_strike(uint32_t k);                     /* re-excite the struck / plucked models */
/* n <= MI_BLOCK samples of oscillator k into out (overwritten): pitch in Braids' units (1/128 semitone; see
 * eng_braids.c BRD_PITCH0 for the 44.1 kHz offset), timbre and color 0..32767 */
void mi_osc_render(uint32_t k, int32_t pitch, int32_t timbre, int32_t color, int16_t *out, uint32_t n);

void mi_peaks_init(uint32_t k);
/* one block of n samples of part k's Peaks modulator, its last value returned (-32768..32767; ENV 0..32767):
 * lfo 0: the envelope (ATTACK a, DECAY d 0..65535, SUSTAIN level s 0..32767; release = d; gate held while a key is),
 *     1: the LFO (RATE a, SHAPE d: Peaks' shape presets; s and gate unused) */
int32_t mi_peaks_block(uint32_t k, uint32_t lfo, uint32_t a, uint32_t d, uint32_t s, uint32_t gate, uint32_t n);
#ifdef __cplusplus
}
#endif
#endif
