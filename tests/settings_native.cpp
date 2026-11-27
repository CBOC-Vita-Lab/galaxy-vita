// Tests of the native Settings model (platform/include/port/settings_native.h) on the memory files of player_settings_files.h.
#include "port/settings_native.h"
#include "port/mode_keys.h"
#include "player_settings_files.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <sstream>
using namespace port;
using namespace port::settings;

static const char* kPath = "mem/player-settings.cfg";
static const std::string kBak = std::string(kPath) + ".bak", kTmp = std::string(kPath) + ".tmp";
static const std::vector<std::string> kLangs{"English", "French", "German"};
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::abort(); } } while (0)

// A host over a UiFiles; calls are counted and the Nth can be made to fail (every FileOps function).
struct Faulty {
    UiFiles inner; FileOps innerOps; unsigned calls = 0, failAt = 0; FileOps ops;
    bool Hit() { ++calls; return failAt && calls == failAt; }
    static Faulty& G(void* p) { return *static_cast<Faulty*>(p); }
    Faulty() {
        innerOps = inner.ops();
        ops = {this,
            [](void* p, const char* n) -> int64_t { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.openRead(f.innerOps.context, n); },
            [](void* p, const char* n) -> int64_t { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.openWrite(f.innerOps.context, n); },
            [](void* p, int64_t h, void* b, size_t z) -> int64_t { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.read(f.innerOps.context, h, b, z); },
            [](void* p, int64_t h, const void* b, size_t z) -> int64_t { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.write(f.innerOps.context, h, b, z); },
            [](void* p, int64_t h) -> int { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.sync(f.innerOps.context, h); },
            [](void* p, int64_t h) -> int { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.close(f.innerOps.context, h); },
            [](void* p, const char* n, FileInfo& i) -> int { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.stat(f.innerOps.context, n, i); },
            [](void* p, const char* a, const char* b) -> int { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.rename(f.innerOps.context, a, b); },
            [](void* p, const char* n) -> int { auto& f = G(p); return f.Hit() ? -EIO : f.innerOps.remove(f.innerOps.context, n); }};
    }
};
// The fake live table: a value index per row, a log of the sets, and switches to make a set fail.
struct FakeLive {
    int val[kNativeRows]; unsigned sets = 0, commits = 0, binds = 0; bool failSet = false, bindsOk = true; NativeRow lastRow = NativeRow::Picture; int lastIndex = -1;
    NativeLiveOps ops;
    FakeLive() {
        for (auto& v : val) v = -1;   // -1: no reader (hidden)
        val[(int)NativeRow::CameraZoom] = 5; val[(int)NativeRow::RightStick] = 0; val[(int)NativeRow::InvertCamera] = 0; val[(int)NativeRow::RearTouch] = 0;
        val[(int)NativeRow::TapToJump] = 1; val[(int)NativeRow::StarBitReach] = 0;
        ops = {this,
            [](void* p, NativeRow r) -> int { return static_cast<FakeLive*>(p)->val[(int)r]; },
            [](void* p, NativeRow r, int i) -> bool { auto& f = *static_cast<FakeLive*>(p); ++f.sets; f.lastRow = r; f.lastIndex = i; if (f.failSet) return false; f.val[(int)r] = i; return true; },
            [](void* p) { ++static_cast<FakeLive*>(p)->commits; },
            [](void* p, const Config&, controls::ProfileId) -> bool { auto& f = *static_cast<FakeLive*>(p); ++f.binds; return f.bindsOk; },
            nullptr};
    }
};
// strong definitions of the two weak platform functions the model reaches (os_alarm.cpp, dvd.cpp are not linked here)
static bool gAsyncOn = false; static std::map<std::string, std::string> gAsyncData; static std::string gFallback;
extern "C" int port_card_write_async(const char* path, int, const void* data, unsigned bytes) { if (!gAsyncOn) return 0; gAsyncData[path].assign(static_cast<const char*>(data), bytes); return 1; }
extern "C" void port_language_set_fallback(const char* name) { gFallback = name; }
static bool gIdle = true; static const char* gSysLang = "english"; static std::vector<std::string> gEnv;
struct Rig {
    Faulty files; std::vector<Override> overrides; Config boot; NativeHost host{}; NativeModel model; FakeLive live; bool withLive = true; std::vector<std::string> langs = kLangs;
    Rig(bool fixed30 = true, bool motion = true) {
        boot = DefaultConfig(controls::GetControlApi());
        gIdle = true; gSysLang = "english"; gEnv.clear(); gAsyncOn = false; gAsyncData.clear(); gFallback.clear(); NativeBootStateClearForTest(); NativeHostSetForTest(nullptr);
        host = {kPath, &files.ops, &boot, &overrides, []() { return false; }, true, fixed30, motion, &live.ops,
                []() { return gIdle; }, []() -> const char* { return gSysLang; },
                [](const char* k) { for (auto& e : gEnv) if (e == k) return true; return false; }, false};
    }
    StoreResult Open() { if (!withLive) host.live = nullptr; return model.Open(host, langs); }
};
static std::string Text(const Config& c, const std::string& base = "") {
    Document d{DefaultConfig(controls::GetControlApi()), base, true};
    std::string out; auto r = SerializeDocument(d, c, controls::GetControlApi(), out); CHECK(r.status == StoreStatus::Ok); return out;
}
static Config Load(Faulty& f, StoreStatus* status = nullptr) {
    Document d; auto r = LoadDocument(kPath, controls::GetControlApi(), f.ops, d); if (status) *status = r.status; return d.config;
}
static std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> v; std::istringstream in(s); std::string l; while (std::getline(in, l)) v.push_back(l); return v;
}
static Config Technical() {   // every technical key away from its default
    Config c = DefaultConfig(controls::GetControlApi());
    c.autoQuality = AutoQuality::Disabled; c.aboveNative = false; c.prioritizeMario = false; c.antiAliasing = AntiAliasing::Off;
    c.sharpness = Sharpness::Strong; c.hdTextures = false; c.effects = EffectsQuality::Full; c.depthOfField = false; c.coreHelp = false;
    c.gxmDirect = false; c.logicOverlap = true; c.overlapMax = false; c.lazyMaterials = false; c.farAnimation = FarAnimation::Aggressive;
    c.fullSpeed = false; c.profileChosen = true;
    return c;
}
static void Put(Rig& r, const std::string& text) { r.files.inner.data[kPath] = text; }

static void SimpleRows() {
    const NativeRow rows[] = {NativeRow::Picture, NativeRow::Language, NativeRow::LessMotion, NativeRow::Tilt, NativeRow::TiltStrength, NativeRow::Pointer};
    for (NativeRow row : rows) {
        Rig r; auto st = r.Open(); CHECK(st.status == StoreStatus::Missing && r.model.Editable() && !r.model.Dirty());
        CHECK(r.model.RowShown(row) && r.model.CanStep(row));
        const unsigned before = r.model.ValueIndex(row);
        CHECK(r.model.Step(row, +1) == StepResult::Changed);
        const unsigned after = r.model.ValueIndex(row);
        CHECK(after != before && r.model.Dirty());
        CHECK(*r.model.ValueKey(row, after) && std::string(r.model.ValueKey(row, after)) != r.model.ValueKey(row, before));
        CHECK(!r.model.RowInfo(row).live == (row == NativeRow::Picture || row == NativeRow::Language));
        CHECK(r.model.Save().status == StoreStatus::Ok && !r.model.Dirty());
        Rig again; again.files.inner.data = r.files.inner.data; auto st2 = again.Open(); CHECK(st2.status == StoreStatus::Ok);
        CHECK(again.model.ValueIndex(row) == after && !again.model.Dirty());
        // step back restores the default
        CHECK(again.model.Step(row, -1) == StepResult::Changed && again.model.ValueIndex(row) == before);
    }
    // values
    Rig r; r.Open();
    CHECK(r.model.ValueCount(NativeRow::Picture) == 2 && r.model.ValueCount(NativeRow::Language) == 4 && r.model.ValueCount(NativeRow::TiltStrength) == 10);
    CHECK(std::string(r.model.ValueKey(NativeRow::Language, 0)) == "system" && std::string(r.model.ValueKey(NativeRow::Language, 1)) == "name");
    CHECK(r.model.ValueIndex(NativeRow::TiltStrength) == 4 && std::string(r.model.ValueKey(NativeRow::TiltStrength, 4)) == "5");
    CHECK(std::string(r.model.ValueKey(NativeRow::Picture, 0)) == "smooth" && std::string(r.model.ValueKey(NativeRow::Picture, 1)) == "detailed");
    CHECK(std::string(r.model.ValueKey(NativeRow::Pointer, 1)) == "quick" && std::string(r.model.ValueKey(NativeRow::Tilt, 1)) == "stick");
    // Language: wraps both ways, own first
    CHECK(r.model.ValueIndex(NativeRow::Language) == 0 && r.model.Step(NativeRow::Language, -1) == StepResult::Changed && r.model.Working().language == "German" && r.model.LanguageName(3) == "German");
    CHECK(r.model.Step(NativeRow::Language, +1) == StepResult::Changed && r.model.Working().language.empty());
    // Tilt Strength stops at the ends; reset goes to 5
    for (int i = 0; i < 4; ++i) CHECK(r.model.Step(NativeRow::TiltStrength, -1) == StepResult::Changed);
    CHECK(r.model.Working().tiltLevel == 1 && r.model.Step(NativeRow::TiltStrength, -1) == StepResult::AtEnd && r.model.Working().tiltLevel == 1);
    CHECK(r.model.ResetRow(NativeRow::TiltStrength) == StepResult::Changed && r.model.Working().tiltLevel == 5);
    for (int i = 0; i < 5; ++i) r.model.Step(NativeRow::TiltStrength, +1);
    CHECK(r.model.Working().tiltLevel == 10 && r.model.Step(NativeRow::TiltStrength, +1) == StepResult::AtEnd);
    // A saved language that the disc does not list reads as the game's own
    Rig u; Config c = DefaultConfig(controls::GetControlApi()); c.language = "Klingon"; Put(u, Text(c)); u.Open();
    CHECK(u.model.ValueIndex(NativeRow::Language) == 0);
    // No motion sensor: Tilt hidden and cannot change. No discs list: Language cannot change.
    Rig n(true, false); n.Open(); CHECK(!n.model.RowShown(NativeRow::Tilt) && n.model.Step(NativeRow::Tilt, 1) == StepResult::Unavailable);
    Rig e; e.Open(); NativeModel m; auto st = m.Open(e.host, {}); (void)st; CHECK(!m.RowShown(NativeRow::Language) && m.Step(NativeRow::Language, 1) == StepResult::Unavailable);
    Rig one; one.langs = {"japanese"}; one.Open(); CHECK(!one.model.RowShown(NativeRow::Language) && one.model.Step(NativeRow::Language, 1) == StepResult::Unavailable);   // one disc language: no row
    Rig two; two.langs = {"english", "french"}; two.Open(); CHECK(two.model.RowShown(NativeRow::Language) && two.model.ValueCount(NativeRow::Language) == 3);
}

