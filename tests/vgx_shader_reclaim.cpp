// GXV_SHADER_RECLAIM (shader23): the real GLES back end on Mesa, with vitaGL's shader and program slot rules put in front of the GL
// (2,048 shader slots; glDeleteShader of an attached shader only marks it, the slot is freed when the last program using it is deleted;
// 1,024 program slots). A session of S stages, each with its own vertex and fragment shaders (more shaders in all than 2,048) plus a
// shared set, is played L times over; every stage visit is a scene change (closed screen), then live frames that draw every program of
// the stage into a pixel of its own.
//   vgx_shader_reclaim <mode> [stages] [loops] [report]
//   mode 1: GXV_SHADER_RECLAIM=1 (the default): no glCreateShader may return 0, the slots stay under the cap, every picture holds
//           exactly the colours of the stage's programs, and a revisit's picture is byte for byte the first visit's.
//   mode 0: the control (GXV_SHADER_RECLAIM=0): must run out of shader slots (glCreateShader = 0) in the same session, and every
//           picture before that must be byte for byte mode 1's (the report file holds the hashes; tools/test_vgx_shader_reclaim.sh
//           compares them).
//   mode tight: GXV_SHADER_RECLAIM=1 with KEEP 256 / HIGH 512: the pass must also delete other stages' programs for room; as mode 1.
//   mode emergency: GXV_SHADER_RECLAIM=1 with the scene-change pass held off (KEEP/HIGH at 4096): the slots fill in play and the
//           last-resort path inside Compile must give back what no linked program uses; pictures as mode 1.
#define main quiet_fixture_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "vgx_backend_quiet.cpp"
#pragma clang diagnostic pop
#undef main
#include <deque>
#include <map>
#include <set>

namespace {
// ---- vitaGL's slot rules (custom_shaders.c: MAX_CUSTOM_SHADERS 2048, glDeleteShader / glAttachShader / glDeleteProgram ref_counter)
constexpr unsigned kShaderSlots = 2048, kProgramSlots = 1024;
struct Slot { int refs = 0; bool flagged = false; };
std::map<GLuint, Slot> slots;                       // live vitaGL shader slots, by name
std::map<GLuint, std::vector<GLuint>> attached;     // program -> shaders attached
std::set<GLuint> programs;
unsigned shaderCreates = 0, shaderCreateFails = 0, shaderReleases = 0, peakSlots = 0, programRejects = 0, flaggedAttach = 0;
unsigned sceneKey = 0, loadingKey = 0, quietScreen = 1;
void Release(GLuint s) { slots.erase(s); ++shaderReleases; }
}
extern "C" uint32_t port_scene_key(void) { return sceneKey; }
extern "C" uint32_t port_scene_loading_key(void) { return loadingKey; }
extern "C" int port_screen_quiet(void) { return static_cast<int>(quietScreen); }
extern "C" GLuint __real_glCreateShader(GLenum);
extern "C" GLuint __wrap_glCreateShader(GLenum type) {
    if (slots.size() >= kShaderSlots) { ++shaderCreateFails; return 0; } // ("Out of shaders handles")
    const GLuint s = __real_glCreateShader(type);
    Require(s != 0 && !slots.count(s), "Mesa gives a fresh shader name");
    slots[s] = Slot{};
    ++shaderCreates;
    peakSlots = std::max(peakSlots, static_cast<unsigned>(slots.size()));
    return s;
}
extern "C" void __real_glDeleteShader(GLuint);
extern "C" void __wrap_glDeleteShader(GLuint s) {
    const auto it = slots.find(s);
    Require(it != slots.end(), "glDeleteShader of a live shader name (vitaGL would index a NULL slot)");
    Require(!it->second.flagged, "a shader is deleted once");
    if (it->second.refs > 0) it->second.flagged = true; else Release(s);
    __real_glDeleteShader(s);
}
extern "C" void __real_glAttachShader(GLuint, GLuint);
extern "C" void __wrap_glAttachShader(GLuint p, GLuint s) {
    const auto it = slots.find(s);
    Require(it != slots.end(), "glAttachShader of a live shader name (vitaGL would read a NULL slot)");
    if (it->second.flagged) ++flaggedAttach; // (works on vitaGL, but the back end must not keep names it gave back)
    ++it->second.refs;
    attached[p].push_back(s);
    __real_glAttachShader(p, s);
}
extern "C" GLuint __real_glCreateProgram();
extern "C" GLuint __wrap_glCreateProgram() {
    if (programs.size() >= kProgramSlots) { ++programRejects; return ~0u; }
    const GLuint p = __real_glCreateProgram();
    Require(p != 0, "Mesa allocates a program");
    programs.insert(p);
    return p;
}
extern "C" void __real_glDeleteProgram(GLuint);
extern "C" void __wrap_glDeleteProgram(GLuint p) {
    Require(p && p != ~0u, "a valid program is deleted");
    if (programs.erase(p)) {
        for (const GLuint s : attached[p]) {
            const auto it = slots.find(s);
            Require(it != slots.end() && it->second.refs > 0, "an attached shader is live while its program is");
            if (!--it->second.refs && it->second.flagged) Release(s);
        }
        attached.erase(p);
    }
    __real_glDeleteProgram(p);
}

