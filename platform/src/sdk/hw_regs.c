// Shadow copies of the Broadway/Hollywood memory-mapped register blocks.
// The host has no such hardware; SDK code that still touches these sees
// ordinary memory, and the port's HLE modules read/write them where the
// behaviour matters (e.g. GX/PE status).
#include "revolution/types.h"

vu16 __VIRegs[0x3B];
vu32 __PIRegs[0xC];
vu16 __DSPRegs[0x20];
vu32 __AIRegs[0x8];
vu32 __EXIRegs[0x10];
vu16 __MEMRegs[0x40];
vu32 __DIRegs[16];
vu32 __SIRegs[64];
vu32 __ACRRegs[89];
vu32 __IPCRegs[4];

// Register files used by the GX library (see GXInit.c).
volatile u32 __PortGXPIRegs[0x400];
volatile u32 __PortGXCPRegs[0x400];
volatile u32 __PortGXPERegs[0x400];
volatile u32 __PortGXMEMRegs[0x400];
