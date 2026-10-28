// SPDX-License-Identifier: GPL-3.0-only
// FM1 Rack (a Felucca fork): the float maths Plaits, Clouds and Warps call (firmware/mi/inc/math.h), in single
// precision. The FM-1 has a single-precision FPU (no double, no square root instruction) and no C library; these
// are written for it: range reduction and short polynomials, accurate to a few units in the last place over the
// ranges the DSP uses (tests/rack_test.c checks them against the host's libm). Also the few C++ runtime pieces the
// virtual classes (Plaits' engines) need: no heap, nothing is ever deleted.
#include <stdint.h>
#include <stddef.h>
#include "math.h"

namespace {
union FB { float f; uint32_t u; int32_t i; };
inline float bits_f(uint32_t u) { FB b; b.u = u; return b.f; }
inline uint32_t f_bits(float f) { FB b; b.f = f; return b.u; }
const float kLog2e = 1.44269504088896341f;
const float kLn2 = 0.69314718055994530942f;
const float kPi = 3.14159265358979323846f;
const float kHalfPi = 1.57079632679489661923f;
}  // namespace

extern "C" {

float fabsf(float x) { return bits_f(f_bits(x) & 0x7FFFFFFFu); }

float truncf(float x) {
  if (!(fabsf(x) < 8388608.0f)) return x;               // (already whole, or not a number)
  return static_cast<float>(static_cast<int32_t>(x));
}
float floorf(float x) {
  float t = truncf(x);
  return t > x ? t - 1.0f : t;
}
float ceilf(float x) {
  float t = truncf(x);
  return t < x ? t + 1.0f : t;
}
float roundf(float x) { return x < 0.0f ? -floorf(0.5f - x) : floorf(x + 0.5f); }
float fmodf(float x, float y) {
  if (y == 0.0f) return 0.0f;
  return x - truncf(x / y) * y;
}
float modff(float x, float* i) {
  float t = truncf(x);
  *i = t;
  return x - t;
}
float ldexpf(float x, int e) {
  // in steps that stay in range: 2^e as a float is exact for -126 <= e <= 127
  while (e > 127) { x *= 1.7014118e38f; e -= 127; }
  while (e < -126) { x *= 1.1754944e-38f; e += 126; }
  return x * bits_f(static_cast<uint32_t>(e + 127) << 23);
}
float frexpf(float x, int* e) {
  uint32_t u = f_bits(x), ex = (u >> 23) & 0xFFu;
  if (x == 0.0f || ex == 0xFFu) { *e = 0; return x; }
  if (!ex) {                                            // subnormal: normalised first
    x *= 16777216.0f;
    u = f_bits(x);
    ex = ((u >> 23) & 0xFFu) - 24u;
  }
  *e = static_cast<int>(ex) - 126;
  return bits_f((u & 0x807FFFFFu) | (126u << 23));
}

float sqrtf(float x) {
  if (!(x > 0.0f)) return 0.0f;                         // (0, negative, not a number: 0)
  if (x == bits_f(0x7F800000u)) return x;
  // 1 / sqrt(x) from the exponent's half, three Newton steps, then x times it and one step on the root itself
  float y = bits_f(0x5F3759DFu - (f_bits(x) >> 1));
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  y = y * (1.5f - 0.5f * x * y * y);
  float r = x * y;
  return 0.5f * (r + x / r);
}

float exp2f(float x) {
  if (x > 127.9f) return bits_f(0x7F800000u);
  if (x < -149.0f) return 0.0f;
  float fl = floorf(x), f = x - fl;                     // 2^x = 2^n 2^f, f in [0, 1)
  int n = static_cast<int>(fl);
  // 2^f on [0, 1): degree 6 (the Taylor series of e^(f ln 2), error < 2e-7 relative)
  float t = f * kLn2;
  float p = 1.0f + t * (1.0f + t * (0.5f + t * (1.66666667e-1f + t * (4.16666667e-2f +
            t * (8.33333333e-3f + t * (1.38888889e-3f + t * 1.98412698e-4f))))));
  return ldexpf(p, n);
}
float expf(float x) {
  // e^x = 2^n e^r, n = round(x / ln 2), r = x - n ln 2 (ln 2 in two parts: exact products), |r| <= ln 2 / 2
  if (x > 88.7f) return bits_f(0x7F800000u);
  if (x < -103.9f) return 0.0f;
  float k = roundf(x * kLog2e);
  float r = (x - k * 0.693145751953125f) - k * 1.428606765330187e-6f;
  float p = 1.0f + r * (1.0f + r * (0.5f + r * (1.66666667e-1f + r * (4.16666667e-2f + r * (8.33333333e-3f +
            r * (1.38888889e-3f + r * 1.98412698e-4f))))));
  return ldexpf(p, static_cast<int>(k));
}

float logf(float x) {
  if (!(x > 0.0f)) return x == 0.0f ? -bits_f(0x7F800000u) : bits_f(0x7FC00000u);
  int e;
  float m = frexpf(x, &e);                              // x = m 2^e, m in [0.5, 1)
  if (m < 0.70710678f) { m *= 2.0f; e -= 1; }           // m in [0.707, 1.414)
  float s = (m - 1.0f) / (m + 1.0f), s2 = s * s;        // ln m = 2 atanh s, |s| < 0.172
  float l = 2.0f * s * (1.0f + s2 * (0.333333333f + s2 * (0.2f + s2 * (0.142857143f + s2 * 0.111111111f))));
  return l + static_cast<float>(e) * kLn2;
}
float log2f(float x) { return logf(x) * kLog2e; }
float log10f(float x) { return logf(x) * 0.434294481903251828f; }
// x^y = e^(y ln x) (expf's reduction keeps the error relative, not growing with the power)
float powf(float x, float y) {
  if (y == 0.0f) return 1.0f;
  if (x == 0.0f) return y > 0.0f ? 0.0f : bits_f(0x7F800000u);
  if (x < 0.0f) {                                       // a whole power of a negative number
    float w = truncf(y);
    if (w != y) return bits_f(0x7FC00000u);
    float r = expf(y * logf(-x));
    return (static_cast<int32_t>(fmodf(w, 2.0f)) != 0) ? -r : r;
  }
  return expf(y * logf(x));
}

// sin and cos on [-pi/4, pi/4] (Taylor series to x^9 / x^10: error < 4e-8)
static inline float sin_k(float x) {
  float x2 = x * x;
  return x * (1.0f + x2 * (-1.66666667e-1f + x2 * (8.33333333e-3f + x2 * (-1.98412698e-4f + x2 * 2.75573192e-6f))));
}
static inline float cos_k(float x) {
  float x2 = x * x;
  return 1.0f + x2 * (-0.5f + x2 * (4.16666667e-2f + x2 * (-1.38888889e-3f + x2 * (2.48015873e-5f +
         x2 * -2.75573192e-7f))));
}
// x reduced by quarter turns: x = q pi/2 + r, |r| <= pi/4 (pi/2 in two parts: the product stays exact longer)
static inline float quarter(float x, int* q) {
  float k = roundf(x * 0.636619772f);
  *q = static_cast<int>(static_cast<int32_t>(k) & 3);
  return (x - k * 1.5703125f) - k * 4.83826794897e-4f;
}
float sinf(float x) {
  int q;
  float r = quarter(x, &q);
  switch (q) {
    case 0: return sin_k(r);
    case 1: return cos_k(r);
    case 2: return -sin_k(r);
    default: return -cos_k(r);
  }
}
float cosf(float x) {
  int q;
  float r = quarter(x, &q);
  switch (q) {
    case 0: return cos_k(r);
    case 1: return -sin_k(r);
    case 2: return -cos_k(r);
    default: return sin_k(r);
  }
}
float tanf(float x) {
  float c = cosf(x);
  return c == 0.0f ? bits_f(0x7F800000u) : sinf(x) / c;
}

float atanf(float x) {
  float a = fabsf(x), off = 0.0f;
  bool inv = a > 1.0f;
  if (inv) a = 1.0f / a;                                // atan x = pi/2 - atan(1/x)
  if (a > 0.41421356f) {                                // atan a = pi/4 + atan((a - 1) / (a + 1)), |.| < 0.414
    a = (a - 1.0f) / (a + 1.0f);
    off = 0.78539816f;
  }
  float a2 = a * a;
  float r = off + a * (1.0f + a2 * (-0.333333333f + a2 * (0.2f + a2 * (-0.142857143f + a2 * (0.111111111f +
            a2 * (-0.0909090909f + a2 * 0.0769230769f))))));
  if (inv) r = kHalfPi - r;
  return x < 0.0f ? -r : r;
}
float atan2f(float y, float x) {
  if (x > 0.0f) return atanf(y / x);
  if (x < 0.0f) return y >= 0.0f ? atanf(y / x) + kPi : atanf(y / x) - kPi;
  return y > 0.0f ? kHalfPi : y < 0.0f ? -kHalfPi : 0.0f;
}
float asinf(float x) { return atan2f(x, sqrtf(1.0f - x * x)); }
float acosf(float x) { return atan2f(sqrtf(1.0f - x * x), x); }
float sinhf(float x) {
  if (fabsf(x) < 1e-3f) return x;
  float e = expf(x);
  return 0.5f * (e - 1.0f / e);
}
float coshf(float x) {
  float e = expf(x);
  return 0.5f * (e + 1.0f / e);
}
float tanhf(float x) {
  if (x > 9.0f) return 1.0f;
  if (x < -9.0f) return -1.0f;
  if (fabsf(x) < 1e-3f) return x;
  float e = expf(2.0f * x);
  return (e - 1.0f) / (e + 1.0f);
}

}  // extern "C"

#ifdef MI_FREESTANDING
// the C++ runtime's few pieces: a pure virtual is never called (abort by spinning), nothing is deleted (Plaits'
// engines have virtual destructors, whose deleting variants name operator delete)
extern "C" void __cxa_pure_virtual() { for (;;) { } }
void operator delete(void*) { }
void operator delete(void*, size_t) { }
#endif
