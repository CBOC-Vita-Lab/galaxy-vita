// POSIX host helper for the optional Vita3K Vulkan pipeline-cache checkpoint.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <unistd.h>

namespace vk_checkpoint {
namespace fs = std::filesystem;

struct Snapshot {
    fs::path shader_file, pipeline_file;
    std::vector<uint8_t> shaders;
    std::vector<uint64_t> keys;
    std::vector<uint8_t> driver;
    uint32_t magic = 0;
};

template<class List>
std::vector<uint8_t> shader_bytes(const List &records, uint32_t version, uint32_t features) {
    std::vector<uint8_t> out;
    const size_t count = records.size();
    out.reserve(sizeof(count) + 2 * sizeof(uint32_t) + count * 64);
    auto append = [&](const auto &v) {
        const auto *p = reinterpret_cast<const uint8_t *>(&v);
        out.insert(out.end(), p, p + sizeof(v));
    };
    append(count);
    append(version);
    append(features);
    for (const auto &h : records) {
        static_assert(sizeof(h.frag) == 32 && sizeof(h.vert) == 32);
        append(h.frag);
        append(h.vert);
    }
    return out;
}

// Never truncate the last good checkpoint. The temporary is unique and on
// the same filesystem as its destination. Each of the two caches is an
// independent upstream file; this is not a transaction across both files.
template<class Write>
bool replace_file(const fs::path &path, Write write) {
    std::string temp = path.string() + ".tmp.XXXXXX";
    const int fd = ::mkstemp(temp.data());
    if (fd < 0)
        return false;
    FILE *f = ::fdopen(fd, "wb");
    bool ok = false;
    if (f) {
        try {
            ok = write(f) && std::fflush(f) == 0 && ::fsync(fd) == 0;
        } catch (...) {
            ok = false;
        }
        if (std::fclose(f) != 0)
            ok = false;
    } else {
        ::close(fd);
    }
    if (ok && ::rename(temp.c_str(), path.c_str()) == 0)
        return true;
    ::unlink(temp.c_str());
    return false;
}

inline bool bytes(FILE *f, const void *data, size_t n) {
    return !n || std::fwrite(data, 1, n, f) == n;
}

inline bool persist(const Snapshot &s) {
    std::error_code err;
    fs::create_directories(s.shader_file.parent_path(), err);
    if (err || !replace_file(s.shader_file, [&](FILE *f) { return bytes(f, s.shaders.data(), s.shaders.size()); }))
        return false;
    if (s.driver.empty())
        return true; // upstream saves the shader list, but not an empty driver cache
    const size_t count = s.keys.size();
    return replace_file(s.pipeline_file, [&](FILE *f) {
        return bytes(f, &s.magic, sizeof(s.magic)) && bytes(f, &count, sizeof(count))
            && bytes(f, s.keys.data(), count * sizeof(uint64_t))
            && bytes(f, s.driver.data(), s.driver.size());
    });
}

// The renderer owns this object. Never queue a second giant driver blob:
// when busy, leave the checkpoint due and take a fresh snapshot on a later
// second. The worker operates only on moved data, never on Vulkan objects.
class Writer {
    std::future<bool> job;
public:
    bool busy() const {
        return job.valid() && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }
    bool finish() {
        return !job.valid() || job.get();
    }
    template<class Write>
    bool start(Snapshot snapshot, Write write) {
        if (job.valid())
            return false;
        try {
            job = std::async(std::launch::async, [s = std::move(snapshot), w = std::move(write)] {
                try { return w(s); } catch (...) { return false; }
            });
            return true;
        } catch (const std::system_error &) {
            return false;
        }
    }
};
} // namespace vk_checkpoint
