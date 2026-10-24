#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <chrono>
#include <map>
#include <string>

// Real front end and null backend; only the asynchronous host handoff is local.
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
alignas(4096) uint8_t ram[16384] = {};
unsigned strips = 80, vertices = strips * 4, listBytes = strips * 15;
std::map<unsigned, vgx::BufUpload> staticBuffers;
uint64_t arenaBytes = 0, arenaIndices = 0, expectedHash = 0;
unsigned pictures = 0, staticPictures = 0;
unsigned uploads = 0, frees = 0;

void Require(bool ok, const char* msg) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); std::exit(1); }
}
void Put32(uint8_t* p, uint32_t v) {
    for (unsigned i = 0; i != 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

uint64_t SourceGeometryHash() {
    uint64_t h = 14695981039346656037ull;
    constexpr unsigned order[] = {0, 1, 2, 2, 1, 3};
    for (unsigned s = 0; s != strips; ++s) {
        for (unsigned j : order) {
            const unsigned i = s * 4 + j;
            float pos[4] = {vgx::bef32(ram + 4096 + i * 12), vgx::bef32(ram + 4100 + i * 12),
                            vgx::bef32(ram + 4104 + i * 12), 0.0f};
            for (uint8_t x : reinterpret_cast<const uint8_t (&)[16]>(pos)) h = (h ^ x) * 1099511628211ull;
        }
    }
    return h;
}

// Canonical indexed vertex stream, independent of static/dynamic buffer layout.
uint64_t GeometryHash(const vgx::CmdBuf& b) {
    uint64_t h = 14695981039346656037ull;
    auto byte = [&](uint8_t x) { h = (h ^ x) * 1099511628211ull; };
    unsigned count = 0;
    for (const auto& c : b.cmds) {
        if (c.type == vgx::CMD_BUF_UPLOAD) {
            const auto& up = b.bufUploads.at(c.arg);
            staticBuffers[up.bufId] = up;
        } else if (c.type == vgx::CMD_BUF_FREE) {
            staticBuffers.erase(c.arg);
        } else if (c.type == vgx::CMD_DRAW) {
            const auto& d = b.draws.at(c.arg);
            Require(d.prim == vgx::PRIM_TRIANGLES, "fixture primitive kind");
            const uint8_t* v = nullptr;
            const uint16_t* idx = nullptr;
            size_t vsize = 0, isize = 0;
            if (d.vbuf) {
                const auto& up = staticBuffers.at(d.vbuf);
                v = up.vtx.data(); vsize = up.vtx.size();
                idx = up.idx.data(); isize = up.idx.size();
                const uint8_t* keptVtx = nullptr;
                const uint16_t* keptIdx = nullptr;
                const bool kept = vgx::StaticBufferData(d.vbuf, keptVtx, keptIdx);
#if defined(PORT_RUNTIME_DIAGNOSTICS) && !PORT_RUNTIME_DIAGNOSTICS
                Require(!kept, "quiet content-hash selector retains no unused static-buffer copy");
#else
                Require(kept == (std::atoi(std::getenv("GXV_HASH")) == 2), "content-hash mode retains exactly the required static-buffer copy");
#endif
                if (kept) Require(std::memcmp(keptVtx, v, vsize) == 0 && std::memcmp(keptIdx, idx, isize * 2) == 0,
                                  "kept hash geometry matches the uploaded static buffer");
            } else {
                const size_t chunk = b.vtxChunks.empty() ? 0 : b.vtxChunks.at(d.vchunk);
                v = b.vtx.data() + chunk; vsize = b.vtx.size() - chunk;
                idx = b.idx.data(); isize = b.idx.size();
            }
            const auto layout = vgx::MakeLayout(d.layoutMask);
            Require(d.idxOffset + d.idxCount <= isize, "index bounds");
            for (unsigned i = 0; i != d.idxCount; ++i) {
                const size_t off = d.vtxOffset + static_cast<size_t>(idx[d.idxOffset + i]) * layout.stride;
                Require(off + layout.stride <= vsize, "vertex bounds");
                for (unsigned j = 0; j != layout.stride; ++j) byte(v[off + j]);
                ++count;
            }
        }
    }
    Require(count == strips * 6, "all triangle vertices retained");
    uint64_t liveStaticBytes = 0;
    for (const auto& pair : staticBuffers) liveStaticBytes += pair.second.vtx.size() + pair.second.idx.size() * 2;
    Require(vgx::DlCacheStaticBytes() == liveStaticBytes, "static-byte accounting balances uploads, frees and recycled ids");
    Require(vgx::DlCacheLiveBytes() < 1024 * 1024, "decoded-byte accounting stays bounded without unsigned underflow");
    return h;
}

void Observe(vgx::CmdBuf& b) {
    const uint64_t h = GeometryHash(b);
    expectedHash = SourceGeometryHash();
    Require(h == expectedHash, "geometry matches current guest source, including after a real edit");
    ++pictures;
    bool usesStatic = false;
    for (const auto& d : b.draws) usesStatic |= d.vbuf != 0;
    staticPictures += usesStatic;
    for (const auto& c : b.cmds) {
        uploads += c.type == vgx::CMD_BUF_UPLOAD;
        frees += c.type == vgx::CMD_BUF_FREE;
    }
    arenaBytes += b.vtx.size();
    arenaIndices += b.idx.size();
    vgx::backend::Execute(b);
}
}

int main(int argc, char** argv) {
    Require(argc >= 2, "usage: vgx_dlcache mode");
    const std::string mode = argv[1];
    const unsigned editPeriod = mode == "periodic-3" ? 3u : mode == "periodic-4" ? 4u : 0u;
    const bool mustPromote = true;
    if (mode == "small-list") { strips = 16; vertices = strips * 4; listBytes = strips * 15; }
    setenv("GXV_ATLAS", "0", 1);
    if (!std::getenv("GXV_HASH")) setenv("GXV_HASH", "0", 1);
    if (!std::getenv("GXV_DLCACHE")) setenv("GXV_DLCACHE", "1", 1);
    setenv("GXV_STATICBUF", mode == "static-disabled" ? "0" : "1", 1);
    vgx::ResetState();
    vgx::g.cpArrayStale = 0;
    vgx::SetVcd(0, 1);
    vgx::SetVcd(9, 3);
    vgx::SetVat(0, 9, 1, 4, 0);
    vgx::g.arrays[9] = {ram + 4096, vertices * 12, 12, false};
    unsigned off = 0;
    for (unsigned s = 0; s != strips; ++s) {
        ram[off++] = 0x98; ram[off++] = 0; ram[off++] = 4;
        for (unsigned j = 0; j != 4; ++j) {
            const unsigned i = s * 4 + j;
            ram[off++] = 0;
            ram[off++] = static_cast<uint8_t>(i >> 8);
            ram[off++] = static_cast<uint8_t>(i);
            for (unsigned k = 0; k != 3; ++k)
                Put32(ram + 4096 + i * 12 + k * 4, vgx::f2u(static_cast<float>(i * 3 + k) / 1024.0f));
        }
    }
    Require(off == listBytes, "list fixture size");
    if (mode == "age-out") std::memcpy(ram + 8192, ram, listBytes);
    vgx::hle::RegisterGuestRam(ram, sizeof ram);
    vgx::SetFrameSink(Observe);
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned f = 0; f != 1000; ++f) {
        if (f) {
            uint8_t* changed = nullptr;
            if (mode == "list-page") changed = ram + 2048;
            else if (mode == "array-page") changed = ram + 4096 + 4000;
            else if (mode == "other-page") changed = ram + 8192;
            else if (mode == "warm-then-noise" && f > 1) changed = ram + 2048;
            else if (mode == "alternate-edit" && (f & 1)) changed = ram + 2048;
            else if (mode == "change-then-noise" && f != 10) changed = ram + 2048;
            else if (editPeriod && f % editPeriod) changed = ram + 2048;
            if (changed) { ++*changed; vgx::hle::NotifyGuestWrite(changed, 1); }
            else if (mode == "same-list-flush") vgx::hle::NotifyGuestWrite(ram, listBytes);
            else if ((mode == "change-once" && f == 10) || mode == "change-each" ||
                     mode == "alternate-edit" || (mode == "change-then-noise" && f == 10)) {
                Put32(ram + 4096, vgx::f2u(1.0f + static_cast<float>(f) / 1024.0f));
                vgx::hle::NotifyGuestWrite(ram + 4096, 4);
            }
            if (editPeriod && f % editPeriod == 0) {
                Put32(ram + 4096, vgx::f2u(1.0f + static_cast<float>(f) / 1024.0f));
                vgx::hle::NotifyGuestWrite(ram + 4096, 4);
            }
            if (mode == "alternate-clean" && !(f & 1)) {
                Put32(ram + 4096, vgx::f2u(1.0f + static_cast<float>(f) / 1024.0f));
                vgx::hle::NotifyGuestWrite(ram + 4096, 4);
            }
        }
        const uint8_t* list = mode == "age-out" && f >= 2 && f < 900 ? ram + 8192 : ram;
        vgx::CallList(list, listBytes);
        vgx::FrontPresent(true, nullptr);
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - begin).count();
    std::printf("mode=%s pictures=%u static-pictures=%u dynamic-vertex-bytes=%llu dynamic-index-elements=%llu geometry=%016llx host-ms=%.3f\n",
                mode.c_str(), pictures, staticPictures, static_cast<unsigned long long>(arenaBytes),
                static_cast<unsigned long long>(arenaIndices), static_cast<unsigned long long>(expectedHash), ns / 1e6);
    vgx::DlCacheReport(stdout);
    if (editPeriod) {
        Require(uploads <= pictures / editPeriod + 2 && frees <= uploads,
                "periodic-edit churn is bounded and upload/free accounting remains balanced");
        std::printf("PASS: periodic edit every %u frames: %u uploads, %u frees; residual churn is not a Vita timing result\n",
                    editPeriod, uploads, frees);
        return 0;
    }
    if (mustPromote) {
        if (mode == "alternate-edit" || mode == "alternate-clean") {
            Require(uploads <= 1 && frees <= 1 && staticPictures <= 1,
                    "alternating edits/page noise must not allocate and free a static buffer every other frame");
            return 0;
        }
        if (mode == "change-then-noise") {
            Require(uploads <= 2 && staticPictures >= pictures - 3,
                    "a once-edited entry still promotes promptly after its content stabilizes");
            return 0;
        }
        const unsigned expected = (mode == "change-each" || mode == "small-list" || mode == "static-disabled") ? 0 :
                                  pictures - (mode == "age-out" || mode == "change-once" ? 3 : 1);
        Require(staticPictures == expected, "static-buffer reuse contract");
    }
}
