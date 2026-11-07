// Scalar model of the ARMv7 NEON intrinsics used (IEEE single, round-to-nearest; VMLA = round(mul) then round(add)).
#pragma once
#include <string.h>
struct float32x4_t { float v[4]; };
struct float32x2_t { float v[2]; };
static inline float32x4_t vld1q_f32(const float* p) { float32x4_t r; memcpy(r.v, p, 16); return r; }
static inline void vst1q_f32(float* p, float32x4_t a) { memcpy(p, a.v, 16); }
static inline float32x4_t vdupq_n_f32(float x) { return {{x, x, x, x}}; }
static inline float32x2_t vget_low_f32(float32x4_t a) { return {{a.v[0], a.v[1]}}; }
static inline float32x2_t vget_high_f32(float32x4_t a) { return {{a.v[2], a.v[3]}}; }
static inline float vgetq_lane_f32(float32x4_t a, int i) { return a.v[i]; }
static inline float32x4_t vsetq_lane_f32(float x, float32x4_t a, int i) { a.v[i] = x; return a; }
static inline float32x4_t vmulq_lane_f32(float32x4_t a, float32x2_t b, int l) { volatile float s = b.v[l]; float32x4_t r; for (int i = 0; i < 4; i++) { volatile float m = a.v[i] * s; r.v[i] = m; } return r; }
static inline float32x4_t vmlaq_lane_f32(float32x4_t acc, float32x4_t a, float32x2_t b, int l) { float32x4_t r; for (int i = 0; i < 4; i++) { volatile float m = a.v[i] * b.v[l]; volatile float s = acc.v[i] + m; r.v[i] = s; } return r; }
static inline float32x4_t vmlaq_n_f32(float32x4_t acc, float32x4_t a, float b) { float32x4_t r; for (int i = 0; i < 4; i++) { volatile float m = a.v[i] * b; volatile float s = acc.v[i] + m; r.v[i] = s; } return r; }
