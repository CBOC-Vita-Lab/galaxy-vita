// Mesa has no native program-slot limit. Capacity fixtures provide their own
// finite allocator/query; this stand-in is never included by production code.
#pragma once
#ifndef VGX_TEST_NATIVE_CAPACITY
extern "C" inline unsigned vglGetFreeProgramSlots() { return 1024; }
#endif
