// Actual DVD boot validator over synthetic cooked AST/FST data. No game or device.
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#include "../platform/src/dvd/dvd.cpp"

namespace {
unsigned checks, visibleErrors;
bool visible = true;
std::string summaries;
void require(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void put16(std::vector<u8>& b, size_t at, u32 value) {
    b[at] = value; b[at + 1] = value >> 8;
}
void put32(std::vector<u8>& b, size_t at, u32 value, bool big = false) {
    for (unsigned i = 0; i < 4; ++i) b[at + (big ? 3 - i : i)] = value >> (8 * i);
}
void writeBytes(const std::string& path, const std::vector<u8>& bytes) {
    FILE* f = fopen(path.c_str(), "wb");
    require(f && fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size() && fclose(f) == 0, "write owned synthetic file");
}
std::vector<u8> ast(unsigned format = 0, unsigned channels = 2, bool rounded = false) {
    // One short final block: its per-channel size may round beyond EOF by <=32 bytes/channel.
    const unsigned perChannel = 128;
    std::vector<u8> b(64 + 32 + perChannel * channels - (rounded ? 16 * channels : 0));
    memcpy(b.data(), "STRM", 4);
    b[9] = format;
    put16(b, 12, channels);
    put16(b, 14, 1);
    put32(b, 16, 32000);
    put32(b, 24, 0);
    put32(b, 28, format ? 64 : 224);
    put32(b, 32, AudParams::streamBlockSize);
    memcpy(b.data() + 64, "BLCK", 4);
    put32(b, 68, perChannel);
    return b;
}
void fst(const std::string& root, u32 bytes, bool title = true) {
    const char names[] = "\0AudioRes\0Stream\0SMG_title_strm.ast\0other.ast\0";
    std::vector<u8> b(4 * 12 + sizeof(names));
    put32(b, 0, 0x01000000, true); put32(b, 8, 4, true);
    put32(b, 12, 0x01000001, true); put32(b, 20, 4, true);
    put32(b, 24, 0x0100000A, true); put32(b, 28, 1, true); put32(b, 32, 4, true);
    put32(b, 36, title ? 17 : 36, true); put32(b, 44, bytes, true);
    memcpy(b.data() + 48, names, sizeof(names));
    writeBytes(root + "/sys/fst.bin", b);
    sDataRoot = root;
    require(loadFst(), "actual FST parser loads synthetic table");
}
void check(bool ok, const char* text = nullptr) {
    const char* error = validateBootTitle();
    require((error == nullptr) == ok, "actual validator result");
    if (text) require(error && strstr(error, text), "specific actionable validation reason");
}
}

extern "C" void port_host_alloc_scope_enter() {}
extern "C" void port_host_alloc_scope_leave() {}
extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" void port_log(const char*, ...) {}
extern "C" void port_log_summary(const char* format, ...) {
    char line[2048]; va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
    summaries += line; summaries += '\n';
}
extern "C" int port_boot_data_error(const char*) { ++visibleErrors; return visible; }

int main(int argc, char**) {
    require(argc == 2, "mode provided by runner");
    const bool enabled = port_boot_data_enabled();
    char temp[] = "/tmp/galaxy-boot-data-XXXXXX";
    require(mkdtemp(temp), "create owned temporary root");
    const std::string root(temp), flat = root + "/files/AudioRes/Stream/SMG_title_strm.ast";
    std::filesystem::create_directories(root + "/sys");
    std::filesystem::create_directories(root + "/files/AudioRes/Stream/_bk00");
    auto bytes = ast();
    writeBytes(flat, bytes);
    fst(root, bytes.size());
    check(true);
    setenv("GXV_BOOTDATA", enabled ? "0" : "1", 1);
    require(port_boot_data_enabled() == enabled, "setting is cached after its first read");
    if (!enabled) {
        std::filesystem::remove_all(root);
        check(true);
        require(!port_boot_data_report("must remain quiet") && summaries.empty() && !visibleErrors,
                "disabled guard performs no file checks, summaries or UI calls even after files disappear");
        std::printf("PASS: disabled guard and cached-setting parity (%u checks)\n", checks);
        return 0;
    }

    for (unsigned format = 0; format < 2; ++format) for (unsigned channels : {1u, 2u, 4u}) for (bool rounded : {false, true}) {
        bytes = ast(format, channels, rounded); writeBytes(flat, bytes); fst(root, bytes.size()); check(true);
    }
    // The Vita resolver already chose this exact bucket path. Validation must not re-open a guessed flat path.
    const std::string bucket = root + "/files/AudioRes/Stream/_bk00/SMG_title_strm.ast";
    require(rename(flat.c_str(), bucket.c_str()) == 0, "move only owned fixture to bucket layout");
    sFst[3].hostPath = bucket;
    check(true);
    require(summaries.empty(), "successful validations emit no errors");
    require(unlink(bucket.c_str()) == 0, "remove owned title fixture");
    check(false, "missing");
    require(summaries.find(bucket) != std::string::npos, "failure identifies resolved bucket path");
    writeBytes(flat, bytes);
    fst(root, bytes.size(), false); check(false, "file list");
    fst(root, bytes.size());
    require(chmod(flat.c_str(), 0000) == 0, "make fixture unreadable under unprivileged runner");
    check(false, "unreadable");
    require(chmod(flat.c_str(), 0600) == 0, "restore fixture mode only");
    auto shortFile = bytes; shortFile.pop_back(); writeBytes(flat, shortFile); check(false, "size");
    writeBytes(flat, std::vector<u8>(63)); fst(root, 63); check(false, "header");
    writeBytes(flat, std::vector<u8>(bytes.size())); fst(root, bytes.size()); check(false, "header");
    bytes = ast();
    for (unsigned channels : {0u, static_cast<unsigned>(AudParams::numStreamChannels + 1)}) {
        auto bad = bytes; put16(bad, 12, channels); writeBytes(flat, bad); fst(root, bad.size()); check(false, "channels");
    }
    auto bad = bytes; bad[9] = 2; writeBytes(flat, bad); fst(root, bad.size()); check(false, "format");
    bad = bytes; put32(bad, 32, 0); writeBytes(flat, bad); check(false, "block");
    bad = bytes; put32(bad, 28, 0); writeBytes(flat, bad); check(false, "loop");
    bad = bytes; put32(bad, 24, 0xFFFFFFFF); writeBytes(flat, bad); check(false, "loop");
    // Non-looping streams do not use loop_start; avoid imposing a new restriction on that unused value.
    bad = bytes; put16(bad, 14, 0); put32(bad, 24, 0xFFFFFFFF); writeBytes(flat, bad); check(true);
    require(unlink(flat.c_str()) == 0 && mkdir(flat.c_str(), 0700) == 0, "directory is not a regular stream");
    check(false, "regular");
    require(rmdir(flat.c_str()) == 0 && mkfifo(flat.c_str(), 0600) == 0, "FIFO is not a regular stream");
    check(false, "regular");
    visible = false;
    require(!port_boot_data_report("unavailable"), "unavailable UI is not falsely reported as visible");
    visible = true;
    require(port_boot_data_report("Restore converted title audio and relaunch; saves unchanged."), "visible UI accepts actionable error");
    require(visibleErrors == 2, "report delegates only enabled visible-state requests");
    std::filesystem::remove_all(root);
    std::printf("PASS: %u real FST/cooked-AST checks; flat/resolved bucket, formats, final rounding and failures\n", checks);
}