namespace {
constexpr unsigned kStageVs = 4, kStageFs = 230, kCommonVs = 2, kCommonFs = 30;
std::deque<vgx::ShaderSource> sources;
struct Stage {
    uint32_t key = 0;
    std::vector<const vgx::ShaderSource*> fresh;    // registered at the first visit
    std::vector<std::pair<uint16_t, uint16_t>> progs; // (vs, fs) ids, pixel = index
    std::vector<uint32_t> colours;                  // expected RGBA of each program's pixel
    bool registered = false;
};
uint16_t nextVs = 10, nextFs = 10;
const vgx::ShaderSource* MakeVs(unsigned tag) {
    sources.emplace_back();
    vgx::ShaderSource& s = sources.back();
    s.id = nextVs++;
    s.keyHash = 0x5600000000ull + s.id;
    s.text = "attribute vec4 a_pos;\nuniform mat4 u_proj;\nvoid main() { gl_Position = u_proj * a_pos + vec4(0.0, 0.0, 0.0, 0.0 * " +
             std::to_string(tag) + ".0); }\n";
    return &s;
}
const vgx::ShaderSource* MakeFs(unsigned r, unsigned g, unsigned b) {
    sources.emplace_back();
    vgx::ShaderSource& s = sources.back();
    s.fragment = true;
    s.id = nextFs++;
    s.keyHash = 0x4600000000ull + s.id;
    char t[160];
    std::snprintf(t, sizeof t, "precision mediump float;\nvoid main() { gl_FragColor = vec4(%u.0 / 255.0, %u.0 / 255.0, %u.0 / 255.0, 1.0); }\n", r, g, b);
    s.text = t;
    return &s;
}
uint32_t Rgba(unsigned r, unsigned g, unsigned b) { return r | g << 8 | b << 16 | 0xFFu << 24; }
std::vector<Stage> MakeStages(unsigned n) {
    std::vector<Stage> stages(n);
    Stage common;
    std::vector<const vgx::ShaderSource*> cvs, cfs;
    for (unsigned v = 0; v < kCommonVs; ++v) cvs.push_back(MakeVs(9000 + v));
    for (unsigned f = 0; f < kCommonFs; ++f) cfs.push_back(MakeFs(200, 1 + f, 250));
    for (unsigned s = 0; s < n; ++s) {
        Stage& st = stages[s];
        st.key = 0x51A60000u + s;
        if (!s) { for (auto* v : cvs) st.fresh.push_back(v); for (auto* f : cfs) st.fresh.push_back(f); }
        for (unsigned f = 0; f < kCommonFs; ++f) {
            st.progs.emplace_back(cvs[f % kCommonVs]->id, cfs[f]->id);
            st.colours.push_back(Rgba(200, 1 + f, 250));
        }
        std::vector<const vgx::ShaderSource*> vs;
        for (unsigned v = 0; v < kStageVs; ++v) { vs.push_back(MakeVs(s * 16 + v)); st.fresh.push_back(vs.back()); }
        const unsigned g = 1 + (s * 13) % 199;
        for (unsigned f = 0; f < kStageFs; ++f) {
            const auto* fs = MakeFs(1 + f, g, 77);
            st.fresh.push_back(fs);
            st.progs.emplace_back(vs[f % kStageVs]->id, fs->id);
            st.colours.push_back(Rgba(1 + f, g, 77));
        }
    }
    return stages;
}
// One frame: the stage's new shaders (first visit), its draws (one pixel each), the display copy with a clear, the present.
vgx::CmdBuf Frame(Stage& st, bool draws, uint64_t frame) {
    vgx::CmdBuf b;
    b.frame = frame;
    if (!st.registered) {
        for (const auto* s : st.fresh) { b.cmds.push_back({vgx::CMD_SHADER, static_cast<uint32_t>(b.shaders.size())}); b.shaders.push_back(s); }
        st.registered = true;
    }
    b.uni = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (draws) {
        for (unsigned i = 0; i < st.progs.size(); ++i) {
            const unsigned x = 2 + i % 28, y = 2 + i / 28; // (the 2 x 2 corner at the origin is not used: the host's present leaves it out)
            const float x0 = x / 16.0f - 1.0f, x1 = (x + 1) / 16.0f - 1.0f, y0 = y / 16.0f - 1.0f, y1 = (y + 1) / 16.0f - 1.0f;
            const float quad[4][4] = {{x0, y0, 0, 1}, {x1, y0, 0, 1}, {x1, y1, 0, 1}, {x0, y1, 0, 1}};
            const uint32_t first = static_cast<uint32_t>(b.vtx.size() / 16);
            const size_t at = b.vtx.size();
            b.vtx.resize(at + sizeof quad);
            std::memcpy(b.vtx.data() + at, quad, sizeof quad);
            vgx::DrawRec d;
            d.vsId = st.progs[i].first;
            d.fsId = st.progs[i].second;
            d.idxOffset = static_cast<uint32_t>(b.idx.size());
            for (const unsigned k : {0u, 1u, 2u, 0u, 2u, 3u}) b.idx.push_back(static_cast<uint16_t>(first + k));
            d.idxCount = 6;
            d.vtxFirst = first;
            d.vtxCount = 4;
            d.ublock[vgx::UB_PROJ] = 0;
            b.cmds.push_back({vgx::CMD_DRAW, static_cast<uint32_t>(b.draws.size())});
            b.draws.push_back(d);
        }
    }
    vgx::CopyDispCmd copy;
    copy.src = {0, 0, 32, 32};
    copy.clear.enable = true;
    b.copyDisp.push_back(copy);
    b.cmds.push_back({vgx::CMD_COPY_DISP, 0});
    b.cmds.push_back({vgx::CMD_PRESENT, 0});
    return b;
}
uint64_t Hash(const Pixels& p) {
    uint64_t h = 1469598103934665603ull;
    for (const uint8_t c : p) { h ^= c; h *= 1099511628211ull; }
    return h;
}
// The picture holds exactly the stage's colours, one pixel each, and black elsewhere.
bool Holds(const Pixels& p, const Stage& st) {
    std::multiset<uint32_t> want(st.colours.begin(), st.colours.end()), got;
    for (size_t i = 0; i < p.size(); i += 4) {
        const uint32_t c = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | static_cast<uint32_t>(p[i + 3]) << 24;
        if ((c & 0xFFFFFFu) != 0) got.insert(c);
    }
    return want == got;
}
}

