#include "../platform/vgx/vita/vgx_shader_pack.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" void port_log_summary(const char*, ...) {}

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const uint64_t abi = XXH3_64bits(argv[2], std::strlen(argv[2]));
    vgx::vita::ShaderPack pack;
    if (!pack.LoadFile(argv[3], argv[1], abi)) return 3;
    unsigned count = 0;
    uint64_t payload = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1])) {
        if (!entry.is_regular_file()) continue;
        std::ifstream file(entry.path(), std::ios::binary);
        const std::vector<uint8_t> original{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const void* bytes = nullptr; size_t size = 0;
        if (original.empty() || !pack.Find(entry.path().c_str(), &bytes, &size) || size != original.size() ||
            std::memcmp(bytes, original.data(), size)) return 4;
        ++count; payload += size;
    }
    if (count != std::strtoul(argv[4], nullptr, 10) || pack.Entries() != count) return 5;
    std::printf("PASS: real seed roundtrip records=%u payload_bytes=%llu retained_bytes=%zu ABI=%016llx (exact bytes, not GPU validation)\n",
                count, static_cast<unsigned long long>(payload), pack.Bytes(), static_cast<unsigned long long>(abi));
}
