#include "checkpoint.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace {
using namespace vk_checkpoint;
using Clock = std::chrono::steady_clock;

double ms(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void fill(std::vector<uint8_t> &bytes, uint32_t salt) {
    for (auto &byte : bytes) {
        salt ^= salt << 13;
        salt ^= salt >> 17;
        salt ^= salt << 5;
        byte = static_cast<uint8_t>(salt);
    }
}

bool same_file(const fs::path &a, const fs::path &b) {
    if (fs::file_size(a) != fs::file_size(b)) return false;
    std::ifstream left(a, std::ios::binary), right(b, std::ios::binary);
    char x[32768], y[32768];
    while (left && right) {
        left.read(x, sizeof(x));
        right.read(y, sizeof(y));
        if (left.gcount() != right.gcount() || !std::equal(x, x + left.gcount(), y)) return false;
    }
    return left.eof() && right.eof() && !left.bad() && !right.bad();
}

void run(const fs::path &out, bool smoke) {
    // Both modes persist identical prebuilt bytes. This times disk scheduling, not Vulkan serialization.
    Snapshot source;
    source.shaders.resize(smoke ? 128 : 77648);
    source.keys.resize(512);
    source.magic = 0xbeef4321;
    for (size_t i = 0; i < source.keys.size(); ++i) source.keys[i] = i;
    const size_t file_bytes = smoke ? 131072 : 31174000;
    source.driver.resize(file_bytes - 4 - sizeof(size_t) - 8 * source.keys.size());
    const auto sync = out / "sync", async = out / "async";
    if (!fs::create_directory(sync) || !fs::create_directory(async)) throw std::runtime_error("fresh paths required");
    std::cout << "pair,mode,driver_file_bytes,shader_file_bytes,caller_ms,through_drain_ms\n";
    for (int pair = 0; pair < (smoke ? 2 : 8); ++pair) {
        fill(source.driver, pair + 1);
        fill(source.shaders, pair + 17);
        for (int arm = 0; arm < 2; ++arm) {
            const bool worker = (pair + arm) % 2 != 0;
            Snapshot snapshot = source;
            const auto dest = worker ? async : sync;
            snapshot.pipeline_file = dest / "pipeline.dat";
            snapshot.shader_file = dest / "shader.dat";
            Writer writer;
            const auto start = Clock::now();
            const bool started = worker ? writer.start(std::move(snapshot), persist) : persist(snapshot);
            const auto returned = Clock::now();
            if (!started || (worker && !writer.finish())) throw std::runtime_error("write failed");
            const auto drained = Clock::now();
            std::cout << pair + 1 << ',' << (worker ? "worker" : "caller") << ',' << file_bytes << ','
                      << source.shaders.size() << ',' << std::fixed << std::setprecision(3)
                      << ms(start, returned) << ',' << ms(start, drained) << '\n';
        }
        for (const char *name : {"shader.dat", "pipeline.dat"})
            if (!same_file(sync / name, async / name)) throw std::runtime_error("readback mismatch");
    }
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 3 || (std::string(argv[2]) != "full" && std::string(argv[2]) != "smoke")) return 2;
        run(argv[1], std::string(argv[2]) == "smoke");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
