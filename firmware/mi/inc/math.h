/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
/* FM1 Rack: the maths the Mutable Instruments code calls, single precision only (the FM-1's FPU has no double;
 * there is no libm: firmware/mi/mi_math.cc implements these). The double names take and return float, so no double
 * operation is ever made (tools/mi_build.py also builds with -fsingle-precision-constant and fails on any
 * soft-double routine). */
#define M_PI 3.14159265358979323846f
#define M_PI_2 1.57079632679489661923f
#define M_SQRT2 1.41421356237309504880f
#define M_LN2 0.69314718055994530942f
#ifdef __cplusplus
extern "C" {
#endif
float sinf(float x);
float cosf(float x);
float tanf(float x);
float expf(float x);
float exp2f(float x);
float logf(float x);
float log2f(float x);
float log10f(float x);
float powf(float x, float y);
float sqrtf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float truncf(float x);
float fmodf(float x, float y);
float tanhf(float x);
float atanf(float x);
float atan2f(float y, float x);
float asinf(float x);
float acosf(float x);
float sinhf(float x);
float coshf(float x);
float modff(float x, float *i);
float frexpf(float x, int *e);
float ldexpf(float x, int e);
#ifdef __cplusplus
}
inline float sin(float x) { return sinf(x); }
inline float cos(float x) { return cosf(x); }
inline float tan(float x) { return tanf(x); }
inline float exp(float x) { return expf(x); }
inline float exp2(float x) { return exp2f(x); }
inline float log(float x) { return logf(x); }
inline float log2(float x) { return log2f(x); }
inline float log10(float x) { return log10f(x); }
inline float pow(float x, float y) { return powf(x, y); }
inline float sqrt(float x) { return sqrtf(x); }
inline float fabs(float x) { return fabsf(x); }
inline float floor(float x) { return floorf(x); }
inline float ceil(float x) { return ceilf(x); }
inline float round(float x) { return roundf(x); }
inline float trunc(float x) { return truncf(x); }
inline float fmod(float x, float y) { return fmodf(x, y); }
inline float tanh(float x) { return tanhf(x); }
inline float atan(float x) { return atanf(x); }
inline float atan2(float y, float x) { return atan2f(y, x); }
inline float asin(float x) { return asinf(x); }
inline float acos(float x) { return acosf(x); }
inline float sinh(float x) { return sinhf(x); }
inline float cosh(float x) { return coshf(x); }
inline int abs(int x) { return x < 0 ? -x : x; }
#endif
