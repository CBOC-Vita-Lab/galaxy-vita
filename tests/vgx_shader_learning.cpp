// Real front end; fault injection stays at the C file-I/O boundary.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "../platform/vgx/src/vgx_draw.cpp"
#include <cerrno>
#include <limits>
#include <set>
#include <unistd.h>

namespace {
std::string mode, listPath;
FILE* appendFile = nullptr;
FILE* rewriteFile = nullptr;
unsigned appendWrites = 0, flushes = 0, frames = 0, appendOpens = 0;
size_t largestWrite = 0, failBudget = std::numeric_limits<size_t>::max();
bool submitted = false, decoded = false, injected = false, readFaultUsed = false;
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
bool Failure() { return mode.rfind("write-fail-", 0) == 0 || mode == "flush-fail" || mode == "rewrite-fail" || mode == "read-fail" || mode == "read-open-fail"; }
struct Header { char magic[8]; uint32_t vs, fs, version, reserved; };
struct Snapshot { unsigned keys[5] = {}; std::set<uint32_t> blend; std::vector<uint8_t> wire; size_t bytes = 0; };
const uint32_t expectedBlends[] = {0x401,0x441,0x481,0x4c1,0x400,0x445,0x485,0x4c5,
                                   0x421,0x461,0x4a1,0x4e1,0x425,0x465,0x4a5,0x4e5};
uint16_t drawnVs = 0, drawnFs = 0;
template<class T> void Bytes(std::vector<uint8_t>& out, const T& value) {
    const auto* p = reinterpret_cast<const uint8_t*>(&value); out.insert(out.end(), p, p + sizeof value);
}
std::vector<uint8_t> ExpectedWire(bool burst) {
    const Header h{{'V','G','X','S','H','L','0','1'}, sizeof(vgx::VsKey), sizeof(vgx::FsKey), vgx::kShaderGenVersion, 0};
    std::vector<uint8_t> bytes; Bytes(bytes, h);
    Bytes(bytes, 2u); Bytes(bytes, vgx::g_fsReg.keys.at(drawnFs));
    Bytes(bytes, 1u); Bytes(bytes, vgx::g_vsReg.keys.at(drawnVs));
    const uint32_t program[] = {3,0,0}; Bytes(bytes, program);
    for (uint32_t blend : expectedBlends) { const uint32_t record[] = {4,0,0,0,blend}; Bytes(bytes, record); }
    if (burst) for (unsigned i = 1; i <= 2000; ++i) { const uint32_t record[] = {4,0,0,i,1}; Bytes(bytes, record); }
    return bytes;
}
Snapshot Read() {
    FILE* f = std::fopen(listPath.c_str(), "rb");
    Require(f && std::fseek(f, 0, SEEK_END) == 0, "test recipe can be read");
    const long size = std::ftell(f);
    Require(size >= static_cast<long>(sizeof(Header)) && std::fseek(f, 0, SEEK_SET) == 0, "complete recipe header");
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    Require(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size() && std::fclose(f) == 0, "complete recipe read");
    Header h{}; std::memcpy(&h, bytes.data(), sizeof h);
    Require(std::memcmp(h.magic, "VGXSHL01", 8) == 0 && h.vs == sizeof(vgx::VsKey) && h.fs == sizeof(vgx::FsKey) &&
            h.version == vgx::kShaderGenVersion && h.reserved == 0, "wire format is unchanged");
    Snapshot s; s.bytes = bytes.size();
    size_t offset = sizeof h;
    while (offset < bytes.size()) {
        Require(bytes.size() - offset >= 4, "no incomplete record type survives a frame checkpoint");
        uint32_t type; std::memcpy(&type, bytes.data() + offset, 4); offset += 4;
        Require(type >= 1 && type <= 4, "only known record types");
        const size_t n = type == 1 ? h.vs : type == 2 ? h.fs : type == 3 ? 8 : 16;
        Require(bytes.size() - offset >= n, "every record payload is complete");
        if (type >= 3) {
            uint32_t ids[4]{}; std::memcpy(ids, bytes.data() + offset, n);
            Require(ids[0] == 0 && ids[1] == 0 && s.keys[1] == 1 && s.keys[2] == 1,
                    "programs and variants refer to preceding file IDs, not seed-only runtime IDs");
            if (type == 4 && ids[2] == 0) s.blend.insert(ids[3]);
            if (type == 4) Require(ids[2] <= 0xfff && ids[3] <= 0xfff, "variant fields retain their format bounds");
        }
        ++s.keys[type]; offset += n;
    }
#if GXV_RECIPE_SEED_BATCH
    vgx::recipe::List parsed;
    Require(vgx::recipe::Parse(bytes, parsed), "checked parser accepts learned synthetic records without duplicates");
#endif
    s.wire = std::move(bytes);
    return s;
}
void Blend(unsigned i) {
    const uint32_t w = 0x41000000u | 1u | (1u << 3) | (1u << 4) | ((i >> 2) << 8) | ((i & 3u) << 5);
    const uint8_t command[] = {0x61, uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)};
    Require(vgx::ProcessStream(command, sizeof command, false) == sizeof command, "real parser applies blend state");
}
void Decoded() {
    decoded = true;
    if (!frames && mode == "cold") {
        std::printf("cold callback: append-opens=%u writes=%u flushes=%u\n", appendOpens, appendWrites, flushes);
        Require(appendOpens == 1, "I/O fault/measurement boundary observes the actual appender");
        Require(appendWrites == 0 && flushes == 0, "small cold-frame writes wait until after producer release");
    }
}
void Observe(vgx::CmdBuf& b) {
    Require(decoded, "producer-release callback precedes frame dispatch");
    unsigned draws = 0;
    for (const auto& c : b.cmds) if (c.type == vgx::CMD_DRAW) {
        const auto& d = b.draws.at(c.arg);
        drawnVs = d.vsId; drawnFs = d.fsId;
        Require(d.idxCount == 3 && d.layoutMask == 0, "triangle geometry is unchanged"); ++draws;
    }
    Require(draws == 16, "all blend-state draws survive cache I/O failures");
    if (!frames && mode == "cold") Require(appendWrites == 0 && flushes == 0, "small cold-frame writes wait until after frame dispatch");
    submitted = true; ++frames; vgx::backend::Execute(b);
}
#if GXV_RECIPE_SEED_BATCH
void PrepareChecked() {
    FILE* f = std::fopen(listPath.c_str(), "wb");
    Header h{{'V','G','X','S','H','L','0','1'}, sizeof(vgx::VsKey), sizeof(vgx::FsKey), vgx::kShaderGenVersion, 0};
    Require(f && std::fwrite(&h, sizeof h, 1, f) == 1 && std::fclose(f) == 0, "synthetic checked branch owns an empty user file");
    vgx::g_shaderListDone = true;
    vgx::g_shaderList = std::fopen(listPath.c_str(), "ab");
    Require(vgx::g_shaderList, "checked appender opens");
    vgx::g_recipeMerged = true;
    vgx::recipe::List seed;
    vgx::VsKey vs{}; vs.numChans = 2;
    vgx::FsKey fs{}; fs.numStages = 1; fs.alphaTestOn = 1; fs.alphaTest = 123;
    seed.vs.push_back(vs); seed.fs.push_back(fs);
    seed.records.push_back(vgx::recipe::Record{1,{0,0,0,0}});
    seed.records.push_back(vgx::recipe::Record{2,{0,0,0,0}});
    vgx::g_shaderListReplay = true;
    vgx::ReplayRecipe(seed, false);
    vgx::g_shaderListReplay = false;
}
#endif
}

