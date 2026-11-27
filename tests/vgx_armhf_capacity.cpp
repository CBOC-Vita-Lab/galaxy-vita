#include <cassert>
extern "C" unsigned vglGetFreeProgramSlots();
extern "C" unsigned glCreateProgram();
int main() {
    // This no-GPU harness supplies an opaque native API result, not a model of
    // vitaGL's allocator. Finite native capacity is covered by separate tests.
    assert(vglGetFreeProgramSlots() == 1024);
    for (unsigned i = 0; i < 1200; ++i) assert(glCreateProgram() != 0);
    assert(vglGetFreeProgramSlots() == 1024);
}
