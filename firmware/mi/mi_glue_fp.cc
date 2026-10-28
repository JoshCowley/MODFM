// SPDX-License-Identifier: GPL-3.0-only
// FM1 Rack (a Felucca fork): Plaits (MODEL), Warps' cross-modulation (XMOD) and Clouds (GRAIN) behind the integer C
// interface of mi_glue.h. Mutable Instruments' code (Emilie Gillet, MIT) from pichenettes/eurorack, compiled for the
// FM-1's single-precision FPU (tools/mi_build.py: the fpu group).
//
// The objects live in raw storage in the pool and are built there by placement new at their first use (Plaits'
// engines are virtual classes: their constructors set the tables), so nothing runs before main. Everything here
// runs on the first core only (fx.c rack_core0): stmlib::Random's one generator is then drawn in one order.
//
// Rates: Plaits is tuned for 47.87 kHz and renders blocks of 12 (its envelopes count blocks): it renders here at 44.1
// kHz, retuned (kModelOffset), always by 12, the samples over a block kept for the next (a FIFO per voice), so its
// times run 8.8 % long, as Peaks' do. Warps' oscillator is tuned for 96 kHz (kXmodOffset). Clouds runs at its own
// 32 kHz rate's maths on 44.1 kHz samples: its grains are 27 % shorter and its pitch shifts as set.

#include <new>
#include "plaits/dsp/voice.h"
#include "clouds/dsp/granular_processor.h"
#include "warps/dsp/oscillator.h"
#include "warps_xmod.h"                         // (generated: warps/dsp/modulator.cc's Xmod functions)
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/utils/random.h"

#include "mi_glue.h"

namespace stmlib {
uint32_t Random::rng_state_ = 0x21;             // (stmlib/utils/random.cc)
}

