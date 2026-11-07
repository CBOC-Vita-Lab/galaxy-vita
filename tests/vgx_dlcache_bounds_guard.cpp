// Real parser/cache/emitter: an unreadable indexed array must not poison later geometry.
#include "vgx_state.h"
#include "vgx_guestmem.h"
#include "vgx_hle.h"

#include <map>
#include <string>

extern "C" int64_t port_host_time_ns() { return 0; }
extern "C" int64_t port_thread_cpu_us() { return 0; }
extern "C" void port_log(const char*, ...) {}
namespace vgx {
bool LogEnabled() { return false; }
void SubmitToBackend(CmdBuf& b) { backend::Execute(b); }
}

namespace {
alignas(4096) uint8_t ram[16384] = {};
std::map<unsigned, vgx::BufUpload> buffers;
std::string scenario;
unsigned frame = 0, failures = 0, pictures = 0, staticDraws = 0;
bool guard = false, cacheOff = false, priming = true;
uint64_t geometryHash = 14695981039346656037ull;

void Require(bool ok, const char* message) {
    if (!ok) {
        if (failures < 12) std::fprintf(stderr, "FAIL: %s frame=%u %s\n", scenario.c_str(), frame, message);
        ++failures;
    }
}

const uint8_t* Resolve(uint32_t addr, uint32_t* size) {
    *size = 0;
    if (addr == 0x200 || addr == 0x400) { *size = 36; return ram + 4096; }
    if (addr == 0x300) { *size = 12; return ram + 4096; }
    if (addr == 0x100) {
        if (scenario == "partial") { *size = 12; return ram + 4096; }
        if (scenario == "shrink") { *size = 36; return ram + 4096; }
        if (scenario == "valid-rebind") { *size = 36; return ram + 8192; }
    }
    return nullptr;
}

float ExpectedX(unsigned index) {
    if (priming || scenario == "direct" || scenario == "valid") return float(index + 1);
    if (scenario == "valid-rebind") return float(index + (frame < 2 ? 7 : 1));
    const bool legacy = !guard && !cacheOff;
    if (scenario == "unmapped") return frame < 2 || legacy ? 0.0f : float(index + 1);
    if (scenario == "partial") return (frame < 2 || legacy) && index != 0 ? 0.0f : float(index + 1);
    // The source changes only for the formerly unreadable elements after recovery.
    if (frame < 2) return float(index + 1);
    if (index == 0) return 1.0f;
    return frame < 4 || legacy ? 0.0f : float(index + 101);
}

void Observe(vgx::CmdBuf& b) {
    unsigned vertices = 0, bad = 0;
    for (const auto& c : b.cmds) {
        if (c.type == vgx::CMD_BUF_UPLOAD) {
            const auto& up = b.bufUploads.at(c.arg);
            buffers[up.bufId] = up;
        } else if (c.type == vgx::CMD_BUF_FREE) {
            buffers.erase(c.arg);
        } else if (c.type == vgx::CMD_DRAW) {
            const auto& d = b.draws.at(c.arg);
            Require(d.prim == vgx::PRIM_TRIANGLES && d.layoutMask == 0, "triangle position-only layout");
            const uint8_t* v;
            const uint16_t* idx;
            size_t vertexBytes, indexCount;
            if (d.vbuf) {
                ++staticDraws;
                const auto& up = buffers.at(d.vbuf);
                v = up.vtx.data(); idx = up.idx.data();
                vertexBytes = up.vtx.size(); indexCount = up.idx.size();
            } else {
                const size_t chunk = b.vtxChunks.empty() ? 0 : b.vtxChunks.at(d.vchunk);
                v = b.vtx.data() + chunk; idx = b.idx.data();
                vertexBytes = b.vtx.size() - chunk; indexCount = b.idx.size();
            }
            Require(size_t(d.idxOffset) + d.idxCount <= indexCount, "index bounds");
            if (size_t(d.idxOffset) + d.idxCount > indexCount) continue;
            for (unsigned i = 0; i < d.idxCount; ++i) {
                const size_t offset = d.vtxOffset + size_t(idx[d.idxOffset + i]) * 16;
                Require(offset + 16 <= vertexBytes, "vertex bounds");
                if (offset + 16 > vertexBytes) continue;
                float pos[4];
                std::memcpy(pos, v + offset, sizeof pos);
                const float x = ExpectedX(vertices % 3);
                bad += pos[0] != x || pos[1] != (x ? x + 10.0f : 0.0f) ||
                       pos[2] != (x ? x + 20.0f : 0.0f) || pos[3] != 0.0f;
                for (unsigned byte = 0; byte < 16; ++byte)
                    geometryHash = (geometryHash ^ v[offset + byte]) * 1099511628211ull;
                ++vertices;
            }
        }
    }
    Require(vertices == 48, "all 48 indexed vertices emitted");
    Require(bad == 0, "geometry matches current bindings, or exact legacy path when disabled");
    ++pictures;
    vgx::backend::Execute(b);
}

void Bind(unsigned base) {
    vgx::WriteCP(0xA0, base);
}

void Position(uint8_t* p, unsigned index, unsigned extra) {
    const float pos[3] = {float(index + 1 + extra), float(index + 11 + extra), float(index + 21 + extra)};
    std::memcpy(p + index * 12, pos, sizeof pos);
}

unsigned MakeList(uint8_t* list, bool direct) {
    unsigned at = 0;
    list[at++] = 0x90; list[at++] = 0; list[at++] = 48;
    for (unsigned i = 0; i < 48; ++i) {
        if (direct) {
            for (unsigned c = 0; c < 3; ++c) {
                const uint32_t word = vgx::f2u(float(i % 3 + 1 + c * 10));
                for (unsigned b = 0; b < 4; ++b) list[at++] = static_cast<uint8_t>(word >> (24 - b * 8));
            }
        } else {
            list[at++] = 0;
            list[at++] = static_cast<uint8_t>(i % 3);
        }
    }
    return at;
}
}

