#include "checkpoint.h"

#include <array>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace vk_checkpoint;
using Bytes = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

void check(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}

Bytes read(const fs::path &path) {
    std::ifstream f(path, std::ios::binary);
    check(f.is_open(), "cache is absent");
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

template<class T> void add(Bytes &out, T value) {
    const auto *p = reinterpret_cast<const uint8_t *>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

struct Hashes {
    std::array<uint8_t, 32> frag, vert;
};

Snapshot example(const fs::path &dir, uint8_t seed) {
    Snapshot s;
    s.shader_file = dir / "hashs-vk.dat";
    s.pipeline_file = dir / "pipeline-cache-vk20.dat";
    std::vector<Hashes> hashes(2);
    uint8_t n = seed;
    for (auto &hash : hashes) {
        for (auto &v : hash.frag) v = n++;
        for (auto &v : hash.vert) v = n++;
    }
    s.shaders = shader_bytes(hashes, 20, 0x67);
    s.keys = { 0x123456789abcdef0ULL, 0xfedcba9999888877ULL };
    s.driver.resize(2 * 1024 * 1024);
    for (auto &v : s.driver) v = n++;
    s.magic = 0xbeef4321;
    return s;
}

Bytes expected_driver(const Snapshot &s) {
    Bytes out;
    add(out, s.magic);
    add(out, s.keys.size());
    for (auto k : s.keys) add(out, k);
    out.insert(out.end(), s.driver.begin(), s.driver.end());
    return out;
}

void run(const fs::path &dir) {
    auto s = example(dir, 7);
    check(persist(s), "first persistence failed");
    Bytes expected_shader;
    add(expected_shader, size_t{2});
    add(expected_shader, uint32_t{20});
    add(expected_shader, uint32_t{0x67});
    for (unsigned i = 7; i < 7 + 128; i++) expected_shader.push_back(i);
    check(read(s.shader_file) == expected_shader, "shader list differs from upstream format");
    auto original = expected_driver(s);
    check(read(s.pipeline_file) == original, "full driver cache or key list differs");
    check(!replace_file(s.pipeline_file, [](FILE *f) { std::fputs("partial", f); return false; }), "accepted partial write");
    check(!replace_file(s.pipeline_file, [](FILE *) -> bool { throw 1; }), "accepted failed writer");
    check(read(s.pipeline_file) == original, "failed write damaged last good file");
    size_t files = 0;
    for ([[maybe_unused]] const auto &entry : fs::directory_iterator(dir)) ++files;
    check(files == 2, "temporary file leaked");
    s.driver.clear();
    check(persist(s) && read(s.pipeline_file) == original, "empty driver replaced last good cache");

    Writer writer;
    auto older = example(dir, 19);
    auto newer = example(dir, 42);
    auto expected_old = expected_driver(older);
    auto expected_new = expected_driver(newer);
    std::promise<void> entered, unblock;
    auto in_worker = entered.get_future();
    auto can_write = unblock.get_future();
    const auto start = Clock::now();
    check(writer.start(std::move(older), [&](const Snapshot &snapshot) {
        entered.set_value();
        can_write.wait();
        return persist(snapshot);
    }), "could not start worker");
    const auto queued = Clock::now();
    const bool worker_started = in_worker.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
    const bool still_busy = writer.busy();
    const bool rejected_second = !writer.start(Snapshot{}, [](const Snapshot &) { return true; });
    const bool old_available_before_commit = read(s.pipeline_file) == original;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    unblock.set_value();
    check(writer.finish(), "draining queued writer failed");
    const auto drained = Clock::now();
    check(worker_started && still_busy && rejected_second && old_available_before_commit, "renderer did not progress independently or writer was unbounded");
    check(read(s.pipeline_file) == expected_old, "queued bytes differ");
    check(persist(newer), "could not publish latest shutdown checkpoint");
    check(read(newer.pipeline_file) == expected_new && read(newer.shader_file) == newer.shaders, "older save overtook shutdown");
    check(writer.start(Snapshot{}, [](const Snapshot &) -> bool { throw 1; }) && !writer.finish(), "worker failure not propagated");
    std::cout << "PASS upstream byte formats (2 MiB payload), empty data, failed write retention, "
                 "single in-flight worker, independent producer progress, orderly latest flush.\n"
              << "Synthetic blocked writer only: enqueue "
              << std::chrono::duration_cast<std::chrono::microseconds>(queued - start).count()
              << " us; enqueue through explicit drain "
              << std::chrono::duration_cast<std::chrono::milliseconds>(drained - start).count()
              << " ms (includes deliberate 80 ms gate + test I/O; not an emulator frame).\n";
}
}

int main(int argc, char **argv) {
    try {
        if (argc != 2) return 2;
        run(argv[1]);
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
