// Writes the handheld pictures of platform/src/port/handheld_art.inc as raw RGBA, to look at them:
//   g++ -O2 -std=c++17 -o art_test tools/handheld/art_test.cpp && ./art_test <out folder>
// (each picture: <hash>.rgba, 128x128, on the game's light blue ground)
#include "../../platform/src/port/handheld_art.inc"
#include <cstdio>
#include <string>
#include <vector>
int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    static const uint64_t hashes[] = {0x5c8ee03db0a44a08ull, 0x8deeaa9e63cd26ffull, 0x2e99253f110c8a84ull, 0x86b80905932085d6ull, 0x55a7b9fe04cfb1e0ull,
                                      0x24e5ac61034c6daeull, 0x7c891ac94b93a354ull, 0x720a2c4a8d40f663ull, 0x11cb720a4576ec9bull, 0x6f81bcf9cef94309ull,
                                      0x1f426fe7e5032b9dull, 0x93eb50681f48acccull, 0x4b5a247d08cc7680ull};
    int n = 0;
    for (uint64_t h : hashes) {
        const handheld_art::Entry* e = handheld_art::Find(h);
        if (!e) continue;
        std::vector<uint8_t> px(128 * 128 * 4, 0);
        px[0] = 168; px[1] = 211; px[2] = 255; px[3] = 255;
        handheld_art::Draw(px.data(), 128, e->pose);
        char name[64];
        std::snprintf(name, sizeof name, "/%02d.rgba", n++);
        if (FILE* f = std::fopen((dir + name).c_str(), "wb")) { std::fwrite(px.data(), 1, px.size(), f); std::fclose(f); }
    }
    std::printf("%d pictures\n", n);
    return 0;
}