static void PictureParity() {
    for (int fixed : {1, 0}) for (int start30 : {0, 1}) {
        Rig r(fixed != 0); Config c = DefaultConfig(controls::GetControlApi());
        if (start30) { c.fpsLimit = 30; c.performance60 = false; }
        Put(r, Text(c)); r.Open();
        SettingsSession session{}; session.mode = OpenMode::ColdStart; session.controls = &controls::GetControlApi();
        session.document.config = r.model.Loaded().config; session.active = session.document.config;
        session.controller = controls::NominalCaps(controls::ProfileId::Handheld); session.features = {fixed != 0, true};
        UiState s; Begin(s, session);
        uint64_t t = 0; auto in = [&](uint32_t key) { auto caps = session.controller; caps.connected = true; Step(s, {key, 0, caps, session.detectedProfile}, t += 40000); };
        in(0);   // arms; the page opens on RowRate (Picture mode)
        CHECK(s.row == RowRate);
        for (int press = 0; press < 3; ++press) for (int dir : {+1, -1}) {
            in(dir > 0 ? Right : Left); in(0);
            const auto res = r.model.Step(NativeRow::Picture, dir);
            CHECK(res == StepResult::Changed || res == StepResult::Unavailable);
            CHECK(s.working.fpsLimit == r.model.Working().fpsLimit && s.working.performance60 == r.model.Working().performance60);
            CHECK(Mode30(s.working) == (r.model.ValueIndex(NativeRow::Picture) == 1));
        }
    }
    // Cross on the old row does the same as Step
    Rig r; r.Open();
    CHECK(r.model.Step(NativeRow::Picture, 1) == StepResult::Changed && Mode30(r.model.Working()));
    CHECK(r.model.CanStep(NativeRow::Picture));
    // Without the fixed 30: Performance cannot move; a saved Quality goes back to Performance
    Rig nf(false); nf.Open(); CHECK(nf.model.Step(NativeRow::Picture, 1) == StepResult::Unavailable && !Mode30(nf.model.Working()));
    Config c30 = DefaultConfig(controls::GetControlApi()); c30.fpsLimit = 30; c30.performance60 = false; Rig nq(false); Put(nq, Text(c30)); nq.Open();
    CHECK(Mode30(nq.model.Working()) && nq.model.Step(NativeRow::Picture, 1) == StepResult::Changed && !Mode30(nq.model.Working()) && nq.model.Working().fpsLimit == 0 && nq.model.Working().performance60);
}

static void TechnicalKeysKept() {
    Rig r; const Config tech = Technical();
    const std::string base = "# my notes\nunknown_key=hello\n; another comment\n";
    const std::string text = Text(tech, base);
    Put(r, text); auto st = r.Open(); CHECK(st.status == StoreStatus::Ok && Equal(r.model.Working(), tech));
    CHECK(text.find("# my notes") == 0);
    for (NativeRow row : {NativeRow::Picture, NativeRow::Language, NativeRow::LessMotion, NativeRow::Tilt, NativeRow::TiltStrength, NativeRow::Pointer})
        CHECK(r.model.Step(row, +1) == StepResult::Changed);
    CHECK(r.model.Save().status == StoreStatus::Ok);
    const std::string now = r.files.inner.data[kPath];
    const auto a = Lines(text), b = Lines(now);
    CHECK(a.size() == b.size());   // no key added or lost
    std::vector<std::string> changed;
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) changed.push_back(a[i].substr(0, a[i].find('=')));
    const std::vector<std::string> want{"fps_limit", "language", "reduced_motion", "mouse_aim", "motion_level", "motion_source", "perf_mode"};
    for (const auto& k : changed) { bool ok = false; for (const auto& w : want) ok |= w == k; if (!ok) std::fprintf(stderr, "unexpected change: %s\n", k.c_str()); CHECK(ok); }
    CHECK(changed.size() == want.size());
    CHECK(b[0] == "# my notes" && b[1] == "unknown_key=hello" && b[2] == "; another comment");   // unknown keys and comments stay, in place
    Config back = Load(r.files); Config expect = tech;   // everything else equals the technical config
    back.fpsLimit = expect.fpsLimit = 0; back.performance60 = expect.performance60 = true; back.language = expect.language = "";
    back.reducedMotion = expect.reducedMotion = false; back.mouseAim = expect.mouseAim = false; back.tiltLevel = expect.tiltLevel = 5;
    back.tiltStick = expect.tiltStick = false; back.safeSaves = expect.safeSaves = false;
    CHECK(Equal(back, expect));
    // A save that changes nothing rewrites the same text
    Rig same; Put(same, text); same.Open(); CHECK(!same.model.Dirty() && same.model.Save().status == StoreStatus::Ok && same.files.inner.data[kPath] == text);
}

static void ReadOnly() {
    Rig r; Put(r, "schema=2\nfuture_key=1\n"); auto st = r.Open();
    CHECK(st.status == StoreStatus::UnsupportedVersion && !r.model.Editable());
    CHECK(r.model.Step(NativeRow::LessMotion, 1) == StepResult::ReadOnly && !r.model.CanStep(NativeRow::LessMotion) && !r.model.Dirty());
    CHECK(r.model.Save().status == StoreStatus::UnsupportedVersion);
    CHECK(r.model.Bind("jump", 0, "circle", true).kind == BindOutcome::Unavailable);
    CHECK(r.files.inner.data[kPath] == "schema=2\nfuture_key=1\n" && !r.files.inner.data.count(kBak) && !r.files.inner.data.count(kTmp));
}

static void WriteErrors() {
    Config old = DefaultConfig(controls::GetControlApi()); old.language = "French";
    Config nw = old; nw.reducedMotion = true;
    // First count the calls of a clean save, then fail each in turn.
    unsigned total = 0;
    { Rig r; Put(r, Text(old)); r.Open(); r.model.Step(NativeRow::LessMotion, 1); r.files.calls = 0; CHECK(r.model.Save().status == StoreStatus::Ok); total = r.files.calls; }
    CHECK(total >= 10);
    for (unsigned at = 1; at <= total; ++at) {
        Rig r; Put(r, Text(old)); r.Open(); r.model.Step(NativeRow::LessMotion, 1);
        r.files.calls = 0; r.files.failAt = at;
        const auto res = r.model.Save(); r.files.failAt = 0;
        StoreStatus status; const Config now = Load(r.files, &status);
        CHECK(status == StoreStatus::Ok || status == StoreStatus::Recovered);   // the file or its .bak is readable
        if (res.status != StoreStatus::Ok) { CHECK(Equal(now, old)); CHECK(r.model.Dirty()); CHECK(r.model.Loaded().config.reducedMotion == false); }
        else CHECK(Equal(now, nw) || Equal(now, old));
        // a retry after the fault works
        CHECK(r.model.Save().status == StoreStatus::Ok); CHECK(Equal(Load(r.files), nw));
    }
}