namespace {

template <typename T, unsigned N>
struct Slots {                                  // N objects' worth of storage, built on first use
  uint32_t raw[N][(sizeof(T) + 3) / 4];
  T& operator[](uint32_t k) { return *reinterpret_cast<T*>(raw[k % N]); }
  T* build(uint32_t k) { return new (raw[k % N]) T(); }
};

const float kModelOffset = 1.4214f;             // 12 log2(47872.34 / 44100) semitones (plaits/dsp/dsp.h: its
                                                // tables are for the module's corrected rate, kCorrectedSampleRate)
const float kXmodOffset = -13.4669f;            // 12 log2(44100 / 96000)
const size_t kModelMemory = 16384;              // Plaits' engines' shared memory (voice.cc: 16 KB)
const uint32_t kModelBlock = plaits::kBlockSize;   // 12

struct ModelSlot {
  plaits::Voice::Frame fifo[plaits::kBlockSize];
  uint8_t have;                                 // frames in fifo not yet given out (at its end)
  uint8_t ready;
  uint8_t gate_low;                             // blocks the gate stays low (a strike: a new rising edge)
  uint8_t pulse;                                // blocks a strike holds the gate high after (a hit with no key)
};

Slots<plaits::Voice, MI_NMODEL> model __attribute__((section(".pool")));
uint32_t model_mem[MI_NMODEL][kModelMemory / 4] __attribute__((section(".pool")));
ModelSlot model_slot[MI_NMODEL] __attribute__((section(".pool")));

Slots<warps::Oscillator, MI_NXMOD> xosc __attribute__((section(".pool")));

// GRAIN: mono, low fidelity (8-bit mu-law at 16 kHz): the large buffer is all sample memory, the small one the FX
// workspace (granular_processor.cc Prepare): the diffuser 8 KB, the reverb 32 KB, then the correlator's 1.5 KB, which
// the pitch shifter's 8 KB line starts on and runs past (Clouds' own small buffer has the room)
const size_t kGrainLarge = 24576;
const size_t kGrainSmall = 8192 + 32768 + 8192;
Slots<clouds::GranularProcessor, 1> grain __attribute__((section(".pool")));
uint32_t grain_large[kGrainLarge / 4] __attribute__((section(".pool")));
uint32_t grain_small[kGrainSmall / 4] __attribute__((section(".pool")));
uint8_t grain_ready __attribute__((section(".pool")));

inline float q15(int32_t x) { return static_cast<float>(x) * (1.0f / 32768.0f); }
inline int16_t s16(float x) {
  int32_t v = static_cast<int32_t>(x * 32768.0f);
  return static_cast<int16_t>(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
}
inline float constrain(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

}  // namespace

extern "C" {

// ---------------------------------------------------------------- MODEL (Plaits)
void mi_model_init(uint32_t k) {
  k %= MI_NMODEL;
  plaits::Voice* v = model.build(k);
  stmlib::BufferAllocator a(model_mem[k], kModelMemory);
  v->Init(&a);
  model_slot[k].have = 0;
  model_slot[k].gate_low = 0;
  model_slot[k].pulse = 0;
  model_slot[k].ready = 1;
}

void mi_model_render(uint32_t k, const mi_model_t* p, int16_t* out, int16_t* aux, uint32_t n) {
  k %= MI_NMODEL;
  ModelSlot& s = model_slot[k];
  if (!s.ready) mi_model_init(k);
  plaits::Patch patch;
  plaits::Modulations mod;
  patch.note = static_cast<float>(p->note) * (1.0f / 256.0f) + kModelOffset;
  patch.harmonics = q15(p->harmonics);
  patch.timbre = q15(p->timbre);
  patch.morph = q15(p->morph);
  patch.frequency_modulation_amount = q15(p->fenv);
  patch.timbre_modulation_amount = q15(p->tenv);
  patch.morph_modulation_amount = q15(p->tenv);
  patch.engine = p->engine < MI_NENGINE ? p->engine : 0;
  patch.decay = q15(p->decay);
  patch.lpg_colour = q15(p->colour);
  mod.engine = 0.0f;
  mod.note = 0.0f;
  mod.frequency = 0.0f;
  mod.harmonics = 0.0f;
  mod.timbre = 0.0f;
  mod.morph = 0.0f;
  mod.level = 1.0f;
  mod.frequency_patched = false;
  mod.timbre_patched = false;
  mod.morph_patched = false;
  mod.trigger_patched = true;                   // the part's notes are Plaits' triggers (its drums and envelopes)
  mod.level_patched = !p->lpg;                  // LPG off: the gate held open at full level (the ADSR shapes it)
  if (p->strike) {
    s.gate_low = 1;
    s.pulse = 4;                                // (past Plaits' 5-sample trigger delay: kTriggerDelay)
  }
  while (n) {
    if (!s.have) {                              // one more of Plaits' own blocks
      mod.trigger = ((p->gate || s.pulse) && !s.gate_low) ? 1.0f : 0.0f;
      if (s.gate_low) s.gate_low--;
      else if (s.pulse) s.pulse--;
      model[k].Render(patch, mod, s.fifo, kModelBlock);
      s.have = kModelBlock;
    }
    uint32_t c = s.have < n ? s.have : n, from = kModelBlock - s.have;
    for (uint32_t i = 0; i < c; ++i) {
      *out++ = s.fifo[from + i].out;
      *aux++ = s.fifo[from + i].aux;
    }
    s.have = static_cast<uint8_t>(s.have - c);
    n -= c;
  }
}

// ---------------------------------------------------------------- XMOD (Warps)
void mi_xmod_init(uint32_t k) { xosc.build(k % MI_NXMOD)->Init(warps::kInternalOscillatorSampleRate); }

namespace {
// {shape, semitones} of CARR's eight carriers
const struct { uint8_t shape; int8_t semis; } kCarrier[8] = {
  {warps::OSCILLATOR_SHAPE_SINE, 0}, {warps::OSCILLATOR_SHAPE_TRIANGLE, 0}, {warps::OSCILLATOR_SHAPE_SAW, 0},
  {warps::OSCILLATOR_SHAPE_PULSE, 0}, {warps::OSCILLATOR_SHAPE_SINE, 5}, {warps::OSCILLATOR_SHAPE_SINE, 7},
  {warps::OSCILLATOR_SHAPE_SINE, 12}, {warps::OSCILLATOR_SHAPE_SINE, -12},
};
inline float xmod_one(int a, float m, float c, float t) {
  using warps::Modulator;
  switch (a) {
    case 0: return Modulator::Xmod<warps::ALGORITHM_XFADE>(m, c, t);
    case 1: return Modulator::Xmod<warps::ALGORITHM_FOLD>(m, c, t);
    case 2: return Modulator::Xmod<warps::ALGORITHM_ANALOG_RING_MODULATION>(m, c, t);
    case 3: return Modulator::Xmod<warps::ALGORITHM_DIGITAL_RING_MODULATION>(m, c, t);
    case 4: return Modulator::Xmod<warps::ALGORITHM_XOR>(m, c, t);
    default: return Modulator::Xmod<warps::ALGORITHM_COMPARATOR>(m, c, t);
  }
}
}  // namespace

void mi_xmod_block(uint32_t k, int32_t* b, uint32_t n, int32_t note, int32_t algo, int32_t timbre, uint32_t carrier) {
  float car[MI_GRAIN_BLOCK], fm[MI_GRAIN_BLOCK];
  k %= MI_NXMOD;
  carrier &= 7u;
  float x = constrain(q15(algo), 0.0f, 1.0f) * 4.999f, t = constrain(q15(timbre), 0.0f, 1.0f);
  int a = static_cast<int>(x);
  float frac = x - static_cast<float>(a);
  float pitch = static_cast<float>(note) * (1.0f / 256.0f) + kCarrier[carrier].semis + kXmodOffset;
  while (n) {
    uint32_t c = n < MI_GRAIN_BLOCK ? n : MI_GRAIN_BLOCK;
    for (uint32_t i = 0; i < c; ++i) fm[i] = 0.0f;
    xosc[k].Render(static_cast<warps::OscillatorShape>(kCarrier[carrier].shape), pitch, fm, car, c);
    for (uint32_t i = 0; i < c; ++i) {
      float m = constrain(q15(b[i]), -1.0f, 1.0f);
      float y = xmod_one(a, m, car[i], t);
      if (frac > 0.0f) y += (xmod_one(a + 1, m, car[i], t) - y) * frac;
      b[i] = static_cast<int32_t>(constrain(y, -1.0f, 1.0f) * 32767.0f);
    }
    b += c;
    n -= c;
  }
}

// ---------------------------------------------------------------- GRAIN (Clouds)
void mi_grain_init(void) {
  clouds::GranularProcessor* g = grain.build(0);
  g->Init(grain_large, kGrainLarge, grain_small, kGrainSmall);
  g->set_quality(3);                            // mono, low fidelity
  g->set_playback_mode(clouds::PLAYBACK_MODE_GRANULAR);
  g->Prepare();
  grain_ready = 1;
}

void mi_grain_block(const mi_grain_t* p, const int32_t* in, int32_t* out, uint32_t n) {
  clouds::ShortFrame fi[MI_GRAIN_BLOCK], fo[MI_GRAIN_BLOCK];
  if (!grain_ready) mi_grain_init();
  clouds::GranularProcessor& g = grain[0];
  if (n > MI_GRAIN_BLOCK) n = MI_GRAIN_BLOCK;
  clouds::Parameters* q = g.mutable_parameters();
  q->position = q15(p->position);
  q->size = q15(p->size);
  q->pitch = q15(p->pitch) * 24.0f;              // -2 .. +2 octaves
  q->density = q15(p->density);
  q->texture = q15(p->texture);
  q->dry_wet = 1.0f;                            // wet only: the bus's return
  q->stereo_spread = q15(p->spread);
  q->feedback = q15(p->feedback);
  q->reverb = q15(p->reverb);
  q->freeze = p->freeze != 0;
  q->trigger = false;
  q->gate = false;
  q->granular.overlap = 0.5f;
  q->granular.window_shape = 0.5f;
  q->granular.stereo_spread = 0.0f;
  q->granular.use_deterministic_seed = false;
  q->spectral.quantization = 0.0f;
  q->spectral.refresh_rate = 0.5f;
  q->spectral.phase_randomization = 0.0f;
  q->spectral.warp = 0.0f;
  clouds::PlaybackMode m = p->mode == 1 ? clouds::PLAYBACK_MODE_STRETCH
                         : p->mode == 2 ? clouds::PLAYBACK_MODE_LOOPING_DELAY : clouds::PLAYBACK_MODE_GRANULAR;
  g.set_playback_mode(m);
  g.Prepare();
  for (uint32_t i = 0; i < n; ++i) {
    int32_t v = in[i];
    v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
    fi[i].l = fi[i].r = static_cast<short>(v);
  }
  g.Process(fi, fo, n);
  for (uint32_t i = 0; i < n; ++i) out[i] = (static_cast<int32_t>(fo[i].l) + fo[i].r) >> 1;
}

}  // extern "C"
