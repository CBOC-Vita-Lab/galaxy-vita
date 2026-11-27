#include "vgx_compact_seed.h"
#include <atomic>
#include <cassert>
#include <fstream>
#include <thread>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    assert(argc == 6);
    const uint64_t abi = std::strtoull(argv[3], nullptr, 16);
    constexpr const char* root = "ux0:data/smg/shadercache/b4d4c2a9/v3";
    using vgx::vita::CompactSeed;
    CompactSeed wrong;
    assert(!wrong.Open(argv[1], root, abi ^ 1));
    CompactSeed seed;
    const bool opened = seed.Open(argv[1], root, abi);
    if (std::string(argv[4]) == "bad-index") { assert(!opened); return 0; }
    assert(opened && seed.Index("unrelated/v/00/0.gxp") < 0);
    std::atomic<unsigned> count{0};
    const bool corrupt = std::string(argv[4]) == "bad-record";
    auto read = [&]() {
        std::ifstream ref(argv[2], std::ios::binary);
        std::string path, length;
        unsigned rejected = 0;
        while (std::getline(ref, path) && std::getline(ref, length)) {
            std::vector<char> expected(std::stoul(length));
            assert(ref.read(expected.data(), expected.size()));
            const int i = seed.Index((std::string(root) + "/" + path).c_str());
            assert(i >= 0);
            const void* data; size_t size;
            if (!seed.Read(i, &data, &size)) { assert(corrupt); ++rejected; continue; }
            std::this_thread::yield(); // another reader may run before vitaGL copies
            assert(size == expected.size() && std::memcmp(data, expected.data(), size) == 0);
            ++count;
        }
        assert(rejected == (corrupt ? 1u : 0u));
    };
    std::thread t(read); read(); t.join();
    assert(count == 2 * (std::stoul(argv[5]) - (corrupt ? 1u : 0u)));
    std::printf("C182 seed: %u byte-exact concurrent reads; invalid-record handling %s\n", count.load(), argv[4]);
}