static void Restart() {
    Rig r; Put(r, Text(DefaultConfig(controls::GetControlApi()))); r.Open();
    CHECK(!r.model.RestartNeeded() && !r.model.WouldNeedRestart() && !r.model.NextStartOnly());
    // only Picture and Language count: a Less Motion change the host cannot apply live is "next start" but not a restart question
    r.model.Step(NativeRow::LessMotion, 1);
    r.live.failSet = true; r.model.Step(NativeRow::Pointer, 1);
    CHECK(r.model.Dirty() && !r.model.WouldNeedRestart() && r.model.NextStartOnly() && r.model.Save().status == StoreStatus::Ok && !r.model.RestartNeeded());
    r.live.failSet = false;
    r.model.Step(NativeRow::Picture, 1); CHECK(r.model.Dirty() && !r.model.RestartNeeded() && r.model.WouldNeedRestart());
    CHECK(r.model.Save().status == StoreStatus::Ok && r.model.RestartNeeded() && !r.model.Dirty());
    r.model.Step(NativeRow::Picture, 1); CHECK(r.model.Dirty() && r.model.RestartNeeded() && !r.model.WouldNeedRestart());
    CHECK(r.model.Save().status == StoreStatus::Ok && !r.model.RestartNeeded());
    // the retired fields and the pad defaults never ask for a restart: the boot Config differs from the file in them, and that is not looked at
    Rig q; Put(q, Text(Technical())); q.boot = DefaultConfig(controls::GetControlApi()); q.boot.profile = controls::ProfileId::PlayStation; q.Open();
    q.model.Step(NativeRow::Pointer, 1); CHECK(q.model.Save().status == StoreStatus::Ok && !q.model.RestartNeeded() && !q.model.WouldNeedRestart());
    // Revert
    r.model.Step(NativeRow::Pointer, 1); CHECK(r.model.Dirty()); r.model.Revert(); CHECK(!r.model.Dirty());
    // Relaunch goes to the host; a host without one does not
    static int launches = 0; r.host.relaunch = []() { ++launches; return false; }; CHECK(!r.model.Relaunch() && launches == 1);
    r.host.relaunch = nullptr; CHECK(!r.model.Relaunch());
}

static void Overridden() {
    Rig r; r.overrides = {{"GXV_TILT_GAIN", "3", "x"}, {"GXV_PICTURE_MODE", "30", "y"}, {"GXV_REMAP", "0", "z"}}; r.Open();
    CHECK(r.model.IsOverridden(NativeRow::TiltStrength) && r.model.IsOverridden(NativeRow::Picture) && !r.model.IsOverridden(NativeRow::Pointer));
    CHECK(r.model.Step(NativeRow::TiltStrength, 1) == StepResult::Overridden && r.model.Working().tiltLevel == 5 && !r.model.CanStep(NativeRow::TiltStrength));
    CHECK(r.model.Step(NativeRow::Picture, 1) == StepResult::Overridden && !Mode30(r.model.Working()));
    CHECK(r.model.ResetRow(NativeRow::TiltStrength) == StepResult::Overridden);
    CHECK(r.model.Step(NativeRow::Pointer, 1) == StepResult::Changed && r.model.BindingsOverridden());
    Rig n; n.Open(); CHECK(!n.model.BindingsOverridden());
}

static void Binding() {
    Rig r; r.Open(); auto& m = r.model; const auto& api = controls::GetControlApi();
    CHECK(m.Profile() == controls::ProfileId::Handheld && m.Moves().size() == api.actionCount);
    CHECK(m.Moves()[2].id == "spin" && m.Moves()[2].slots == 2 && m.Moves()[0].slots == 1);
    CHECK(m.ButtonOf("jump", 0) == "cross" && m.ButtonOf("star_bits", 0) == "circle" && m.ButtonOf("spin", 1) == "r1" && m.ButtonOf("jump", 1).empty());
    CHECK(!m.Buttons().empty());
    // Applied
    CHECK(m.Bind("crouch", 0, "triangle", false).kind == BindOutcome::Conflict);
    auto c = m.Bind("jump", 0, "circle", false); CHECK(c.kind == BindOutcome::Conflict && c.conflictMove == "star_bits" && m.ButtonOf("jump", 0) == "cross" && !m.Dirty());
    CHECK(m.Bind("jump", 0, "circle", true).kind == BindOutcome::Ok);
    CHECK(m.ButtonOf("jump", 0) == "circle" && m.ButtonOf("star_bits", 0) == "cross" && m.Dirty());
    // Required action lost, unavailable
    CHECK(m.Bind("jump", 0, "", false).kind == BindOutcome::RequiredActionLost && m.ButtonOf("jump", 0) == "circle");
    CHECK(m.Bind("jump", 1, "r1", false).kind == BindOutcome::Unavailable);   // a one-slot move has no second slot
    CHECK(m.Bind("nonsense", 0, "cross", false).kind == BindOutcome::Unavailable);
    std::string missing; for (size_t i = 0; i < api.buttonCount; ++i) { const std::string id = api.buttons[i].id; bool in = false; for (const auto& x : m.Buttons()) in |= x.id == id; if (!in) { missing = id; break; } }
    if (!missing.empty()) CHECK(m.Bind("crouch", 0, missing, false).kind == BindOutcome::Unavailable);
    // clear a second slot
    CHECK(m.Bind("spin", 1, "", false).kind == BindOutcome::Ok && m.ButtonOf("spin", 1).empty());
    // saves and reloads
    CHECK(m.Save().status == StoreStatus::Ok);
    Config back = Load(r.files); CHECK(Equal(back, m.Working()) && back.profiles[0].bindings[0].buttons[0] == "circle");
    // the other profile is untouched; switching marks the choice
    // defaults: the active profile only
    m.Defaults();
    CHECK(SameBindings(m.Working().profiles[0], api.defaults(controls::ProfileId::Handheld)) && m.ButtonOf("jump", 0) == "cross");
    // a connected pad restricts the buttons
    controls::ControllerCaps caps = controls::NominalCaps(controls::ProfileId::Handheld); caps.connected = true; caps.availableButtons &= ~uint64_t(controls::Select);
    m.SetController(caps, controls::ProfileId::Handheld);
    for (const auto& b : m.Buttons()) CHECK(b.id != "select");
    CHECK(m.Save().status == StoreStatus::Malformed);   // the default Minus is Select: the attached pad cannot press it (settings_ui's third check)
    CHECK(m.RemapAvailable() == controls::RemapEnabled());
}

static void DamagedMain() {
    // Damaged main file, good .bak: Open recovers from the .bak; a save then REMOVES the damaged main (it is not rotated into .bak),
    // writes the new file, and leaves the old .bak as it was.
    Config good = DefaultConfig(controls::GetControlApi()); good.language = "German";
    const std::string goodText = Text(good);
    Rig r; Put(r, "schema=1\nfps_limit=3"); r.files.inner.data[kBak] = goodText;
    auto st = r.Open(); CHECK(st.status == StoreStatus::Recovered && r.model.Working().language == "German" && r.model.Editable());
    r.model.Step(NativeRow::LessMotion, 1); CHECK(r.model.Save().status == StoreStatus::Ok);
    CHECK(r.files.inner.data[kBak] == goodText);                       // .bak is the older good file, unchanged
    CHECK(r.files.inner.data[kPath] != goodText && Load(r.files).reducedMotion && !r.files.inner.data.count(kTmp));
    // A second save then rotates the (now good) main into .bak as usual
    r.model.Step(NativeRow::Pointer, 1); CHECK(r.model.Save().status == StoreStatus::Ok);
    CHECK(r.files.inner.data[kBak] != goodText && Load(r.files).mouseAim);
    // Damaged main and no .bak: defaults, editable; the save replaces the bad file
    Rig d; Put(d, "garbage"); auto s2 = d.Open(); CHECK(s2.status == StoreStatus::Malformed && d.model.Editable());
    d.model.Step(NativeRow::Pointer, 1); CHECK(d.model.Save().status == StoreStatus::Ok && Load(d.files).mouseAim);
}