extern "C" FILE* __real_fopen(const char*, const char*);
extern "C" FILE* __real_fopen64(const char*, const char*);
extern "C" int __real_fclose(FILE*);
extern "C" size_t __real_fwrite(const void*, size_t, size_t, FILE*);
extern "C" int __real_fflush(FILE*);
struct ReadCookie { FILE* input; size_t left; };
static ssize_t FailRead(void* context, char* out, size_t n) {
    auto& c = *static_cast<ReadCookie*>(context);
    if (!c.left) { injected = true; errno = EIO; return -1; }
    const size_t count = std::fread(out, 1, std::min(n, c.left), c.input);
    c.left -= count;
    return static_cast<ssize_t>(count);
}
static int CloseRead(void* context) {
    auto* c = static_cast<ReadCookie*>(context);
    const int result = __real_fclose(c->input); delete c; return result;
}
static FILE* TrackOpen(FILE* f, const char* name, const char* access) {
    if (name == listPath && f) {
        if (!std::strcmp(access, "ab")) { appendFile = f; ++appendOpens; }
        else if (!std::strcmp(access, "wb")) rewriteFile = f;
        else if (mode == "read-open-fail" && !std::strcmp(access, "rb")) {
            __real_fclose(f); injected = true; errno = EACCES; return nullptr;
        }
        else if (mode == "read-fail" && !readFaultUsed && !std::strcmp(access, "rb")) {
            readFaultUsed = true;
            auto* c = new ReadCookie{f, sizeof(Header) + 4 + sizeof(vgx::VsKey) + 3};
            cookie_io_functions_t io{FailRead, nullptr, nullptr, CloseRead};
            FILE* bad = fopencookie(c, "r", io);
            Require(bad, "read-error stream is available");
            return bad;
        }
    }
    return f;
}
extern "C" FILE* __wrap_fopen(const char* name, const char* access) { return TrackOpen(__real_fopen(name, access), name, access); }
extern "C" FILE* __wrap_fopen64(const char* name, const char* access) { return TrackOpen(__real_fopen64(name, access), name, access); }
extern "C" int __wrap_fclose(FILE* f) {
    if (f == appendFile) appendFile = nullptr;
    if (f == rewriteFile) rewriteFile = nullptr;
    return __real_fclose(f);
}
extern "C" size_t __wrap_fwrite(const void* p, size_t size, size_t count, FILE* f) {
    const size_t bytes = size * count;
    if (f == appendFile) {
        ++appendWrites; largestWrite = std::max(largestWrite, bytes);
        if (failBudget != std::numeric_limits<size_t>::max()) {
            const size_t allowed = std::min(failBudget, bytes);
            const size_t written = allowed ? __real_fwrite(p, 1, allowed, f) : 0;
            failBudget -= written;
            if (written != bytes) { injected = true; errno = ENOSPC; }
            return size ? written / size : 0;
        }
    } else if (f == rewriteFile && mode == "rewrite-fail") {
        __real_fwrite(p, 1, std::min(size_t(3), bytes), f);
        injected = true; errno = ENOSPC; return 0;
    }
    return __real_fwrite(p, size, count, f);
}
extern "C" int __wrap_fflush(FILE* f) {
    if (f == appendFile) {
        ++flushes;
        if (mode == "flush-fail" && !injected) { injected = true; __real_fflush(f); errno = EIO; return EOF; }
    }
    const int result = __real_fflush(f);
    if (f == appendFile && mode == "burst" && result == 0) Read();
    return result;
}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}
static std::vector<uint8_t> RawFile() {
    FILE* f = __real_fopen64(listPath.c_str(), "rb");
    Require(f && std::fseek(f, 0, SEEK_END) == 0, "raw fixture read");
    const long n = std::ftell(f);
    Require(n >= 0 && std::fseek(f, 0, SEEK_SET) == 0, "raw fixture length");
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    Require(std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size() && __real_fclose(f) == 0, "raw fixture bytes");
    return bytes;
}
static void ReadFailureFixture() {
    FILE* f = __real_fopen64(listPath.c_str(), "wb");
    Header h{{'V','G','X','S','H','L','0','1'}, sizeof(vgx::VsKey), sizeof(vgx::FsKey), vgx::kShaderGenVersion, 0};
    vgx::VsKey vs{}; vs.numChans = 2;
    vgx::FsKey fs{}; fs.numStages = 1;
    const uint32_t vtype = 1, ftype = 2, program[] = {3,0,0}, variant[] = {4,0,0,0,0x401};
    Require(f && __real_fwrite(&h, sizeof h, 1, f) == 1 && __real_fwrite(&vtype,4,1,f) == 1 &&
            __real_fwrite(&vs,sizeof vs,1,f) == 1 && __real_fwrite(&ftype,4,1,f) == 1 &&
            __real_fwrite(&fs,sizeof fs,1,f) == 1 && __real_fwrite(program,sizeof program,1,f) == 1 &&
            __real_fwrite(variant,sizeof variant,1,f) == 1 && __real_fclose(f) == 0, "valid read-failure fixture");
}
int main(int argc, char** argv) {
    Require(argc == 3, "supply scenario and container-owned temporary path");
    mode = argv[1]; listPath = argv[2];
    const bool warm = mode == "warm", recover = mode == "recover", burst = mode == "burst";
    if (!warm && !recover && mode != "device-full") std::remove(listPath.c_str());
    std::vector<uint8_t> original;
    if (mode == "read-fail" || mode == "read-open-fail") { ReadFailureFixture(); original = RawFile(); }
    if (mode.rfind("write-fail-", 0) == 0) failBudget = std::stoul(mode.substr(11));
    setenv("GXV_SOFTXFORM", "0", 1); setenv("GXV_ATLAS", "0", 1);
    vgx::SetShaderListFile(mode == "no-list" ? nullptr : listPath.c_str());
    vgx::SetShaderWarmupDeferred(true); vgx::SetFrameSink(Observe); vgx::ResetState();
#if GXV_RECIPE_SEED_BATCH
    if (mode == "checked" || mode == "checked-fail") {
        PrepareChecked();
        if (mode == "checked-fail") failBudget = 3;
    }
#else
    Require(mode != "checked" && mode != "checked-fail", "checked branch requires recipe flag");
#endif
    unsigned writesAfterFirst = 0, flushesAfterFirst = 0;
    const float vertices[3][4] = {{0,0,0,0},{1,0,0,0},{0,1,0,0}};
    for (unsigned frame = 0; frame < 3; ++frame) {
        submitted = decoded = false;
        for (unsigned i = 0; i < 16; ++i) {
            Blend(Failure() && frame && i == 0 ? 16 : i);
            vgx::DrawDecoded(0x90, 0, reinterpret_cast<const uint8_t*>(vertices), 3);
        }
        if (burst && !frame) for (unsigned i = 1; i <= 2000; ++i) {
            const uint32_t record[] = {0, 0, i, 1};
            vgx::ListAppend(4, record, sizeof record);
        }
        if (!frame && mode == "reset") vgx::ResetState();
        if (!frame && mode == "path-change") vgx::SetShaderListFile((listPath + ".other").c_str());
        vgx::FrontPresent(true, Decoded);
        Require(submitted, "cache checkpoint does not omit frame submission");
        if (!frame) { writesAfterFirst = appendWrites; flushesAfterFirst = flushes; }
        else Require(appendWrites == writesAfterFirst && flushes == flushesAfterFirst,
                     "known or disabled learning does no later writes/flushes");
        if (Failure() || mode == "checked-fail") Require(injected && !vgx::g_shaderList, "write/flush/rewrite failure disables appending for this run");
        else if (mode == "no-list" || mode == "unwritable" || mode == "device-full") Require(appendWrites == 0 && flushes == 0, "unconfigured/unwritable cache does no writes");
        else {
            const auto s = Read();
            Require(s.keys[1] == 1 && s.keys[2] == 1 && s.keys[3] == 1 && s.keys[4] == (burst ? 2016u : 16u),
                    "exact shader/program/variant identities persist by FrontPresent return");
            // RGB8 has no alpha writes; GX source factors2/3 map to destination colour.
            const std::set<uint32_t> expected(std::begin(expectedBlends), std::end(expectedBlends));
            Require(s.blend == expected, "all independently specified blend keys persist unchanged");
            if (!recover) Require(s.wire == ExpectedWire(burst), "persisted header, key payloads and record order are byte-exact");
        }
    }
    Require(frames == 3 && largestWrite <= 16 * 1024, "frames retained and write buffer bounded to16KiB");
    if (mode == "cold" || mode == "checked" || mode == "reset" || mode == "path-change") Require(appendWrites == 1 && flushes == 1, "small learning burst uses one write and one flush");
    if (recover) Require(appendWrites <= 1 && flushes == appendWrites, "recovery writes only missing records in at most one batch");
    if (warm) Require(appendWrites == 0 && flushes == 0, "warm replay does no persistence work");
    if (burst) Require(appendWrites >= 2 && appendWrites <= 4 && flushes == appendWrites, "large learning burst uses bounded whole-record batches");
    if (mode == "rewrite-fail") Require(appendOpens == 0, "failed header/rewrite cannot be reopened for append");
    if (mode == "read-fail" || mode == "read-open-fail") Require(appendOpens == 0 && RawFile() == original, "read error neither rewrites nor appends to a possibly good file");
    if (mode == "device-full") Require(!vgx::g_shaderList && appendOpens == 0, "real ENOSPC at rewrite close disables learning");
    if (mode == "path-change") {
        FILE* other = __real_fopen64((listPath + ".other").c_str(), "rb");
        if (other) __real_fclose(other);
        Require(!other, "startup-only path selection cannot redirect queued bytes to another file");
    }
    std::printf("PASS: shader learning %s writes=%u flushes=%u largest-write=%zu size_t=%zu (synthetic I/O counts, not Vita FPS)\n",
                mode.c_str(), appendWrites, flushes, largestWrite, sizeof(size_t));
}
