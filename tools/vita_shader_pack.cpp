// Local-only pack builder for already trusted compiled stage-cache files.
#include "../platform/vgx/vita/vgx_shader_pack.h"
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <string>
#include <vector>

namespace {
using vgx::vita::ShaderPack;
struct Record { uint32_t stage; uint64_t hash; std::vector<uint8_t> bytes; };
bool Error(const char* text) { std::fprintf(stderr, "shader pack: %s\n", text); return false; }
void U32(std::vector<uint8_t>& b, size_t at, uint32_t n) { for (unsigned i = 0; i < 4; ++i) b[at + i] = n >> (8 * i); }
void U64(std::vector<uint8_t>& b, size_t at, uint64_t n) { for (unsigned i = 0; i < 8; ++i) b[at + i] = n >> (8 * i); }
bool Build(const char* root, const char* identity, const char* output) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!*identity || std::strlen(identity) >= 512) return Error("invalid full ABI identity");
    uint32_t key = 2166136261u;
    for (const auto* c = reinterpret_cast<const uint8_t*>(identity); *c; ++c) key = (key ^ *c) * 16777619u;
    char expected[16]; std::snprintf(expected, sizeof expected, "%08x", key);
    if (fs::path(root).filename() != "v2" || fs::path(root).parent_path().filename() != expected)
        return Error("full ABI identity does not match the cache folder key/version");
    const uint64_t abi = XXH3_64bits(identity, std::strlen(identity));
    if (fs::exists(output, ec) || ec) return Error("refuse to overwrite an existing pack");
    std::vector<Record> records;
    size_t total = ShaderPack::kHeaderBytes;
    for (char kind : {'v', 'f'}) {
        const fs::path directory = fs::path(root) / std::string(1, kind);
        if (fs::symlink_status(directory, ec).type() != fs::file_type::directory || ec) return Error("both real stage directories must exist");
        fs::recursive_directory_iterator it(directory, fs::directory_options::none, ec), end;
        if (ec) return Error("cannot enumerate stage cache");
        for (; it != end; it.increment(ec)) {
            if (ec) return Error("cache enumeration failed");
            if (it->is_symlink(ec) || ec) return Error("cache symlinks are not accepted");
            if (it->is_directory(ec)) { if (ec || it.depth() != 0) return Error("unexpected cache directory depth"); continue; }
            if (ec || !it->is_regular_file(ec) || ec) return Error("cache contains nonregular data");
            const std::string name = it->path().string();
            Record record{};
            if (!ShaderPack::ParsePath(root, std::strlen(root), name.c_str(), record.stage, record.hash)) return Error("noncanonical stage-cache path");
            const auto size = it->file_size(ec);
            if (ec || size < 8 || size > ShaderPack::kMaxRecordBytes || records.size() >= ShaderPack::kMaxEntries) return Error("record size/count limit");
            const uint64_t next = uint64_t(total) + ShaderPack::kEntryBytes + ((size + 3) & ~uint64_t(3));
            if (next > ShaderPack::kMaxBytes) return Error("total pack limit");
            FILE* file = std::fopen(name.c_str(), "rb");
            if (!file) return Error("cannot open stage cache");
            record.bytes.resize(static_cast<size_t>(size));
            const bool read = std::fread(record.bytes.data(), 1, record.bytes.size(), file) == record.bytes.size();
            uint8_t extra;
            const bool exact = read && std::fread(&extra, 1, 1, file) == 0 && !std::ferror(file);
            const bool closed = std::fclose(file) == 0;
            if (!exact || !closed) return Error("stage cache changed or could not be read");
            total = static_cast<size_t>(next);
            records.push_back(std::move(record));
        }
        if (ec) return Error("cache enumeration failed");
    }
    if (records.empty()) return Error("empty stage cache");
    std::sort(records.begin(), records.end(), [](const Record& a, const Record& b) {
        return a.stage < b.stage || (a.stage == b.stage && a.hash < b.hash);
    });
    for (size_t i = 1; i < records.size(); ++i)
        if (records[i - 1].stage == records[i].stage && records[i - 1].hash == records[i].hash) return Error("duplicate cache identity");
    std::vector<uint8_t> bytes(total, 0);
    std::memcpy(bytes.data(), "VGXRAM01", 8); U32(bytes, 8, 1); U32(bytes, 12, records.size());
    U64(bytes, 16, bytes.size()); U64(bytes, 24, abi);
    size_t offset = ShaderPack::kHeaderBytes + records.size() * ShaderPack::kEntryBytes;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i]; const size_t e = ShaderPack::kHeaderBytes + i * ShaderPack::kEntryBytes;
        U32(bytes, e, r.stage); U32(bytes, e + 4, r.bytes.size()); U64(bytes, e + 8, r.hash); U64(bytes, e + 16, offset);
        std::memcpy(bytes.data() + offset, r.bytes.data(), r.bytes.size()); offset += (r.bytes.size() + 3) & ~size_t(3);
    }
    U64(bytes, 32, XXH3_64bits(bytes.data() + 40, bytes.size() - 40));
    const std::string temporary = std::string(output) + ".tmp";
    if (fs::exists(temporary, ec) || ec) return Error("temporary destination already exists");
    FILE* file = std::fopen(temporary.c_str(), "wbx");
    if (!file) return Error("cannot create temporary pack");
    const bool written = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    ShaderPack verify;
    if (!written || !closed || !verify.LoadFile(temporary.c_str(), root, abi)) { std::remove(temporary.c_str()); return Error("written pack failed verification"); }
    // Host-side publish without an overwrite race; never performed on the Vita.
    fs::create_hard_link(temporary, output, ec);
    if (ec) { std::remove(temporary.c_str()); return Error("cannot publish verified pack"); }
    std::remove(temporary.c_str());
    std::printf("PACK records=%zu retained_bytes=%zu abi=%016llx\n", records.size(), bytes.size(), static_cast<unsigned long long>(abi));
    return true;
}
}
int main(int argc, char** argv) {
    if (argc != 4) return Error("usage: vita_shader_pack cache-root-v2 full-ABI-identity new-output") ? 0 : 1;
    return Build(argv[1], argv[2], argv[3]) ? 0 : 1;
}