static void LiveRows() {
    for (NativeRow row : {NativeRow::CameraZoom, NativeRow::RightStick, NativeRow::RearTouch, NativeRow::TapToJump, NativeRow::StarBitReach}) {
        Rig r; r.Open(); auto& m = r.model;
        const int before = r.live.val[(int)row];
        CHECK(m.RowShown(row) && m.CanStep(row) && m.ValueIndex(row) == (unsigned)before && m.RowInfo(row).live && !m.RowInfo(row).restart);
        const unsigned sets = r.live.sets;
        CHECK(m.Step(row, +1) == StepResult::Changed);
        CHECK(r.live.val[(int)row] != before && r.live.sets == sets + 1 && r.live.lastRow == row);   // previewed at once, through the table
        CHECK(m.Dirty() && Equal(m.Working(), m.Loaded().config) && m.ValueIndex(row) == (unsigned)r.live.val[(int)row]);
        m.Revert(); CHECK(r.live.val[(int)row] == before && !m.Dirty() && m.ValueIndex(row) == (unsigned)before);   // Revert undoes it through the table
        m.Step(row, +1); CHECK(m.Save().status == StoreStatus::Ok && r.live.commits == 1 && !m.Dirty());
        const int saved = r.live.val[(int)row]; m.Revert(); CHECK(r.live.val[(int)row] == saved);   // saved: Revert keeps it
        // a refused set changes nothing
        r.live.failSet = true; CHECK(m.Step(row, +1) == StepResult::Unavailable && r.live.val[(int)row] == saved && m.ValueIndex(row) == (unsigned)saved);
        // no reader on this platform: the row is hidden
        Rig n; n.withLive = false; n.Open(); CHECK(!n.model.RowShown(row) && n.model.Step(row, 1) == StepResult::Unavailable);
    }
    Rig r; r.Open(); auto& m = r.model;
    // Camera Zoom: 1.0 .. 2.0 in tenths, stops at both ends
    CHECK(m.ValueCount(NativeRow::CameraZoom) == 11 && std::string(m.ValueKey(NativeRow::CameraZoom, 0)) == "1.0" && std::string(m.ValueKey(NativeRow::CameraZoom, 5)) == "1.5" && std::string(m.ValueKey(NativeRow::CameraZoom, 10)) == "2.0");
    for (int i = 0; i < 5; ++i) CHECK(m.Step(NativeRow::CameraZoom, -1) == StepResult::Changed);
    CHECK(m.ValueIndex(NativeRow::CameraZoom) == 0 && m.Step(NativeRow::CameraZoom, -1) == StepResult::AtEnd);
    for (int i = 0; i < 10; ++i) CHECK(m.Step(NativeRow::CameraZoom, +1) == StepResult::Changed);
    CHECK(m.Step(NativeRow::CameraZoom, +1) == StepResult::AtEnd && m.ResetRow(NativeRow::CameraZoom) == StepResult::Changed && m.ValueIndex(NativeRow::CameraZoom) == 5);
    // Star Bit Reach 1..5
    CHECK(m.ValueCount(NativeRow::StarBitReach) == 5 && std::string(m.ValueKey(NativeRow::StarBitReach, 4)) == "5");
    for (int i = 0; i < 4; ++i) m.Step(NativeRow::StarBitReach, +1);
    CHECK(m.Step(NativeRow::StarBitReach, +1) == StepResult::AtEnd);
    // Invert Camera is shown only while the right stick is the camera
    CHECK(m.RowShown(NativeRow::InvertCamera) && m.Step(NativeRow::InvertCamera, 1) == StepResult::Changed);
    CHECK(m.Step(NativeRow::RightStick, 1) == StepResult::Changed && std::string(m.ValueKey(NativeRow::RightStick, m.ValueIndex(NativeRow::RightStick))) == "pointer");
    r.live.val[(int)NativeRow::InvertCamera] = 0;
    CHECK(!m.RowShown(NativeRow::InvertCamera) && m.Step(NativeRow::InvertCamera, 1) == StepResult::Unavailable);
    // Tap to Jump standard is On
    Rig t; t.Open(); CHECK(t.model.ValueIndex(NativeRow::TapToJump) == 1 && std::string(t.model.ValueKey(NativeRow::TapToJump, 1)) == "on");
    // Config-backed live rows (Less Motion, Tilt, Tilt Strength, Pointer) are sent to the table, and need no restart when it applies them
    Rig c; c.Open(); auto& cm = c.model;
    CHECK(cm.Step(NativeRow::LessMotion, 1) == StepResult::Changed && c.live.val[(int)NativeRow::LessMotion] == 1 && cm.Working().reducedMotion);
    CHECK(!cm.NextStartOnly() && cm.Save().status == StoreStatus::Ok && !cm.RestartNeeded());
    cm.Step(NativeRow::Tilt, 1); cm.Step(NativeRow::TiltStrength, 1); cm.Step(NativeRow::Pointer, 1);
    cm.Revert(); CHECK(c.live.val[(int)NativeRow::Tilt] == 0 && c.live.val[(int)NativeRow::TiltStrength] == 4 && c.live.val[(int)NativeRow::Pointer] == 0 && c.live.val[(int)NativeRow::LessMotion] == 1 && !cm.Dirty());
    // ... and wait for the next start when the table refuses (or there is none)
    Rig f; f.Open(); f.live.failSet = true; f.model.Step(NativeRow::Pointer, 1);
    CHECK(f.model.NextStartOnly() && !f.model.WouldNeedRestart() && f.model.Save().status == StoreStatus::Ok && !f.model.RestartNeeded() && f.model.NextStartOnly());
    // Buttons: handed to the table on a save; a refusal leaves a restart pending
    Rig b; b.Open(); b.model.Bind("jump", 0, "circle", true); CHECK(b.model.BindingsChanged() && !b.model.NextStartOnly());
    CHECK(b.model.Save().status == StoreStatus::Ok && b.live.binds == 1 && !b.model.RestartNeeded() && !b.model.NextStartOnly());
    Rig bn; bn.live.bindsOk = false; bn.Open(); bn.model.Bind("jump", 0, "circle", true); CHECK(bn.model.Save().status == StoreStatus::Ok && bn.model.NextStartOnly() && !bn.model.RestartNeeded());
    Rig bz; bz.withLive = false; bz.Open(); bz.model.Bind("jump", 0, "circle", true); CHECK(bz.model.NextStartOnly() && bz.model.Save().status == StoreStatus::Ok && bz.model.NextStartOnly() && !bz.model.RestartNeeded());
}

static void WhereAndFlags() {
    CHECK(NativeModel::RowInfo(NativeRow::Picture).restart && !NativeModel::RowInfo(NativeRow::Picture).live && NativeModel::RowInfo(NativeRow::Language).restart);
    CHECK(NativeModel::RowInfo(NativeRow::CameraZoom).live && NativeModel::RowInfo(NativeRow::LessMotion).live && !NativeModel::RowInfo(NativeRow::LessMotion).restart);
    for (unsigned i = 0; i < kNativeRows; ++i) { auto in = NativeModel::RowInfo(static_cast<NativeRow>(i)); CHECK(in.shownEntry && in.shownPause && in.shownSession); CHECK(in.live != in.restart); }
    Rig r; r.Open(); auto& m = r.model;
    CHECK(m.Where() == NativeWhere::Entry && m.CanStep(NativeRow::Picture) && m.CanStep(NativeRow::StarBitReach) && m.BindingsEnabled() == controls::RemapEnabled());
    m.SetWhere(NativeWhere::Pause); CHECK(m.CanStep(NativeRow::Picture) && m.CanStep(NativeRow::Language) && m.BindingsEnabled() == controls::RemapEnabled());
    m.SetWhere(NativeWhere::Session, false);
    CHECK(m.Blocked(NativeRow::Picture) == RowBlock::Together && m.Step(NativeRow::Picture, 1) == StepResult::NotNow && !Mode30(m.Working()));
    CHECK(m.Blocked(NativeRow::Language) == RowBlock::Together && m.Step(NativeRow::Language, 1) == StepResult::NotNow && m.Working().language.empty());
    for (NativeRow row : {NativeRow::CameraZoom, NativeRow::RightStick, NativeRow::InvertCamera, NativeRow::RearTouch, NativeRow::TapToJump, NativeRow::Tilt, NativeRow::TiltStrength, NativeRow::Pointer, NativeRow::LessMotion, NativeRow::StarBitReach})
        CHECK(m.Blocked(row) == RowBlock::None);
    CHECK(!m.BindingsEnabled() && m.Bind("jump", 0, "circle", true).kind == BindOutcome::Unavailable && m.ButtonOf("jump", 0) == "cross");
    m.Defaults(); CHECK(!m.Dirty());
    m.SetWhere(NativeWhere::Session, true);   // a Star Race
    CHECK(m.Blocked(NativeRow::StarBitReach) == RowBlock::StarRace && m.Step(NativeRow::StarBitReach, 1) == StepResult::NotNow && r.live.val[(int)NativeRow::StarBitReach] == 0);
    CHECK(m.Blocked(NativeRow::CameraZoom) == RowBlock::None);
    // a read-only file beats everything
    Rig ro; Put(ro, "schema=2\n"); ro.Open(); CHECK(ro.model.Blocked(NativeRow::CameraZoom) == RowBlock::ReadOnly && ro.model.Step(NativeRow::CameraZoom, 1) == StepResult::ReadOnly);
}

static void Retire() {
    Config c = Technical(); c.safeSaves = false; c.allow50 = true; c.language = ""; c.fpsLimit = 30; c.performance60 = false; c.reducedMotion = true;
    Config d = c; NativeRetire(d, "French");
    CHECK(!d.fullSpeed && d.sharpness == Sharpness::Strong && !d.hdTextures && !d.depthOfField && !d.coreHelp && !d.gxmDirect && d.logicOverlap && !d.overlapMax && !d.lazyMaterials && d.farAnimation == FarAnimation::Aggressive && d.safeSaves && !d.allow50);   // the 2.3 tuning stays; Safe Saves forced on and retired Allow50 forced off
    CHECK(d.language.empty() && gFallback == "French");               // System Language: the Config is not changed; the disc-side fallback is registered
    CHECK(d.fpsLimit == 30 && !d.performance60 && d.reducedMotion);   // the rows that stay are not touched
    CHECK(d.autoQuality == c.autoQuality && d.aboveNative == c.aboveNative && d.antiAliasing == c.antiAliasing && d.effects == c.effects);   // (the hidden ones are already ignored)
    Config e = c; e.language = "german"; NativeRetire(e, "french"); CHECK(e.language == "german");   // a chosen language stays in the Config (the disc side lets it win over the fallback)
    gFallback.clear(); Config n = c; NativeRetire(n); CHECK(n.language.empty() && gFallback.empty());
    // env presence is recorded in NativeRetire, before ApplyStartup could set anything itself
    for (const char* k : {"PETARI_TOUCH_A", "PETARI_RSTICK", "PETARI_ZOOM_FORCE", "PETARI_STAR_BIT_REACH", "GXV_MOUSE_AIM", "GXV_REDUCED_MOTION"}) {
        setenv(k, "0", 1); Config t = c; NativeRetire(t); CHECK(NativeEnvWasPresent(k)); unsetenv(k); CHECK(NativeEnvWasPresent(k));   // (a snapshot)
        NativeRetire(t); CHECK(!NativeEnvWasPresent(k));
    }
    // The file keeps its lines: a native save of an unrelated change leaves every retired line (and comments, unknown keys) byte for byte
    Rig r; Config file = Technical(); file.safeSaves = false; const std::string text = Text(file, "# keep me\nweird_key=1\n"); Put(r, text); r.Open();
    Config run = r.model.Working(); NativeRetire(run, "english"); CHECK(!run.fullSpeed && run.sharpness == Sharpness::Strong && run.safeSaves && gFallback == "english");   // the RUN keeps the file's values (Safe Saves on) ...
    CHECK(r.model.Working().sharpness == Sharpness::Strong && !r.model.Working().safeSaves && !r.model.Working().hdTextures);   // ... the model's file Config is not
    r.model.Step(NativeRow::LessMotion, 1); r.model.Step(NativeRow::LessMotion, 1); CHECK(!r.model.Dirty() && r.model.Save().status == StoreStatus::Ok && r.files.inner.data[kPath] == text);
    r.model.Step(NativeRow::Pointer, 1); CHECK(r.model.Save().status == StoreStatus::Ok);
    const auto a = Lines(text), b = Lines(r.files.inner.data[kPath]); CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) CHECK(a[i].compare(0, 9, "mouse_aim") == 0);
}

