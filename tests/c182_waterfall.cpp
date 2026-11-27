#include <cassert>
#include <cstdio>

struct TVec3f {
    float x, y, z;
    TVec3f() = default;
    TVec3f(float a, float b, float c) : x(a), y(b), z(c) {}
    void set(float a, float b, float c) { x = a; y = b; z = c; }
};
struct SimpleEffectObj {
#include "offset.inc"
};

// Both WaterfallS and WaterfallL inherit this exact implementation.
__attribute__((noinline)) static TVec3f* get(const SimpleEffectObj& actor) {
    return actor.getClippingCenterOffset();
}
int main() {
    SimpleEffectObj waterfall;
    const TVec3f* off = get(waterfall);
    assert(off && off->x == 0.0f && off->y == 0.0f && off->z == 0.0f);
    std::puts("C182 waterfall clipping: valid zero offset after virtual call");
}
