// Keep the production C translation unit and its static projection writer real.
#include "../decomp/src/RVL_SDK/gx/GXTransform.c"

void TestProjectionPayload(const float* source) {
    WriteProjPS(source, 0);
}