// The owner's rule (15 Nov): a settings file written by c181 (2.3), with the owner's values, shows exactly those values; opening and closing changes no byte;
// one change changes only that key. c181 knows no atomic_saves key (Safe Saves is 3.0's, always on), so a c181 file does not have it.
static std::string C181File(const Config& c) {
    std::string out; auto r = SerializeDocument(Document{DefaultConfig(controls::GetControlApi()), "", true}, c, controls::GetControlApi(), out); CHECK(r.status == StoreStatus::Ok);
    std::string keep; for (const auto& l : Lines(out)) if (l.compare(0, 13, "atomic_saves=") != 0 && l.compare(0, 9, "assist60=") != 0 && l.compare(0, 8, "allow50=") != 0) keep += l + "\n";   // (setnat31: nor assist60)
    return keep;
}
static void C181File_() {
    Config own = Technical(); own.safeSaves = false; own.fpsLimit = 0; own.performance60 = true; own.reducedMotion = true; own.tiltLevel = 7; own.tiltStick = true; own.language = "French";
    const std::string text = C181File(own);
    CHECK(text.find("atomic_saves") == std::string::npos && text.find("perf_mode=1") != std::string::npos);
    Rig r; Put(r, text); auto st = r.Open(); CHECK(st.status == StoreStatus::Ok && !r.model.Dirty());
    CHECK(Equal(r.model.Working(), own));   // every key the file has, as the owner set it
    CHECK(r.model.ValueIndex(NativeRow::Picture) == 0 && r.model.ValueIndex(NativeRow::LessMotion) == 1 && r.model.ValueIndex(NativeRow::TiltStrength) == 6 && r.model.ValueIndex(NativeRow::Tilt) == 1);
    CHECK(r.model.Working().language == "French");
    // opened and closed with no change: not a byte
    CHECK(r.model.Save().status == StoreStatus::Ok && r.files.inner.data[kPath] == text);
    // one change: only that key
    CHECK(r.model.Step(NativeRow::LessMotion, 1) == StepResult::Changed && r.model.Save().status == StoreStatus::Ok);
    const auto a = Lines(text), b = Lines(r.files.inner.data[kPath]); CHECK(a.size() == b.size());
    unsigned diff = 0; for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) { ++diff; CHECK(a[i].compare(0, 15, "reduced_motion=") == 0); }
    CHECK(diff == 1);
    // Quality (30) the same
    Config q = own; q.fpsLimit = 30; q.performance60 = false; Rig rq; Put(rq, C181File(q)); rq.Open(); CHECK(rq.model.ValueIndex(NativeRow::Picture) == 1 && rq.model.Save().status == StoreStatus::Ok && rq.files.inner.data[kPath] == C181File(q));
    // a c181 file with keys missing (an older 2.3 file): a save adds none of them at their default
    std::string sparse; for (const auto& l : Lines(text)) if (l.compare(0, 12, "motion_level") != 0 && l.compare(0, 11, "mouse_aim=") != 0 && l.compare(0, 14, "profile_chosen") != 0) sparse += l + "\n";
    Rig s2; Put(s2, sparse); CHECK(s2.Open().status == StoreStatus::Ok); CHECK(s2.model.Step(NativeRow::LessMotion, 1) == StepResult::Changed && s2.model.Save().status == StoreStatus::Ok);
    const auto c0 = Lines(sparse), c1 = Lines(s2.files.inner.data[kPath]); CHECK(c0.size() == c1.size());
    for (size_t i = 0; i < c0.size(); ++i) if (c0[i] != c1[i]) CHECK(c0[i].compare(0, 15, "reduced_motion=") == 0);
}

static std::string Marker(Rig& r) { auto it = r.files.inner.data.find(NativeMarkerPath(kPath)); return it == r.files.inner.data.end() ? std::string("(none)") : it->second; }
static NativeBootResult Start(Rig& r, Config& run) { run = Load(r.files); return NativeBootCheck(r.host, run); }
static void BootCheck() {
    CHECK(NativeMarkerPath("mem/player-settings.cfg") == "mem/player-settings.untested");
    // no marker: nothing happens
    { Rig r; Config run = DefaultConfig(controls::GetControlApi()); run.fpsLimit = 30; run.performance60 = false; auto res = NativeBootCheck(r.host, run); CHECK(!res.standard && !res.toast && Mode30(run) && Marker(r) == "(none)"); }
    // a clean start never fires, over many cycles
    { Rig r; Put(r, Text(DefaultConfig(controls::GetControlApi()))); r.Open();
      for (int i = 0; i < 6; ++i) {
          r.model.Step(NativeRow::Picture, 1); CHECK(r.model.Save().status == StoreStatus::Ok);
          const bool standardPair = !Mode30(r.model.Loaded().config);
          CHECK(Marker(r) == (standardPair ? "(none)" : "try 0\n"));
          Config run; auto res = Start(r, run); CHECK(!res.standard && !res.toast && Mode30(run) == !standardPair);
          if (!standardPair) CHECK(Marker(r) == "try 1\n");
          NativeEntryReached(r.host); CHECK(Marker(r) == "(none)");
      } }
    // two starts that never reach the entry screen: the third is a hold (standard Picture, the disc's first language, a toast); it cannot loop
    { Rig r; Put(r, Text(DefaultConfig(controls::GetControlApi()))); r.Open(); r.model.Step(NativeRow::Picture, 1); r.model.Step(NativeRow::Language, 1);
      CHECK(r.model.Save().status == StoreStatus::Ok && Marker(r) == "try 0\n");
      const std::string file = r.files.inner.data[kPath];
      Config run; auto s1 = Start(r, run); CHECK(!s1.standard && !s1.toast && Mode30(run) && Marker(r) == "try 1\n");
      auto s2 = Start(r, run); CHECK(!s2.standard && !s2.toast && Mode30(run) && run.language == "English" && Marker(r) == "try 2\n");
      auto s3 = Start(r, run); CHECK(s3.standard && s3.toast && !Mode30(run) && run.language.empty() && Marker(r) == "hold\n");
      for (int i = 0; i < 4; ++i) { auto s = Start(r, run); CHECK(s.standard && s.toast && !Mode30(run) && run.language.empty() && Marker(r) == "hold\n"); if (i == 1) NativeEntryReached(r.host); CHECK(Marker(r) == "hold\n"); }   // a toast at every hold start; the entry screen does not end a hold
      CHECK(r.files.inner.data[kPath] == file);   // the settings file is never touched
      // picking Full Detail (or a language) again, after a save, gets a fresh try
      NativeModel m; m.Open(r.host, kLangs); m.Step(NativeRow::Language, 1); CHECK(m.Save().status == StoreStatus::Ok && Marker(r) == "try 0\n");
      auto s4 = Start(r, run); CHECK(!s4.standard && !s4.toast && Marker(r) == "try 1\n");
      NativeEntryReached(r.host); CHECK(Marker(r) == "(none)"); }
    // an unreadable marker counts as one attempt
    { Rig r; r.files.inner.data[NativeMarkerPath(kPath)] = "%%%"; Config run = DefaultConfig(controls::GetControlApi()); run.fpsLimit = 30; run.performance60 = false;
      auto res = NativeBootCheck(r.host, run); CHECK(!res.standard && Marker(r) == "try 2\n"); }
    // the saved pair decides the marker: Less Motion writes none; a save of the standard pair removes it
    { Rig q; q.Open(); q.model.Step(NativeRow::Pointer, 1); q.model.Step(NativeRow::CameraZoom, 1); CHECK(q.model.Save().status == StoreStatus::Ok && Marker(q) == "(none)"); }
    { Rig r; Config c = DefaultConfig(controls::GetControlApi()); c.fpsLimit = 30; c.performance60 = false; Put(r, Text(c)); r.files.inner.data[NativeMarkerPath(kPath)] = "try 1\n"; r.Open();
      r.model.Step(NativeRow::LessMotion, 1); CHECK(r.model.Save().status == StoreStatus::Ok && Marker(r) == "try 1\n");
      r.model.Step(NativeRow::Picture, 1); CHECK(r.model.Save().status == StoreStatus::Ok && Marker(r) == "(none)"); }
    // NativeStartup: the check, the retirement (the system language goes to the disc side as a fallback) and nothing when the host is not available
    { Rig r; Put(r, Text(Technical())); r.host.systemLanguage = []() -> const char* { return "German"; };
      NativeHostSetForTest(&r.host); setenv("GXV_MOUSE_AIM", "0", 1); setenv("GXV_AUTOQ_TARGET", "30", 1);
      Config run = Load(r.files); NativeStartup(run);
      CHECK(run.language.empty() && gFallback == "German" && !run.fullSpeed && run.safeSaves && !run.depthOfField && !NativeStandardRun() && !NativeTakeBootToast());
      CHECK(NativeEnvWasPresent("GXV_MOUSE_AIM") && !NativeEnvWasPresent("GXV_AUTOQ_TARGET") && !NativeEnvWasPresent("GXV_TILT_GAIN"));
      unsetenv("GXV_MOUSE_AIM"); unsetenv("GXV_AUTOQ_TARGET");
      r.files.inner.data[NativeMarkerPath(kPath)] = "try 2\n"; gFallback.clear(); run = Load(r.files); NativeStartup(run);
      CHECK(NativeStandardRun() && NativeTakeBootToast() && !NativeTakeBootToast() && !Mode30(run) && gFallback.empty());   // a hold: no system language, the disc's first
      NativeStartup(run); CHECK(NativeStandardRun() && NativeTakeBootToast());                                                // ... and a toast at every hold start
      r.host.available = false; Config off = Technical(); NativeStartup(off); CHECK(Equal(off, Technical()));
      NativeHostSetForTest(nullptr); }
    // the always-run hook: once per process, on the platform host
    { Rig r; r.files.inner.data[NativeMarkerPath(kPath)] = "try 1\n"; NativeHostSetForTest(&r.host); NativeEntryReached(); CHECK(Marker(r) == "(none)");
      r.files.inner.data[NativeMarkerPath(kPath)] = "try 1\n"; NativeEntryReached(); CHECK(Marker(r) == "try 1\n");   // (a second call does nothing)
      NativeHostSetForTest(nullptr); }
}

