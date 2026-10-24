// Include the original unit to exercise its private FIFO producer unchanged.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#include "../decomp/src/JSystem/J3DGraphBase/J3DShape.cpp"
#pragma clang diagnostic pop

extern "C" void test_j3d_array_base(unsigned attr, void* data) {
    J3DLoadArrayBasePtr(static_cast<GXAttr>(attr), data);
}