int main(int argc, char** argv) {
    Require(argc == 3 || argc == 4, "usage: bounds-guard on|off scenario [flip]");
    if (failures) return 1;
    guard = std::string(argv[1]) == "on";
    scenario = argv[2];
    Require(scenario == "unmapped" || scenario == "partial" || scenario == "shrink" ||
            scenario == "valid-rebind" || scenario == "valid" || scenario == "direct", "known scenario");
    if (failures) return 1;
    cacheOff = std::getenv("GXV_DLCACHE") && std::string(std::getenv("GXV_DLCACHE")) == "0";
    setenv("GXV_ATLAS", "0", 1);
    setenv("GXV_SOFTXFORM", "0", 1);
    setenv("GXV_FOLDREGS", "0", 1);
    vgx::ResetState();
    vgx::GuestHooks hooks{};
    hooks.resolve = Resolve;
    vgx::SetGuestHooks(hooks);
    vgx::WriteCP(0x50, (scenario == "direct" ? 1u : 3u) << 9);
    vgx::WriteCP(0x70, 1u | (4u << 1));
    vgx::WriteCP(0xB0, 12);
    const unsigned listBytes = MakeList(ram, scenario == "direct");
    std::memcpy(ram + 1024, ram, listBytes);
    for (unsigned i = 0; i < 3; ++i) {
        Position(ram + 4096, i, 0);
        Position(ram + 8192, i, 6);
    }
    vgx::hle::RegisterGuestRam(ram, sizeof ram);
    vgx::SetFrameSink(Observe);
    Bind(0x200);
    vgx::CallList(ram + 1024, listBytes);
    vgx::FrontPresent(true, nullptr);
    priming = false;
    // The first valid build above latches the switch before a different list tests it.
    if (argc == 4) setenv("GXV_DLCACHE_BOUNDS_GUARD", guard ? "0" : "1", 1);
    Bind(scenario == "valid" || scenario == "direct" ? 0x200 : 0x100);
    const unsigned frames = scenario == "shrink" ? 610 : 6;
    for (frame = 0; frame < frames; ++frame) {
        if (frame == 2) Bind(scenario == "shrink" ? 0x300 : 0x200);
        if (scenario == "shrink" && frame == 4) {
            Position(ram + 4096, 1, 100);
            Position(ram + 4096, 2, 100);
            vgx::hle::NotifyGuestWrite(ram + 4096 + 12, 24);
            Bind(0x400);
        }
        vgx::CallList(ram, listBytes);
        vgx::FrontPresent(true, nullptr);
    }
    const bool staticEnabled = !std::getenv("GXV_STATICBUF") || std::string(std::getenv("GXV_STATICBUF")) != "0";
    if (!cacheOff && staticEnabled && (scenario == "valid" || scenario == "direct" || scenario == "valid-rebind"))
        Require(staticDraws > 0, "fully valid lists still promote and reuse static buffers");
    std::printf("scenario=%s pictures=%u static-draws=%u geometry=%016llx failures=%u\n",
                scenario.c_str(), pictures, staticDraws, static_cast<unsigned long long>(geometryHash), failures);
    vgx::DlCacheReport(stdout);
    return failures ? 1 : 0;
}