static void HoldMenu() {
    // a hold start: the menu shows what is running; nothing is asked on Back; the next save ends the hold
    Config c = DefaultConfig(controls::GetControlApi()); c.fpsLimit = 30; c.performance60 = false; c.language = "french";
    Rig r; Put(r, Text(c)); r.files.inner.data[NativeMarkerPath(kPath)] = "try 2\n"; NativeHostSetForTest(&r.host);
    Config run = Load(r.files); NativeStartup(run); CHECK(NativeStandardRun()); r.boot = run;
    r.Open(); auto& m = r.model;
    CHECK(m.InHold() && !m.Dirty() && !Mode30(m.Working()) && m.Working().language == "English" && m.ValueIndex(NativeRow::Language) == 1 && m.ValueIndex(NativeRow::Picture) == 0);
    CHECK(Mode30(m.Loaded().config) && m.Loaded().config.language == "french");   // the file is as it was
    CHECK(!m.WouldNeedRestart());                                                  // what is shown is what runs
    m.Step(NativeRow::Pointer, 1); CHECK(m.Dirty() && m.Save().status == StoreStatus::Ok);
    CHECK(Marker(r) == "(none)" && !m.InHold() && !Mode30(Load(r.files)) && !m.RestartNeeded());   // the standard pair was saved: the hold is over
    // picking Full Detail again writes a fresh try
    Rig q; Put(q, Text(c)); q.files.inner.data[NativeMarkerPath(kPath)] = "hold\n"; NativeHostSetForTest(&q.host); run = Load(q.files); NativeStartup(run); q.boot = run; q.Open();
    q.model.Step(NativeRow::Picture, 1); CHECK(Mode30(q.model.Working()) && q.model.Save().status == StoreStatus::Ok && Marker(q) == "try 0\n" && q.model.RestartNeeded());
    NativeHostSetForTest(nullptr); NativeBootStateClearForTest();
}

static void QueuedSave() {
    Rig r; r.Open(); auto& m = r.model;
    m.Step(NativeRow::Pointer, 1); gAsyncOn = true;
    CHECK(m.Save().status == StoreStatus::Ok && !m.SaveQueued() && !gAsyncData.count(kPath) && Load(r.files).mouseAim);   // outside a session: the synchronous, verified save
    m.SetWhere(NativeWhere::Session);
    m.Step(NativeRow::LessMotion, 1);
    CHECK(m.Save().status == StoreStatus::Ok && m.SaveQueued() && gAsyncData.count(kPath) && !m.Dirty() && m.Loaded().config.reducedMotion);
    CHECK(gAsyncData[kPath].find("reduced_motion=1") != std::string::npos && gAsyncData[kPath] == m.Loaded().source);
    CHECK(!Load(r.files).reducedMotion);   // (nothing was written on this thread: the queue's writer does it)
    gAsyncOn = false; m.Step(NativeRow::LessMotion, 1);   // the queue refuses or is absent: the synchronous path is the fallback
    CHECK(m.Save().status == StoreStatus::Ok && !m.SaveQueued() && !Load(r.files).reducedMotion && Load(r.files).mouseAim);
    // a read-only file hands nothing over
    Rig ro; Put(ro, "schema=2\n"); ro.Open(); ro.model.SetWhere(NativeWhere::Session); gAsyncOn = true; gAsyncData.clear(); CHECK(ro.model.Save().status == StoreStatus::UnsupportedVersion && gAsyncData.empty());
    gAsyncOn = false;
}

static void LanguageRow() {
    Rig r; r.Open(); auto& m = r.model;
    CHECK(m.SystemLanguageName() == "English" || m.SystemLanguageName() == "english");   // (the disc's spelling)
    gSysLang = "german"; CHECK(m.SystemLanguageName() == "German");
    gSysLang = "japanese"; CHECK(m.SystemLanguageName().empty());   // the disc lacks it: its first language
    gSysLang = "english";
    // the file says System Language (""), the run was started with the system's language: no restart for an unrelated change
    r.boot.language = "english"; Rig s; s.boot.language = "english"; s.Open();
    s.model.Step(NativeRow::Pointer, 1); CHECK(s.model.Save().status == StoreStatus::Ok && !s.model.RestartNeeded());
    s.model.Step(NativeRow::Language, 1); CHECK(s.model.ValueIndex(NativeRow::Language) == 1 && s.model.Working().language == "English" && s.model.PictureOrLanguageChanged());
    CHECK(!s.model.WouldNeedRestart());                        // the explicit choice equals what is running
    s.model.Step(NativeRow::Language, 1); CHECK(s.model.WouldNeedRestart() && s.model.Save().status == StoreStatus::Ok && s.model.RestartNeeded());
    s.model.Step(NativeRow::Language, -1); s.model.Step(NativeRow::Language, -1); CHECK(s.model.ValueIndex(NativeRow::Language) == 0);   // back to System Language
    CHECK(s.model.Save().status == StoreStatus::Ok && !s.model.RestartNeeded());
    // off-by-one: n disc languages give n + 1 values, the last index is the last language, and stepping from the last wraps to System Language
    CHECK(s.model.ValueCount(NativeRow::Language) == kLangs.size() + 1 && s.model.LanguageName(kLangs.size()) == kLangs.back() && s.model.LanguageName(kLangs.size() + 1).empty() && s.model.LanguageName(0).empty());
    s.model.Step(NativeRow::Language, -1); CHECK(s.model.ValueIndex(NativeRow::Language) == kLangs.size() && s.model.Working().language == kLangs.back());
    s.model.Step(NativeRow::Language, +1); CHECK(s.model.ValueIndex(NativeRow::Language) == 0 && s.model.Working().language.empty());
}

static void PsTv() {
    Rig r; r.host.externalPad = true; Put(r, Text(DefaultConfig(controls::GetControlApi()))); r.Open(); auto& m = r.model;
    CHECK(!m.Dirty() && !m.RowShown(NativeRow::RearTouch) && m.Step(NativeRow::RearTouch, 1) == StepResult::Unavailable);
    Rig h; h.Open(); CHECK(h.model.Profile() == controls::ProfileId::Handheld && h.model.RowShown(NativeRow::RearTouch));
    if (controls::PadDefaultsEnabled()) {
        CHECK(m.Profile() == controls::ProfileId::PlayStation && m.ButtonOf("star_bits", 0) == "r2" && m.Working().profile == controls::ProfileId::Handheld);
        const Config before = m.Working();
        CHECK(m.Bind("star_bits", 0, "r1", true).kind == BindOutcome::Ok && m.ButtonOf("star_bits", 0) == "r1");
        CHECK(SameBindings(m.Working().profiles[0], before.profiles[0]) && !SameBindings(m.Working().profiles[1], before.profiles[1]));   // the Handheld list is untouched
        CHECK(m.Save().status == StoreStatus::Ok);
        Config t = Load(r.files); CHECK(ApplyPadDefaults(t, true) && t.profile == controls::ProfileId::PlayStation);   // the next start still picks the pad's profile
        m.Defaults(); CHECK(m.ButtonOf("star_bits", 0) == "r2");
    } else CHECK(m.Profile() == controls::ProfileId::Handheld);
}

