#include "JSystem/J3DGraphBase/J3DGD.hpp"

extern "C" __attribute__((noinline)) void gd_raw_u16(u16 value) { J3DGDWrite_u16(value); }
extern "C" __attribute__((noinline)) void gd_raw_u32(u32 value) { J3DGDWrite_u32(value); }
extern "C" __attribute__((noinline)) void gd_checked_u16(u16 value) { GDWrite_u16(value); }
extern "C" __attribute__((noinline)) void gd_checked_u32(u32 value) { GDWrite_u32(value); }
extern "C" __attribute__((noinline)) void gd_raw_float(f32 value) { J3DGDWrite_f32(value); }
extern "C" __attribute__((noinline)) void gd_checked_float(f32 value) { GDWrite_f32(value); }
extern "C" __attribute__((noinline)) void gd_raw_bp(u32 value) { J3DGDWriteBPCmd(value); }
extern "C" __attribute__((noinline)) void gd_raw_u8(u8 value) { J3DGDWrite_u8(value); }
