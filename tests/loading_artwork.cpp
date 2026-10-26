#include "loading_screen.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

constexpr size_t kPixels = 960u * 544u;
constexpr uint32_t kGuard = 0x51e2a9f4u;
void Check(bool valid, const char* message) {
    if (!valid) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
int main(int argc, char** argv) {
    Check(argc == 3, "asset and scratch directory supplied");
    const auto path = std::filesystem::absolute(argv[1]);
    std::vector<uint32_t> original(kPixels);
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(original.data()), kPixels * 4);
    Check(input.gcount() == static_cast<std::streamsize>(kPixels * 4) && input.peek() == EOF,
          "complete selected title picture is present");
    std::filesystem::current_path(argv[2]);
    std::filesystem::create_directories("app0:/sce_sys");
    std::filesystem::copy_file(path, "app0:/sce_sys/loading-title.rgba");
    Check(loading::Init(), "actual loader initializes");
    for (float progress : {0.0f, 0.5f, 1.0f}) {
        std::vector<uint32_t> frame(kPixels + 64, kGuard);
        loading::State state;
        state.progress = progress;
        state.caption = "Preparing shaders";
        loading::Render(frame.data() + 32, 0, state);
        Check(std::all_of(frame.begin(), frame.begin() + 32, [](auto p) { return p == kGuard; }) &&
              std::all_of(frame.end() - 32, frame.end(), [](auto p) { return p == kGuard; }),
              "frame canaries unchanged");
        for (unsigned y = 0; y < 544; ++y)
            for (unsigned x = 0; x < 960; ++x)
                if (x < 184 || x >= 776 || y < 390 || y >= 477)
                    Check(frame[32 + y * 960 + x] == original[y * 960 + x],
                          "all pixels outside the existing loading band stay identical");
        Check(frame[32 + 433 * 960 + 350] == (progress > 0 ? 0xfffae482u : 0xff41300au),
              "progress fills the left bar correctly");
        Check(frame[32 + 433 * 960 + 600] == (progress == 1 ? 0xfffae482u : 0xff41300au),
              "progress fills the right bar correctly");
    }
    loading::Release();
    std::filesystem::remove("app0:/sce_sys/loading-title.rgba");
    Check(loading::Init(), "existing missing-file fallback remains reachable at runtime");
    std::vector<uint32_t> frame(kPixels);
    loading::Render(frame.data(), 0, loading::State{});
    Check(frame[80 * 960 + 480] == 0xff301c01u, "missing file still uses the plain fallback");
    loading::Release();
    std::puts("PASS: actual loading renderer preserves title pixels, bar progression and runtime fallback");
}
