// The loading screen's picture: the player's own capture when it is there, a plain dark starfield of our own when it is not.
// Usage: loading_artwork <capture.rgba or -> <scratch dir>
#include "loading_screen.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

constexpr size_t kPixels = 960u * 544u;
constexpr uint32_t kGuard = 0x51e2a9f4u;
const char* const kCapture = "ux0:data/smg/art/title.rgba";
void Check(bool valid, const char* message) {
    if (!valid) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
bool InBand(unsigned x, unsigned y) { return x >= 184 && x < 776 && y >= 390 && y < 477; }
std::vector<uint32_t> RenderOnce(float progress, const char* caption = "Preparing shaders") {
    std::vector<uint32_t> frame(kPixels + 64, kGuard);
    loading::State state;
    state.progress = progress;
    state.caption = caption;
    loading::Render(frame.data() + 32, 0, state);
    Check(std::all_of(frame.begin(), frame.begin() + 32, [](auto p) { return p == kGuard; }) &&
          std::all_of(frame.end() - 32, frame.end(), [](auto p) { return p == kGuard; }), "frame canaries unchanged");
    return std::vector<uint32_t>(frame.begin() + 32, frame.begin() + 32 + kPixels);
}
void WritePreview(const char* name, const std::vector<uint32_t>& frame) {
    const char* dir = std::getenv("TITLEART_PREVIEW_DIR");
    if (!dir) return;
    std::ofstream out(std::string(dir) + "/" + name, std::ios::binary);
    out << "P6\n960 544\n255\n";
    for (uint32_t p : frame) { const char rgb[3] = {char(p & 255), char((p >> 8) & 255), char((p >> 16) & 255)}; out.write(rgb, 3); }
}
void CheckBar(const std::vector<uint32_t>& frame, float progress) {
    Check(frame[433 * 960 + 350] == (progress > 0 ? 0xfffae482u : 0xff41300au), "progress fills the left bar correctly");
    Check(frame[433 * 960 + 600] == (progress == 1 ? 0xfffae482u : 0xff41300au), "progress fills the right bar correctly");
}
// No capture: the neutral starfield. Dark, starry, text-safe, deterministic, and independent of anything on disk.
std::vector<uint32_t> CheckNeutral(const char* what) {
    Check(loading::Init(), what);
    const auto frame = RenderOnce(0.0f, "");
    CheckBar(frame, 0.0f);
    unsigned stars = 0, bright = 0;
    uint64_t sum = 0;
    for (unsigned y = 0; y < 544; ++y)
        for (unsigned x = 0; x < 960; ++x) {
            const uint32_t p = frame[y * 960 + x];
            const unsigned r = p & 255, b = (p >> 16) & 255;
            Check((p >> 24) == 255, "neutral picture is opaque");
            sum += r + ((p >> 8) & 255) + b;
            if (r > 90) ++stars;
            if (r > 200) ++bright;
            const bool bar = y >= 426 && y < 440 && x >= 220 && x < 740, percent = y >= 380 && y < 420 && x >= 680 && x < 745;  // (the bar, and the "0%" it draws)
            if (InBand(x, y) && !bar && !percent) Check(b < 100 && r < 40, "no star or bright ground behind the bar and its text");
        }
    Check(stars > 300, "the neutral picture has a starfield");
    Check(bright > 60, "some stars are bright");
    Check(sum / kPixels < 140, "the neutral picture is dark overall");
    Check((frame[2 * 960 + 2] & 255) < 20 && ((frame[2 * 960 + 2] >> 16) & 255) < 60, "the corner is deep blue-black");
    loading::Release();
    return frame;
}
int main(int argc, char** argv) {
    Check(argc == 3, "capture (or -) and scratch directory supplied");
    std::filesystem::current_path(argv[2]);
    std::filesystem::create_directories("ux0:data/smg/art");
    std::vector<uint32_t> original(kPixels);
    if (std::string(argv[1]) == "-") {  // a synthetic capture
        for (unsigned y = 0; y < 544; ++y)
            for (unsigned x = 0; x < 960; ++x) original[y * 960 + x] = 0xff000000u | ((x * 3 + y) & 255) << 16 | ((y * 5 + x / 3) & 255) << 8 | ((x ^ y) & 255);
    } else {
        std::ifstream input(std::filesystem::absolute(argv[1]), std::ios::binary);
        input.read(reinterpret_cast<char*>(original.data()), kPixels * 4);
        Check(input.gcount() == static_cast<std::streamsize>(kPixels * 4) && input.peek() == EOF, "complete capture is present");
    }
    // 1. Nothing on disk: neutral.
    const auto neutral = CheckNeutral("first start: no capture, the neutral picture initializes");
    WritePreview("loading-neutral.ppm", neutral);
    // 2. The old packaged path is not used, and a wrong-size file at the new path is not used.
    std::filesystem::create_directories("app0:/sce_sys");
    { std::ofstream(  "app0:/sce_sys/loading-title.rgba", std::ios::binary).write(reinterpret_cast<const char*>(original.data()), kPixels * 4); }
    Check(CheckNeutral("a picture at the old package path is ignored") == neutral, "the neutral picture does not depend on any file");
    { std::ofstream out(kCapture, std::ios::binary); out.write(reinterpret_cast<const char*>(original.data()), kPixels * 4 - 4); }
    Check(CheckNeutral("a short capture is ignored") == neutral, "short capture gives the neutral picture");
    { std::ofstream out(kCapture, std::ios::binary | std::ios::app); out.write("abcdefgh", 8); }  // now 4 bytes too long
    Check(CheckNeutral("a long capture is ignored") == neutral, "long capture gives the neutral picture");
    std::filesystem::remove("app0:/sce_sys/loading-title.rgba");
    // 3. The capture: the player's picture, with only the loading band changed.
    std::filesystem::remove(kCapture);
    { std::ofstream out(kCapture, std::ios::binary); out.write(reinterpret_cast<const char*>(original.data()), kPixels * 4); }
    Check(loading::Init(), "the loader initializes with a capture");
    for (float progress : {0.0f, 0.5f, 1.0f}) {
        const auto frame = RenderOnce(progress);
        for (unsigned y = 0; y < 544; ++y)
            for (unsigned x = 0; x < 960; ++x)
                if (x < 184 || x >= 776 || y < 390 || y >= 477)
                    Check(frame[y * 960 + x] == original[y * 960 + x], "all pixels outside the loading band stay identical to the capture");
        CheckBar(frame, progress);
        if (progress == 0.5f) WritePreview("loading-capture.ppm", frame);
    }
    loading::Release();
    // 4. The capture goes away (the player deleted it): neutral again at the next start.
    std::filesystem::remove(kCapture);
    CheckNeutral("capture removed: neutral again");
    std::puts("PASS: loading screen uses the player's capture, falls back to the neutral starfield, and ignores the old package path");
}