int main(int argc, char** argv) {
    Require(argc >= 2, "mode 0, 1 or emergency");
    const std::string mode = argv[1];
    const unsigned stageCount = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 12;
    const unsigned loops = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 3;
    const char* report = argc > 4 ? argv[4] : nullptr;
    for (const auto* name : diagnosticNames) unsetenv(name);
    for (const auto* name : functionalNames) unsetenv(name);
    setenv("GXV_AUTOQ", "0", 1);
    setenv("GXV_MAXPROGRAMS", "900", 1);
    if (mode == "0") setenv("GXV_SHADER_RECLAIM", "0", 1);
    else if (mode == "1") setenv("GXV_SHADER_RECLAIM", "1", 1);
    else if (mode == "tight") { setenv("GXV_SHADER_RECLAIM", "1", 1); setenv("GXV_SHADER_RECLAIM_KEEP", "256", 1); setenv("GXV_SHADER_RECLAIM_HIGH", "512", 1); }
    else if (mode == "emergency") { setenv("GXV_SHADER_RECLAIM", "1", 1); setenv("GXV_SHADER_RECLAIM_KEEP", "4096", 1); setenv("GXV_SHADER_RECLAIM_HIGH", "4096", 1); }
    else Require(false, "known mode");
    Context();
    Require(vgx::backend::Init(32, 32), "real backend initialized");
    using namespace vgx::backend;
    std::vector<Stage> stages = MakeStages(stageCount);
    std::map<uint32_t, uint64_t> firstHash;
    FILE* out = report ? std::fopen(report, "w") : nullptr;
    uint64_t frame = 1;
    unsigned visits = 0, mismatches = 0, wrong = 0, firstFailVisit = 0, maxHeldAtScene = 0, maxSlotsAtScene = 0;
    for (unsigned loop = 0; loop < loops; ++loop) {
        for (Stage& st : stages) {
            ++visits;
            // scene change: the stage loads behind a closed screen (one frame), then 3 live frames; ~12 s of play between visits
            g_serial += 700;
            loadingKey = st.key; quietScreen = 1;
            Execute(Frame(st, false, frame++));
            loadingKey = 0; sceneKey = st.key; quietScreen = 0;
            maxHeldAtScene = std::max(maxHeldAtScene, GetStats().shadersHeld);
            maxSlotsAtScene = std::max(maxSlotsAtScene, static_cast<unsigned>(slots.size()));
            Pixels pic{};
            for (int f = 0; f < 3; ++f) { Execute(Frame(st, true, frame++)); pic = Read(); }
            const uint64_t h = Hash(pic);
            const bool holds = Holds(pic, st);
            if (!holds && std::getenv("RECLAIM_DUMP") && visits == 1) {
                char line[64];
                for (size_t i = 0; i < pic.size(); i += 4) {
                    const int n = std::snprintf(line, sizeof line, "%zu: %u %u %u %u\n", i / 4, pic[i], pic[i + 1], pic[i + 2], pic[i + 3]);
                    ::write(STDERR_FILENO, line, n);
                }
            }
            if (!holds) ++wrong;
            if (!holds && !firstFailVisit) firstFailVisit = visits;
            const auto seen = firstHash.find(st.key);
            if (seen == firstHash.end()) firstHash[st.key] = h;
            else if (seen->second != h) ++mismatches;
            if (out) std::fprintf(out, "visit %u loop %u stage %08x hash %016llx holds %d fails %u held %u slots %u\n", visits, loop, st.key,
                                  static_cast<unsigned long long>(h), holds ? 1 : 0, shaderCreateFails, GetStats().shadersHeld,
                                  static_cast<unsigned>(slots.size()));
        }
    }
    if (out) {
        for (const std::string& line : fixture::summaryLines)
            if (line.find("[shader-reclaim]") != std::string::npos) std::fprintf(out, "log: %s\n", line.c_str());
        std::fclose(out);
    }
    const Stats s = GetStats();
    Require(glGetError() == GL_NO_ERROR, "the session leaves the real GL valid");
    char result[1024];
    const int length = std::snprintf(result, sizeof result,
        "{\"mode\":\"%s\",\"stages\":%u,\"loops\":%u,\"visits\":%u,\"distinct_shaders\":%u,\"shader_creates\":%u,\"create_fails\":%u,"
        "\"releases\":%u,\"peak_slots\":%u,\"max_slots_at_scene\":%u,\"max_held_at_scene\":%u,\"held_end\":%u,\"held_peak\":%u,\"reclaimed\":%u,"
        "\"passes\":%u,\"reclaim_program_deletes\":%u,\"backend_create_fails\":%u,\"program_rejects\":%u,\"flagged_attach\":%u,"
        "\"wrong_pictures\":%u,\"first_wrong_visit\":%u,\"revisit_mismatches\":%u,\"programs_linked\":%u}\n",
        mode.c_str(), stageCount, loops, visits, static_cast<unsigned>(sources.size()), shaderCreates, shaderCreateFails, shaderReleases, peakSlots,
        maxSlotsAtScene, maxHeldAtScene, s.shadersHeld, s.shadersHeldPeak, s.shadersReclaimed, s.shaderReclaimPasses, s.shaderReclaimProgramDeletes,
        s.shaderCreateFails, programRejects, flaggedAttach, wrong, firstFailVisit, mismatches, s.programLinks);
    Require(length > 0 && static_cast<size_t>(length) < sizeof result, "bounded report");
    Require(::write(STDOUT_FILENO, result, length) == length, "report written");
    Require(sources.size() > kShaderSlots, "the session has more distinct shaders than vitaGL has slots");
    if (mode == "0") {
        Require(shaderCreateFails > 0 && wrong > 0, "control: without reclaim the slots run out and pictures lose draws");
        Require(s.shadersReclaimed == 0 && s.shaderReclaimPasses == 0, "control: GXV_SHADER_RECLAIM=0 gives nothing back");
    } else {
        if (mode == "1") Require(shaderCreateFails == 0 && s.shaderCreateFails == 0, "no glCreateShader returned 0");
        Require(wrong == 0, "every picture holds exactly its stage's programs' colours");
        Require(mismatches == 0, "every revisit's picture is byte for byte the first visit's");
        Require(flaggedAttach == 0, "the back end never attaches a shader it gave back");
        Require(peakSlots <= kShaderSlots, "slots within vitaGL's cap");
        if (mode == "1") Require(maxHeldAtScene <= 1408 + 2 * (kStageVs + kStageFs), "the scene-change pass keeps the held count bounded");
        if (mode == "tight") Require(s.shaderReclaimProgramDeletes > 0 && maxHeldAtScene <= 512 + 2 * (kStageVs + kStageFs + kCommonVs + kCommonFs),
                                     "above the mark the pass deletes other stages' programs and keeps the held count bounded");
        if (mode == "emergency") Require(s.shaderCreateFails > 0 && s.shadersReclaimed > 0, "the in-play last resort ran and gave shaders back");
    }
    return 0;
}