static void EnvPresence() {
    Rig r; r.Open(); auto& m = r.model;
    for (auto row : {NativeRow::Picture, NativeRow::Language, NativeRow::LessMotion, NativeRow::Tilt, NativeRow::TiltStrength, NativeRow::Pointer}) CHECK(!m.IsOverridden(row));
    gEnv = {"GXV_MOUSE_AIM", "GXV_FPS_LIMIT", "PETARI_LANGUAGE"};   // by the key being PRESENT: the value is not looked at (it may agree with the file)
    CHECK(m.IsOverridden(NativeRow::Pointer) && m.IsOverridden(NativeRow::Picture) && m.IsOverridden(NativeRow::Language) && !m.IsOverridden(NativeRow::Tilt));
    CHECK(m.Step(NativeRow::Pointer, 1) == StepResult::Overridden && !m.Working().mouseAim && m.Step(NativeRow::Language, 1) == StepResult::Overridden && m.Blocked(NativeRow::Picture) == RowBlock::Overridden);
    CHECK(m.Step(NativeRow::Tilt, 1) == StepResult::Changed);
    gEnv.clear();
    r.overrides = {{"GXV_REDUCED_MOTION", "1", "x"}, {"GXV_TILT_STICK", "0", "y"}}; CHECK(m.IsOverridden(NativeRow::LessMotion) && m.IsOverridden(NativeRow::Tilt) && !m.IsOverridden(NativeRow::Pointer));
    // the live rows' own env keys (Camera Zoom, Right Stick, Tap to Jump, Star Bit Reach)
    gEnv = {"PETARI_ZOOM_FORCE", "PETARI_RSTICK", "PETARI_TOUCH_A", "PETARI_STAR_BIT_REACH"};
    for (auto row : {NativeRow::CameraZoom, NativeRow::RightStick, NativeRow::TapToJump, NativeRow::StarBitReach}) CHECK(m.IsOverridden(row) && m.Step(row, 1) == StepResult::Overridden);
    CHECK(!m.IsOverridden(NativeRow::RearTouch) && !m.IsOverridden(NativeRow::InvertCamera));
    // an override on one row leaves the camera rows alone
    gEnv = {"GXV_TILT_GAIN"}; CHECK(m.IsOverridden(NativeRow::TiltStrength) && !m.IsOverridden(NativeRow::CameraZoom) && m.CanStep(NativeRow::CameraZoom));
}

static void DamagedReport() {
    { Rig r; auto st = r.Open(); CHECK(st.status == StoreStatus::Missing && !r.model.LoadDamaged() && r.model.LoadStatus().status == StoreStatus::Missing); }
    { Rig r; Put(r, Text(DefaultConfig(controls::GetControlApi()))); r.Open(); CHECK(!r.model.LoadDamaged()); }
    { Rig r; Put(r, "garbage"); auto st = r.Open(); CHECK(st.status == StoreStatus::Malformed && r.model.LoadDamaged()); }
    { Rig r; Put(r, "garbage"); r.files.inner.data[kBak] = Text(DefaultConfig(controls::GetControlApi())); auto st = r.Open(); CHECK(st.status == StoreStatus::Recovered && r.model.LoadDamaged()); }
    { Rig r; Put(r, Text(DefaultConfig(controls::GetControlApi()))); r.files.failAt = 1; auto st = r.Open(); CHECK(st.status == StoreStatus::IoError && r.model.LoadDamaged() && r.model.Editable()); }
    { Rig r; Put(r, "schema=2\n"); r.Open(); CHECK(!r.model.LoadDamaged()); }   // a newer file is read-only, not damaged
}

static void PutBack() {
    Config c = Technical(); c.fpsLimit = 30; c.performance60 = false; c.language = "french"; c.reducedMotion = true; c.tiltStick = true; c.tiltLevel = 8; c.mouseAim = true; c.safeSaves = false;
    const std::string text = Text(c, "# mine\nodd_key=3\n");
    Rig r; Put(r, text); gEnv = {"GXV_TILT_STICK"}; r.Open(); auto& m = r.model;
    r.live.val[(int)NativeRow::CameraZoom] = 9; r.live.val[(int)NativeRow::RightStick] = 1; r.live.val[(int)NativeRow::RearTouch] = 1; r.live.val[(int)NativeRow::TapToJump] = 0; r.live.val[(int)NativeRow::StarBitReach] = 3;
    r.Open();
    m.Bind("jump", 0, "circle", true); m.Bind("spin", 1, "", false);
    m.PutEverythingBack();
    CHECK(m.ValueIndex(NativeRow::Picture) == 0 && m.ValueIndex(NativeRow::Language) == 0 && m.ValueIndex(NativeRow::LessMotion) == 0 && m.ValueIndex(NativeRow::Pointer) == 0 && m.ValueIndex(NativeRow::TiltStrength) == 4);
    CHECK(m.ValueIndex(NativeRow::Tilt) == 1);   // an env.txt key holds Tilt where it is
    CHECK(m.ValueIndex(NativeRow::CameraZoom) == 5 && m.ValueIndex(NativeRow::RightStick) == 0 && m.ValueIndex(NativeRow::RearTouch) == 0 && m.ValueIndex(NativeRow::TapToJump) == 1 && m.ValueIndex(NativeRow::StarBitReach) == 0);
    CHECK(SameBindings(m.Working().profiles[0], controls::GetControlApi().defaults(controls::ProfileId::Handheld)));
    CHECK(m.Save().status == StoreStatus::Ok);
    const auto a = Lines(text), b = Lines(r.files.inner.data[kPath]); CHECK(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i) if (a[i] != b[i]) {
        const std::string k = a[i].substr(0, a[i].find('='));
        const bool ok = k == "fps_limit" || k == "perf_mode" || k == "language" || k == "reduced_motion" || k == "motion_level" || k == "mouse_aim" || k.compare(0, 5, "bind.") == 0;
        if (!ok) std::fprintf(stderr, "unexpected change: %s -> %s\n", a[i].c_str(), b[i].c_str());
        CHECK(ok);   // every retired key, unknown key and comment is as it was
    }
    CHECK(b[0] == "# mine" && b[1] == "odd_key=3");
    const Config back = Load(r.files);
    CHECK(back.sharpness == Sharpness::Strong && !back.hdTextures && !back.depthOfField && back.farAnimation == FarAnimation::Aggressive && back.tiltStick);
    // read-only and session: nothing moves
    Rig ro; Put(ro, "schema=2\n"); ro.Open(); ro.model.PutEverythingBack(); CHECK(!ro.model.Dirty());
}

static void RelaunchWait() {
    static int launches = 0;
    Rig r; r.Open(); r.host.relaunch = []() { ++launches; return false; };
    gIdle = false; CHECK(!r.model.RelaunchReady() && launches == 0);   // the menu asks every frame; nothing here waits
    gIdle = true; CHECK(r.model.RelaunchReady() && !r.model.Relaunch() && launches == 1);
    r.host.relaunchReady = nullptr; CHECK(r.model.RelaunchReady());
    const auto* port = NativeLiveOpsPort(); CHECK(port && port->get(nullptr, NativeRow::CameraZoom) == -1 && !port->set(nullptr, NativeRow::CameraZoom, 5));   // no perf.cpp in this test: nothing to forward to
    CHECK(port->get(nullptr, NativeRow::TapToJump) == -1 && port->get(nullptr, NativeRow::InvertCamera) == -1);
}

static void HostAndFlag() {
    CHECK(NativeHostGet() == nullptr);   // no platform host is linked into this test
    Rig r; NativeHostSetForTest(&r.host); CHECK(NativeHostGet() == &r.host); NativeHostSetForTest(nullptr); CHECK(NativeHostGet() == nullptr);
    CHECK(!NativeMenuOpen() && !port_settings_native_menu_open() && &port_settings_native_request == nullptr);
    NativeMenuOpenSet(true); CHECK(NativeMenuOpen() && port_settings_native_menu_open()); NativeMenuOpenSet(false); CHECK(!NativeMenuOpen());
}

