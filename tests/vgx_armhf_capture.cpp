#include <cassert>
#include <cstring>
extern "C" unsigned long long port_headless_render(const char*, int);
static unsigned calls = 0;
static const char* receivedPath;
static int receivedScale;
static unsigned long long completedFrame;
static int active = 1;
static int clockEnabled = 1;
static unsigned releases = 0;
static unsigned expectedReleases = 0;
extern "C" int port_vgx_active() { return active; }
extern "C" int port_vclock_enabled() { return clockEnabled; }
extern "C" void port_vclock_release() { ++releases; }
extern "C" unsigned long long port_vgx_capture(const char* path, int scale) {
    assert(releases == expectedReleases);
    ++calls; receivedPath = path; receivedScale = scale; return completedFrame;
}
int main() {
    const char path[] = "no-rasterized-pixels.png";
    completedFrame = 42;
    expectedReleases = 1;
    assert(port_headless_render(path, 3) == 42);
    assert(calls == 1 && receivedPath == path && receivedScale == 3);
    completedFrame = 0;
    expectedReleases = 2;
    assert(port_headless_render(path, 1) == 0);
    assert(calls == 2 && receivedPath == path && receivedScale == 1);
    active = 0; completedFrame = 123;
    assert(port_headless_render(path, 2) == 0);
    assert(calls == 2 && releases == 2);
    active = 1; clockEnabled = 0; completedFrame = 43;
    assert(port_headless_render(path, 2) == 43);
    assert(calls == 3 && releases == 2 && receivedScale == 2);
}
