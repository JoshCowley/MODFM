// SPDX-License-Identifier: GPL-3.0-only
// FM1 Rack (a Felucca fork): Braids' macro oscillator and Peaks' envelope and LFO behind a C interface
// (mi_glue.h). Mutable Instruments' code (MIT) is used unchanged from pichenettes/eurorack.
//
// Built freestanding: no exceptions, RTTI, static constructors or C library (firmware/mi/inc holds the few
// headers the modules include). The objects live in raw storage in the pool (zeroed at boot by main.c) and are
// set up by their own Init(), so nothing runs before main. tools/build.py fails the build if this object
// brings a constructor table (.init_array / .ctors) or a symbol the firmware does not define.

#include "braids/macro_oscillator.h"
#include "peaks/modulations/lfo.h"
#include "peaks/modulations/multistage_envelope.h"
#include "stmlib/utils/gate_flags.h"

#include "mi_glue.h"

namespace {

template <typename T, unsigned N>
struct Slots {                                    // N objects' worth of storage, no constructor
  uint32_t raw[N][(sizeof(T) + 3) / 4];
  T& operator[](uint32_t k) { return *reinterpret_cast<T*>(raw[k % N]); }
};

Slots<braids::MacroOscillator, MI_NOSC> osc __attribute__((section(".pool")));
Slots<peaks::MultistageEnvelope, MI_NPEAKS> penv __attribute__((section(".pool")));
Slots<peaks::Lfo, MI_NPEAKS> plfo __attribute__((section(".pool")));
uint8_t pgate[MI_NPEAKS] __attribute__((section(".pool")));

const uint8_t kNoSync[MI_BLOCK] = { 0 };

inline int16_t clamp16(int32_t x, int32_t lo, int32_t hi) {
  return static_cast<int16_t>(x < lo ? lo : x > hi ? hi : x);
}

}  // namespace

extern "C" {

// each oscillator and LFO draws its own random numbers (tools/mi_build.py PATCHES: stmlib::Random's single generator
// would be shared by the two cores), seeded by its slot: a part sounds the same on either core
void mi_osc_init(uint32_t k) {
  osc[k].Init();
  osc[k].SeedRandom(0x21u + (k % MI_NOSC) * 0x9E3779B9u);
}

void mi_osc_shape(uint32_t k, uint32_t shape) {
  if (shape >= MI_NSHAPE) shape = 0;
  osc[k].set_shape(static_cast<braids::MacroOscillatorShape>(shape));
}

void mi_osc_strike(uint32_t k) { osc[k].Strike(); }

void mi_osc_render(uint32_t k, int32_t pitch, int32_t timbre, int32_t color, int16_t* out, uint32_t n) {
  braids::MacroOscillator& o = osc[k];
  if (n > MI_BLOCK) n = MI_BLOCK;
  o.set_pitch(clamp16(pitch, 0, 16383));
  o.set_parameters(clamp16(timbre, 0, 32767), clamp16(color, 0, 32767));
  o.Render(kNoSync, out, n);
}

void mi_peaks_init(uint32_t k) {
  k %= MI_NPEAKS;
  penv[k].Init();
  plfo[k].Init();
  plfo[k].SeedRandom(0x3779B9u + k * 0x85EBCA6Bu);
  pgate[k] = 0;
}

int32_t mi_peaks_block(uint32_t k, uint32_t lfo, uint32_t a, uint32_t d, uint32_t s, uint32_t gate, uint32_t n) {
  stmlib::GateFlags flags[32];
  int16_t out[32];
  k %= MI_NPEAKS;
  if (n > 32) n = 32;
  if (n == 0) return 0;
  for (uint32_t i = 0; i < n; ++i) {
    flags[i] = stmlib::ExtractGateFlags(pgate[k], gate != 0);
    pgate[k] = flags[i];
  }
  if (lfo) {
    peaks::Lfo& l = plfo[k];
    l.set_sync(false);
    l.set_rate(static_cast<uint16_t>(a));
    l.set_shape_parameter_preset(static_cast<uint16_t>(d));
    for (uint32_t i = 0; i < n; ++i) flags[i] = stmlib::GATE_FLAG_LOW;   // free-running
    l.Process(flags, out, n);
  } else {
    peaks::MultistageEnvelope& e = penv[k];
    e.set_adsr(static_cast<uint16_t>(a), static_cast<uint16_t>(d), static_cast<uint16_t>(s), static_cast<uint16_t>(d));
    e.Process(flags, out, n);
  }
  return out[n - 1];
}

}  // extern "C"