// setnat31: the 60 Assist row: Config::assist60 (key assist60, absent = on = c181), a restart row shown only in Smooth Motion
static void Assist60() {
    {
        Rig r; r.Open(); auto& m = r.model;
        CHECK(m.RowShown(NativeRow::Assist60) && m.ValueCount(NativeRow::Assist60) == 2 && m.ValueIndex(NativeRow::Assist60) == 1);
        CHECK(std::string(m.ValueKey(NativeRow::Assist60, 1)) == "on" && std::string(m.ValueKey(NativeRow::Assist60, 0)) == "off" && std::string(NativeModel::RowKey(NativeRow::Assist60)) == "assist60");
        CHECK(m.RowInfo(NativeRow::Assist60).restart && !m.RowInfo(NativeRow::Assist60).live && m.CanStep(NativeRow::Assist60));
        CHECK(!m.WouldNeedRestart() && !m.PictureOrLanguageChanged());
        CHECK(m.Step(NativeRow::Assist60, 1) == StepResult::Changed && !m.Working().assist60 && m.Dirty() && m.WouldNeedRestart() && m.PictureOrLanguageChanged() && !m.NextStartOnly());
        CHECK(m.Save().status == StoreStatus::Ok && m.RestartNeeded() && !m.Dirty() && Marker(r) == "try 0\n");
        CHECK(r.files.inner.data[kPath].find("assist60=0\n") != std::string::npos && !Load(r.files).assist60);
        // Full Detail: the row is hidden and cannot change; a 60 Assist difference alone is then no restart
        CHECK(m.Step(NativeRow::Picture, 1) == StepResult::Changed && !m.RowShown(NativeRow::Assist60) && m.Step(NativeRow::Assist60, 1) == StepResult::Unavailable);
        CHECK(m.Step(NativeRow::Picture, 1) == StepResult::Changed && m.RowShown(NativeRow::Assist60));
        // back on, saved: the standard trio again, so the marker goes
        CHECK(m.ResetRow(NativeRow::Assist60) == StepResult::Changed && m.Working().assist60 && m.Save().status == StoreStatus::Ok && Marker(r) == "(none)" && !m.RestartNeeded());
    }
    {   // Full Detail at boot and in the file: assist60 differing does not ask for a restart
        Rig r; Config c = DefaultConfig(controls::GetControlApi()); c.fpsLimit = 30; c.performance60 = false; c.assist60 = false; Put(r, Text(c)); r.boot = c; r.boot.assist60 = true; r.Open();
        CHECK(!r.model.RestartNeeded() && !r.model.RowShown(NativeRow::Assist60));
    }
    {   // Put Everything Back: on again
        Rig r; Config c = DefaultConfig(controls::GetControlApi()); c.assist60 = false; Put(r, Text(c)); r.boot = c; r.Open();
        CHECK(r.model.ValueIndex(NativeRow::Assist60) == 0 && !r.model.RestartNeeded());
        r.model.PutEverythingBack(); CHECK(r.model.Working().assist60 && r.model.WouldNeedRestart());
    }
    {   // a session: grey, as Picture
        Rig r; r.Open(); r.model.SetWhere(NativeWhere::Session);
        CHECK(r.model.Blocked(NativeRow::Assist60) == RowBlock::Together && r.model.Step(NativeRow::Assist60, 1) == StepResult::NotNow);
    }
    {   // env.txt's GXV_HRW beats the row (by the key being present)
        Rig r; r.Open(); gEnv = {"GXV_HRW"};
        CHECK(r.model.IsOverridden(NativeRow::Assist60) && r.model.Step(NativeRow::Assist60, 1) == StepResult::Overridden && r.model.Working().assist60);
        gEnv.clear();
    }
    {   // NativeRetire: GXV_HRW from the mode-key snapshot (entry.cpp's own default "0" is set before it runs); without a snapshot the environment counts
        Config t = DefaultConfig(controls::GetControlApi());
        setenv("GXV_HRW", "0", 1);
        NativeRetire(t); CHECK(NativeEnvWasPresent("GXV_HRW"));
        port::modekeys::SnapshotUser([](const char*) -> const char* { return nullptr; });   // env.txt had none: the "0" is entry.cpp's
        NativeRetire(t); CHECK(!NativeEnvWasPresent("GXV_HRW"));
        port::modekeys::SnapshotUser([](const char* k) -> const char* { return std::strcmp(k, "GXV_HRW") == 0 ? "5" : nullptr; });
        NativeRetire(t); CHECK(NativeEnvWasPresent("GXV_HRW"));
        port::modekeys::Snapshotted() = false; port::modekeys::UserMask() = 0; unsetenv("GXV_HRW");
        NativeRetire(t); CHECK(!NativeEnvWasPresent("GXV_HRW"));
    }
    {   // a hold start runs the standard trio: 60 Assist on
        Rig r; Config c = DefaultConfig(controls::GetControlApi()); c.assist60 = false; Put(r, Text(c)); r.files.inner.data[NativeMarkerPath(kPath)] = "hold\n";
        Config run; const auto res = Start(r, run); CHECK(res.standard && run.assist60 && !Load(r.files).assist60);   // (the file is untouched)
    }
    {   // the file: absent = on; an unknown value reads on and is never an error; a full write has the key
        Rig r; Config c = DefaultConfig(controls::GetControlApi()); std::string t = Text(c);
        CHECK(t.find("assist60=1\n") != std::string::npos);
        const size_t at = t.find("assist60=1\n"); t.erase(at, 11); Put(r, t);
        StoreStatus st; CHECK(Load(r.files, &st).assist60 && st == StoreStatus::Ok);
        Put(r, t + "assist60=maybe\n"); CHECK(Load(r.files, &st).assist60 && st == StoreStatus::Ok);
        Put(r, t + "assist60=0\n"); CHECK(!Load(r.files, &st).assist60 && st == StoreStatus::Ok);
    }
}

// Wave 4: Allow50 is retired from the UI (hidden in all modes, migrated off by NativeRetire), while legacy allow50=1 files still parse cleanly and never cause spurious Dirty()/RestartNeeded() states
static void Allow50() {
    {
        Rig r; r.Open(); auto& m = r.model;
        CHECK(!m.RowShown(NativeRow::Allow50) && !m.CanStep(NativeRow::Allow50));
        CHECK(m.Blocked(NativeRow::Allow50) == RowBlock::Unavailable && m.Step(NativeRow::Allow50, 1) == StepResult::Unavailable);
        CHECK(std::string(NativeModel::RowKey(NativeRow::Allow50)) == "allow50" && m.ValueCount(NativeRow::Allow50) == 2 && !m.Working().allow50);
        // Full Detail: still hidden and cannot change
        CHECK(m.Step(NativeRow::Picture, 1) == StepResult::Changed && !m.RowShown(NativeRow::Allow50) && m.Step(NativeRow::Allow50, 1) == StepResult::Unavailable);
        CHECK(m.Step(NativeRow::Picture, 1) == StepResult::Changed && !m.RowShown(NativeRow::Allow50));
    }
    {   // backward-compatible settings.cfg parsing: absent / unknown reads off, allow50=1 parses without error
        Rig r; r.Open(); StoreStatus st;
        std::string t = Text(DefaultConfig(controls::GetControlApi()));
        CHECK(t.find("allow50") != std::string::npos);
        const size_t at = t.find("allow50=0\n"); CHECK(at != std::string::npos); t.erase(at, 10); Put(r, t);
        CHECK(!Load(r.files, &st).allow50 && st == StoreStatus::Ok);
        Put(r, t + "allow50=maybe\n"); CHECK(!Load(r.files, &st).allow50 && st == StoreStatus::Ok);
        Put(r, t + "allow50=1\n"); CHECK(Load(r.files, &st).allow50 && st == StoreStatus::Ok);
    }
    {   // legacy file with allow50=1 migrated at startup by NativeRetire (boot_.allow50 == false):
        // opening Settings MUST NOT show Allow50 and MUST NOT report Dirty(), RestartNeeded(), WouldNeedRestart(), or PictureOrLanguageChanged()
        Rig r; Config c = DefaultConfig(controls::GetControlApi()); c.allow50 = true; Put(r, Text(c));
        Config run = c; NativeRetire(run); CHECK(!run.allow50);
        r.boot = run;
        CHECK(r.Open().status == StoreStatus::Ok);
        CHECK(!r.model.RowShown(NativeRow::Allow50) && !r.model.Dirty() && !r.model.RestartNeeded() && !r.model.WouldNeedRestart() && !r.model.PictureOrLanguageChanged());
        // saving an unrelated live setting must NOT spuriously trigger RestartNeeded()
        CHECK(r.model.Step(NativeRow::LessMotion, 1) == StepResult::Changed && !r.model.WouldNeedRestart() && !r.model.PictureOrLanguageChanged());
        CHECK(r.model.Save().status == StoreStatus::Ok && !r.model.RestartNeeded() && !r.model.Dirty());
    }
    {   // in a session: still hidden / unavailable
        Rig r; r.Open(); r.model.SetWhere(NativeWhere::Session);
        CHECK(!r.model.RowShown(NativeRow::Allow50) && r.model.Blocked(NativeRow::Allow50) == RowBlock::Unavailable && r.model.Step(NativeRow::Allow50, 1) == StepResult::Unavailable);
    }
    {   // env.txt's GXV_HRW_ALLOW50 still recognized as override key if queried
        Rig r; r.Open(); gEnv = {"GXV_HRW_ALLOW50"};
        CHECK(r.model.IsOverridden(NativeRow::Allow50) && !r.model.RowShown(NativeRow::Allow50) && !r.model.CanStep(NativeRow::Allow50));
        gEnv.clear();
    }
}

int main() {
    Assist60();
    Allow50();
    SimpleRows(); LiveRows(); WhereAndFlags(); Retire(); C181File_(); BootCheck(); HoldMenu(); QueuedSave(); LanguageRow(); PsTv(); EnvPresence(); DamagedReport(); PutBack(); RelaunchWait(); PictureParity(); TechnicalKeysKept(); ReadOnly(); WriteErrors(); Restart(); Overridden(); Binding(); DamagedMain(); HostAndFlag();
    std::puts("settings_native: all passed");
}
