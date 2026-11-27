// The stale-settings guard (port/settings_guard.h) on memory files: a 2.2.1-style file, an already-stamped file, a corrupt file, a backup that is already there, and the
// safety rules (never overwrite a backup, never lose a line, never change what the file reads as). Build: tools/test_player_settings.sh guard
#include "port/settings_guard.h"
#include "player_settings_files.h"
#include <cassert>
#include <iostream>
#include <set>
#include <string>
using namespace port;
using settings::GuardOutcome;
namespace {
const controls::ControlApi& Api() { return controls::GetControlApi(); }
const char* kPath = "player-settings.cfg";
const char* kBackup = "player-settings.before-3.0.cfg";
const char* kOldBak = "player-settings.before-3.0-bak.cfg";
const char* kUnreadable = "player-settings.unreadable-3.0.cfg";
std::string Fresh(const settings::Config& c) {
    std::string out; settings::Document blank{settings::DefaultConfig(Api()), "schema=1\n", true};
    assert(settings::SerializeDocument(blank, c, Api(), out).status == settings::StoreStatus::Ok); return out;
}
std::string Drop(std::string text, const char* key) {
    const auto at = text.find(std::string(key) + "=");
    if (at != std::string::npos && (at == 0 || text[at - 1] == '\n')) text.erase(at, text.find('\n', at) + 1 - at);
    return text;
}
std::string Set(std::string text, const std::string& key, const std::string& value) {
    const auto at = text.find(key + "=");
    if (at == std::string::npos) return text + key + "=" + value + "\n";
    text.replace(at, text.find('\n', at) - at, key + "=" + value); return text;
}
// What release 2.2.1 wrote: only the required keys (schema, fps_limit, auto_quality, profile, language, reduced_motion, bindings) plus the 2.x optional keys that existed then
// and none of the keys this build added. The player's own choices are in it: a language, a changed binding, a hand-written line and a comment.
std::string Release221() {
    settings::Config c = settings::DefaultConfig(Api()); c.language = "French"; c.reducedMotion = true; c.profile = controls::ProfileId::PlayStation; c.profileChosen = true;
    c.fpsLimit = 0; c.performance60 = false;   // the old default: fps_limit=0 with no perf_mode
    std::string t = Fresh(c);
    for (const char* k : {"perf_mode", "above_native", "mario_first", "anti_aliasing", "picture_sharpness", "effects_quality", "core_plan", "gxm_renderer", "logic_overlap", "material_updates", "far_animation", "game_speed", "motion_level", "motion_source", "atomic_saves"}) t = Drop(t, k);
    return "# my own note\nsome_future_key = keep me\n" + t;
}
settings::Config Read(const std::string& text) {
    settings::Document d; const auto r = settings::ParseDocument(text, Api(), d);
    if (r.status != settings::StoreStatus::Ok) std::cerr << "parse: " << r.message << "\n";
    assert(r.status == settings::StoreStatus::Ok); return d.config;
}
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
settings::GuardReport Guard(UiFiles& fs, const char* version = "3.0.0") { auto ops = fs.ops(); return settings::RunSettingsGuard(kPath, version, Api(), ops); }
// Memory files with faults: names that stat as a directory, names that stat fine but cannot be opened, and a stat error.
struct Faulty {
    UiFiles* fs; settings::FileOps base{};
    explicit Faulty(UiFiles* f) : fs(f) {}
    std::set<std::string> dirs, noOpen, statError, noRename, noWrite;
    static Faulty& get(void* p) { return *static_cast<Faulty*>(p); }
    settings::FileOps ops() {
        base = fs->ops();
        return {this,
            [](void* p, const char* n) -> int64_t { auto& s = get(p); if (s.noOpen.count(n)) return -EIO; return s.base.openRead(s.base.context, n); },
            [](void* p, const char* n) -> int64_t { auto& s = get(p); if (s.noWrite.count(n)) return -EIO; return s.base.openWrite(s.base.context, n); },
            [](void* p, int64_t h, void* b, size_t z) -> int64_t { auto& s = get(p); return s.base.read(s.base.context, h, b, z); },
            [](void* p, int64_t h, const void* b, size_t z) -> int64_t { auto& s = get(p); return s.base.write(s.base.context, h, b, z); },
            [](void* p, int64_t h) { auto& s = get(p); return s.base.sync(s.base.context, h); },
            [](void* p, int64_t h) { auto& s = get(p); return s.base.close(s.base.context, h); },
            [](void* p, const char* n, settings::FileInfo& i) { auto& s = get(p); if (s.statError.count(n)) return -EIO; int r = s.base.stat(s.base.context, n, i); if (s.dirs.count(n)) { i = {4096, false}; return 0; } return r; },
            [](void* p, const char* a, const char* b) { auto& s = get(p); if (s.noRename.count(a)) return -EIO; return s.base.rename(s.base.context, a, b); },
            [](void* p, const char* n) { auto& s = get(p); return s.base.remove(s.base.context, n); }};
    }
};
std::string Crlf(const std::string& t) { std::string o; for (char c : t) { if (c == '\n') o += '\r'; o += c; } return o; }
}  // namespace
int main() {
    const std::string old = Release221();
    const auto oldConfig = Read(old);
    assert(settings::Mode30(oldConfig));   // the 2.2.x default reads as the 30 mode

    // ---- no file: nothing is created
    { UiFiles fs; auto r = Guard(fs); assert(r.outcome == GuardOutcome::NoFile && fs.data.empty() && r.line.empty()); }

    // ---- 1. a 2.2.1-style file
    {
        UiFiles fs; fs.data[kPath] = old; auto ops = fs.ops();
        const auto r = Guard(fs);
        std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Migrated && r.backupMade && !r.backupExisted && r.changedLines == 2 && !r.line.empty());
        assert(fs.data[kBackup] == old);                                                     // the backup is the old file, byte for byte
        const std::string now = fs.data[kPath];
        assert(now.rfind("# player-settings written by Galaxy 3.0.0\n", 0) == 0);           // stamped on the first line
        assert(Has(now, "# my own note\n") && Has(now, "some_future_key = keep me\n"));     // comments and unknown lines kept
        assert(Has(now, "fps_limit=30\n") && Has(now, "perf_mode=0\n"));                    // the changed meaning written out
        for (const char* k : {"above_native", "mario_first", "anti_aliasing", "gxm_renderer", "motion_level", "far_animation"}) assert(!Has(now, std::string(k) + "="));   // new keys NOT pinned: defaults by absence
        assert(Read(now).fpsLimit == 30 && settings::Equal(Read(now), oldConfig));           // reads exactly as before
        for (const auto& l : {std::string("language=French\n"), std::string("profile=playstation\n"), std::string("reduced_motion=1\n")}) assert(Has(now, l));
        assert(fs.data.count(std::string(kPath) + ".bak") && fs.data[std::string(kPath) + ".bak"] == old);   // the store's own rotation
        assert(!fs.data.count(std::string(kPath) + ".tmp") && !fs.data.count(std::string(kBackup) + ".tmp"));
        settings::Document loaded; assert(settings::LoadDocument(kPath, Api(), ops, loaded).status == settings::StoreStatus::Ok && settings::Equal(loaded.config, oldConfig));
        // the Settings screen's save afterwards keeps the stamp, the note and the unknown line
        settings::Document saved; auto c = loaded.config; c.language = "German";
        assert(settings::SaveDocument(kPath, loaded, c, Api(), ops, saved).status == settings::StoreStatus::Ok);
        assert(fs.data[kPath].rfind("# player-settings written by Galaxy 3.0.0\n", 0) == 0 && Has(fs.data[kPath], "some_future_key = keep me\n"));
        // starting again: current, nothing happens, the backup is untouched
        const std::string before = fs.data[kPath];
        const auto again = Guard(fs); assert(again.outcome == GuardOutcome::Current && fs.data[kPath] == before && fs.data[kBackup] == old && again.line.empty());
    }

    // ---- 2. already-stamped files
    {
        for (const char* stamp : {"3.0.0", "3.0.7", "3.1.0", "12.0.0"}) {
            UiFiles fs; fs.data[kPath] = std::string("# player-settings written by Galaxy ") + stamp + "\n" + old; const std::string same = fs.data[kPath];
            assert(Guard(fs).outcome == GuardOutcome::Current && fs.data[kPath] == same && !fs.data.count(kBackup) && fs.data.size() == 1);
        }
        UiFiles fs; fs.data[kPath] = "# player-settings written by Galaxy 2.9.0\n" + old;      // an older stamp: the guard runs and replaces the stamp (one stamp line only)
        assert(Guard(fs).outcome == GuardOutcome::Migrated && Has(fs.data[kPath], "Galaxy 3.0.0\n") && !Has(fs.data[kPath], "Galaxy 2.9.0"));
        // a 2.3-style file (perf_mode already there, no stamp): only the stamp is added, every other byte stays, a backup is still made
        UiFiles fs2; settings::Config c = settings::DefaultConfig(Api()); c.language = "Spanish"; const std::string t23 = "# kept\n" + Fresh(c); fs2.data[kPath] = t23;
        const auto r = Guard(fs2); assert(r.outcome == GuardOutcome::Migrated && r.changedLines == 0 && fs2.data[kBackup] == t23);
        assert(fs2.data[kPath] == "# player-settings written by Galaxy 3.0.0\n" + t23);
    }

    // ---- 3. a corrupt file
    {
        for (const std::string& bad : {std::string("schema=1\nfps_limit"), std::string("schema=1\nreduced_motion=yes\n"), std::string("schema=1\n"), std::string("garbage\x01 not a settings file\n"), std::string()}) {
            UiFiles fs; fs.data[kPath] = bad;
            const auto r = Guard(fs); std::cout << r.line << "\n";
            assert(r.outcome == GuardOutcome::Unreadable && fs.data[kPath] == bad && !fs.data.count(kBackup));   // untouched, no stamp, no before-3.0 copy
            if (bad.empty()) { assert(!fs.data.count(kUnreadable) && Has(r.line, "empty")); continue; }          // a zero-byte file: no copy, the log says so
            assert(fs.data.count(kUnreadable) && fs.data[kUnreadable] == bad);                                  // one copy of the bytes kept
            fs.data[kUnreadable] = "EARLIER COPY";                                                              // a later run never overwrites it
            assert(Guard(fs).outcome == GuardOutcome::Unreadable && fs.data[kUnreadable] == "EARLIER COPY");
        }
        UiFiles big; big.data[kPath] = std::string(settings::kDocumentLimit + 1, 'x');
        assert(Guard(big).outcome == GuardOutcome::Unreadable && big.data[kPath].size() == settings::kDocumentLimit + 1 && big.data.size() == 2 && big.data.count(kUnreadable));   // (D4: a byte copy is kept, the file is not touched)
        UiFiles newer; newer.data[kPath] = "schema=2\nfuture=keep\n";
        assert(Guard(newer).outcome == GuardOutcome::NewerFormat && newer.data[kPath] == "schema=2\nfuture=keep\n" && newer.data.size() == 1);
        // a corrupt file with a good .bak: the guard leaves both; the loader recovers from .bak as before
        UiFiles rec; rec.data[kPath] = "schema=1\nfps_limit"; rec.data[std::string(kPath) + ".bak"] = old; auto ops = rec.ops(); Guard(rec);
        settings::Document d; assert(settings::LoadDocument(kPath, Api(), ops, d).status == settings::StoreStatus::Recovered);
    }

    // ---- 4. a backup is already present: kept, never overwritten, the file is still migrated
    {
        UiFiles fs; fs.data[kPath] = old; fs.data[kBackup] = "THE OLDEST COPY";
        const auto r = Guard(fs); std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Migrated && r.backupExisted && !r.backupMade && fs.data[kBackup] == "THE OLDEST COPY");
        assert(Has(fs.data[kPath], "perf_mode=0\n") && settings::Equal(Read(fs.data[kPath]), oldConfig));
    }

    // ---- 5. the backup cannot be written: the file stays exactly as it is
    {
        UiFiles fs; fs.data[kPath] = old; fs.failWrite = true;
        const auto r = Guard(fs); assert(r.outcome == GuardOutcome::BackupFailed && fs.data[kPath] == old && !fs.data.count(kBackup));
    }

    // ---- 6. line endings and a 1.2 beta motion_gain
    {
        UiFiles fs; fs.data[kPath] = Crlf(Set(old, "motion_gain", "150")); const std::string src = fs.data[kPath];
        const auto r = Guard(fs); assert(r.outcome == GuardOutcome::Migrated && r.changedLines == 3);   // fps_limit, perf_mode, motion_level
        const std::string now = fs.data[kPath];
        assert(fs.data[kBackup] == src && Has(now, "motion_gain=150\r\n") && Has(now, "motion_level=") && Has(now, "perf_mode=0\r\n"));
        for (size_t at = 0; (at = now.find('\n', at)) != std::string::npos; ++at) assert(at > 0 && now[at - 1] == '\r');   // every line still ends CRLF
        assert(settings::Equal(Read(now), Read(src)) && Read(now).tiltLevel == Read(src).tiltLevel);
    }
    // ---- 9. D1: the backup counts as present only if it is a regular, non-empty file that reads back
    {
        const auto untouched = [&](UiFiles& fs, Faulty& fl, const std::string& before) {
            auto ops = fl.ops(); const auto r = settings::RunSettingsGuard(kPath, "3.0.0", Api(), ops);
            std::cout << r.line << "\n";
            assert(r.outcome == GuardOutcome::BackupFailed && !r.backupMade && fs.data[kPath] == before && !fs.data.count(std::string(kPath) + ".bak") && !fs.data.count(std::string(kBackup) + ".tmp"));
        };
        { UiFiles fs; fs.data[kPath] = old; fs.data[kBackup] = "x"; Faulty fl{&fs}; fl.dirs.insert(kBackup); untouched(fs, fl, old); assert(fs.data[kBackup] == "x"); }   // a directory of that name
        { UiFiles fs; fs.data[kPath] = old; fs.data[kBackup] = ""; Faulty fl{&fs}; untouched(fs, fl, old); assert(fs.data[kBackup].empty()); }                           // a zero-size file
        { UiFiles fs; fs.data[kPath] = old; fs.data[kBackup] = "present but unreadable"; Faulty fl{&fs}; fl.noOpen.insert(kBackup); untouched(fs, fl, old); assert(fs.data[kBackup] == "present but unreadable"); }   // cannot be read back
        { UiFiles fs; fs.data[kPath] = old; Faulty fl{&fs}; fl.statError.insert(kBackup); untouched(fs, fl, old); }                                                     // a stat error
        // a good backup is still kept as it is
        { UiFiles fs; fs.data[kPath] = old; fs.data[kBackup] = "GOOD"; Faulty fl{&fs}; auto ops = fl.ops(); const auto r = settings::RunSettingsGuard(kPath, "3.0.0", Api(), ops); assert(r.outcome == GuardOutcome::Migrated && r.backupExisted && fs.data[kBackup] == "GOOD"); }
    }

    // ---- 10. D3: the player's own .bak is moved to a second before-3.0 name by the first-boot rewrite, never removed
    {
        UiFiles fs; fs.data[kPath] = old; const std::string bak = std::string(kPath) + ".bak"; fs.data[bak] = "THE PLAYER'S OLDER FILE\n";
        const auto r = Guard(fs); std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Migrated && r.oldBackupMoved && r.oldBackupPath == kOldBak);
        assert(fs.data[kOldBak] == "THE PLAYER'S OLDER FILE\n");                       // the old .bak's bytes exist under the new name
        assert(fs.data[bak] == old && fs.data[kBackup] == old);                          // the normal rotation then made the pre-rewrite file the .bak
        assert(Has(r.line, kOldBak));
        // ordinary saves afterwards keep the normal rotation (and leave the moved copy alone)
        auto ops = fs.ops(); settings::Document loaded; settings::LoadDocument(kPath, Api(), ops, loaded); auto c = loaded.config; c.language = "German"; settings::Document saved;
        const std::string beforeSave = fs.data[kPath];
        assert(settings::SaveDocument(kPath, loaded, c, Api(), ops, saved).status == settings::StoreStatus::Ok && fs.data[bak] == beforeSave && fs.data[kOldBak] == "THE PLAYER'S OLDER FILE\n");
        // the first name taken: the next numbered name is used, nothing is overwritten and the file is migrated
        UiFiles busy; busy.data[kPath] = old; busy.data[bak] = "KEEP ME"; busy.data[kOldBak] = "SOMETHING ELSE";
        const auto b = Guard(busy); assert(b.outcome == GuardOutcome::Migrated && busy.data[kOldBak] == "SOMETHING ELSE" && busy.data["player-settings.before-3.0-bak-2.cfg"] == "KEEP ME" && b.oldBackupPath == "player-settings.before-3.0-bak-2.cfg");
        // a .bak byte for byte equal to an existing copy: nothing moved, ordinary rotation
        UiFiles same; same.data[kPath] = old; same.data[bak] = "SAME BYTES"; same.data[kOldBak] = "SAME BYTES";
        const auto sm = Guard(same); assert(sm.outcome == GuardOutcome::Migrated && !sm.oldBackupMoved && same.data[kOldBak] == "SAME BYTES" && same.data[bak] == old && !same.data.count("player-settings.before-3.0-bak-2.cfg"));
        UiFiles sameFirst; sameFirst.data[kPath] = old; sameFirst.data[bak] = old; assert(Guard(sameFirst).outcome == GuardOutcome::Migrated && !sameFirst.data.count(kOldBak));   // equal to the before-3.0 copy made this run
        // every name taken: the .bak stays, the file is not rewritten (and a repeat boot is the same, no damage)
        UiFiles full; full.data[kPath] = old; full.data[bak] = "KEEP ME";
        full.data[kOldBak] = "t1"; for (int i = 2; i <= 9; ++i) full.data["player-settings.before-3.0-bak-" + std::to_string(i) + ".cfg"] = "t" + std::to_string(i);
        for (int rep = 0; rep < 2; ++rep) { const auto fl2 = Guard(full); assert(fl2.outcome == GuardOutcome::OldBackupBlocked && full.data[kPath] == old && full.data[bak] == "KEEP ME" && full.data[kOldBak] == "t1"); }
        // the rewrite fails after the move and the .bak cannot be put back: the line says where it lives
        UiFiles wf; wf.data[kPath] = old; wf.data[bak] = "PLAYER BAK"; Faulty wff{&wf}; wff.noRename.insert(kPath); auto wops = wff.ops();
        const auto w = settings::RunSettingsGuard(kPath, "3.0.0", Api(), wops);
        assert(w.outcome == GuardOutcome::WriteFailed && wf.data[kPath] == old);
        assert(wf.data.count(bak) ? (wf.data[bak] == "PLAYER BAK") : (w.oldBackupMoved && wf.data[w.oldBackupPath] == "PLAYER BAK" && Has(w.line, w.oldBackupPath)));
        // the rename fails: same
        UiFiles nr; nr.data[kPath] = old; nr.data[bak] = "KEEP ME"; Faulty fl{&nr}; fl.noRename.insert(bak); auto nops = fl.ops();
        const auto n = settings::RunSettingsGuard(kPath, "3.0.0", Api(), nops); assert(n.outcome == GuardOutcome::OldBackupBlocked && nr.data[kPath] == old && nr.data[bak] == "KEEP ME" && !nr.data.count(kOldBak));
        // no .bak: nothing is moved
        UiFiles none; none.data[kPath] = old; const auto z = Guard(none); assert(z.outcome == GuardOutcome::Migrated && !z.oldBackupMoved && !none.data.count(kOldBak));
    }

    // ---- 11. D4: steps owed by the full version compare; Current only when none is owed and major.minor matches
    {
        static int ran = 0;
        const settings::GuardStep patch[] = {{"3.0.1", "patch step", [](const std::string& t, const settings::Config&, const controls::ControlApi&, std::vector<std::string>& notes, unsigned& changed) -> std::string {
            ++ran; notes.push_back("patch step ran"); ++changed; return t + "# patch step ran\n"; }}};
        const std::string stamped = "# player-settings written by Galaxy 3.0.0\n" + old;
        auto run = [&](UiFiles& fs, const char* version) { auto ops = fs.ops(); return settings::RunSettingsGuardSteps(kPath, version, Api(), ops, patch, 1); };
        { UiFiles fs; fs.data[kPath] = stamped; const auto r = run(fs, "3.0.0"); assert(r.outcome == GuardOutcome::Current && ran == 0 && fs.data[kPath] == stamped); }   // the step is for a later release than this build
        { UiFiles fs; fs.data[kPath] = stamped; const auto r = run(fs, "3.0.1");                                                                                   // owed: 3.0.1 > 3.0.0
          std::cout << r.line << "\n";
          assert(r.outcome == GuardOutcome::Migrated && ran == 1 && r.changedLines == 1 && Has(fs.data[kPath], "# patch step ran\n") && fs.data[kPath].rfind("# player-settings written by Galaxy 3.0.1\n", 0) == 0);
          assert(!fs.data.count(kBackup) && !fs.data.count(kOldBak) && fs.data[std::string(kPath) + ".bak"] == stamped);   // same release line: no new before-3.0 copy, ordinary rotation
          assert(settings::Equal(Read(fs.data[kPath]), Read(stamped)));
          const auto again = run(fs, "3.0.1"); assert(again.outcome == GuardOutcome::Current && ran == 1); }                                                      // not owed any more
        { UiFiles fs; fs.data[kPath] = "# player-settings written by Galaxy 3.0.1\n" + old; assert(run(fs, "3.0.1").outcome == GuardOutcome::Current && ran == 1); }
        { UiFiles fs; fs.data[kPath] = stamped; assert(run(fs, "3.0.7").outcome == GuardOutcome::Migrated && ran == 2); }   // a 3.0.7 build owes the 3.0.1 step to a 3.0.0 file
        // an older line: the first-boot rewrite with the backup, and the owed steps include the patch step too
        { UiFiles fs; fs.data[kPath] = "# player-settings written by Galaxy 2.9.0\n" + old; const auto r = run(fs, "3.0.1"); assert(r.outcome == GuardOutcome::Migrated && r.backupMade && ran == 3); }
        // major.minor not matching with no step owed: restamped with its own backup (3.1 build on a 3.0 file)
        { UiFiles fs; const std::string at301 = "# player-settings written by Galaxy 3.0.1\n" + old; fs.data[kPath] = at301; auto ops = fs.ops(); const auto r = settings::RunSettingsGuardSteps(kPath, "3.1.0", Api(), ops, patch, 1);
          assert(r.outcome == GuardOutcome::Migrated && r.backupMade && fs.data["player-settings.before-3.1.cfg"] == at301 && ran == 3); }
        // a stamp from a newer line under this build: left alone
        { UiFiles fs; fs.data[kPath] = "# player-settings written by Galaxy 3.2.0\n" + old; assert(run(fs, "3.0.9").outcome == GuardOutcome::Current && ran == 3); }
    }

    // ---- 12. D4: a too-large file gets a byte copy (name free) and no rewrite; a stamped file that does not parse the same
    {
        UiFiles big; const std::string text(settings::kDocumentLimit + 5, 'x'); big.data[kPath] = text;
        const auto r = Guard(big); std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Unreadable && r.backupMade && big.data[kPath] == text && big.data[kUnreadable] == text && !big.data.count(kBackup));
        big.data[kUnreadable] = "EARLIER"; assert(Guard(big).outcome == GuardOutcome::Unreadable && big.data[kUnreadable] == "EARLIER");   // never over a name that is taken
        UiFiles dirTaken; dirTaken.data[kPath] = text; dirTaken.data[kUnreadable] = "x"; Faulty fl{&dirTaken}; fl.dirs.insert(kUnreadable); auto ops = fl.ops();
        assert(settings::RunSettingsGuard(kPath, "3.0.0", Api(), ops).outcome == GuardOutcome::Unreadable && dirTaken.data[kUnreadable] == "x" && dirTaken.data[kPath] == text);
        for (const char* stamp : {"3.0.0", "3.4.0", "2.9.0"}) {
            UiFiles fs; const std::string bad = std::string("# player-settings written by Galaxy ") + stamp + "\nschema=1\nfps_limit"; fs.data[kPath] = bad;
            const auto s = Guard(fs); std::cout << s.line << "\n";
            assert(s.outcome == GuardOutcome::Unreadable && s.backupMade && fs.data[kPath] == bad && fs.data[kUnreadable] == bad && !fs.data.count(kBackup));
            fs.data[kUnreadable] = "EARLIER"; assert(Guard(fs).outcome == GuardOutcome::Unreadable && fs.data[kUnreadable] == "EARLIER" && fs.data[kPath] == bad);
        }
    }

    // ---- 13. the optional key the smallfix branch adds (atomic_saves, sorts before reduced_motion): kept byte for byte, never required, same Config before and after
    {
        UiFiles fs; fs.data[kPath] = "atomic_saves=1\n" + old;   // (an unknown key on a build without that branch, a managed one with it: either way kept and read the same)
        const auto before = Read(fs.data[kPath]);
        const auto r = Guard(fs); assert(r.outcome == GuardOutcome::Migrated);
        assert(Has(fs.data[kPath], "atomic_saves=1\n") && settings::Equal(Read(fs.data[kPath]), before));
        // a file that has the key and is already stamped is Current
        UiFiles st; st.data[kPath] = "# player-settings written by Galaxy 3.0.0\natomic_saves=0\n" + old; assert(Guard(st).outcome == GuardOutcome::Current);
    }
    // ---- 14. a missing settings file with a .bak (a cut between the store's two renames): migrated from the .bak, backup first
    {
        const std::string bak = std::string(kPath) + ".bak";
        UiFiles fs; fs.data[bak] = old;
        const auto r = Guard(fs); std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Migrated && r.backupMade && fs.data[kBackup] == old && fs.data[bak] == old && !r.oldBackupMoved && !fs.data.count(kOldBak));
        assert(Has(fs.data[kPath], "perf_mode=0\n") && settings::Equal(Read(fs.data[kPath]), oldConfig) && fs.data[kPath].rfind("# player-settings written by Galaxy 3.0.0\n", 0) == 0);
        assert(Has(r.line, ".bak"));
        auto ops = fs.ops(); settings::Document d; assert(settings::LoadDocument(kPath, Api(), ops, d).status == settings::StoreStatus::Ok && settings::Mode30(d.config));   // the picture mode is the old one
        assert(Guard(fs).outcome == GuardOutcome::Current);
        // an already-stamped .bak: nothing to do. A corrupt .bak: left, copy kept. Neither: NoFile.
        UiFiles st; st.data[bak] = "# player-settings written by Galaxy 3.0.0\n" + old; assert(Guard(st).outcome == GuardOutcome::Current && !st.data.count(kPath));
        UiFiles bad; bad.data[bak] = "schema=1\nfps_limit"; assert(Guard(bad).outcome == GuardOutcome::Unreadable && !bad.data.count(kPath) && bad.data[bak] == "schema=1\nfps_limit");
        UiFiles nb; nb.data[kBackup] = "x"; assert(Guard(nb).outcome == GuardOutcome::NoFile);
        // the backup cannot be made: no file is written
        UiFiles bf; bf.data[bak] = old; bf.data[kBackup] = ""; assert(Guard(bf).outcome == GuardOutcome::BackupFailed && !bf.data.count(kPath));
    }
    // ---- 15. the lone-.bak case with a leftover .tmp from the cut save: a fully parsing one is the (newer) source, any one is set aside, never truncated
    {
        const std::string bak = std::string(kPath) + ".bak", tmp = std::string(kPath) + ".tmp", aside = "player-settings.before-3.0-tmp.cfg";
        std::string newest = Set(old, "language", "German");   // the player's newest choice, in the 2.2.1 layout
        assert(Read(newest).language == "German");
        UiFiles fs; fs.data[bak] = old; fs.data[tmp] = newest;
        const auto r = Guard(fs); std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Migrated && r.tmpSetAside == aside && fs.data[aside] == newest && !fs.data.count(tmp) && fs.data[kOldBak] == old && !fs.data.count(bak));   // the .bak is the last committed file: kept as -bak
        assert(fs.data[kBackup] == newest && Read(fs.data[kPath]).language == "German" && Has(fs.data[kPath], "perf_mode=0\n"));
        // a .tmp that does not parse: the .bak is the source, the .tmp is still set aside byte for byte
        UiFiles bad; bad.data[bak] = old; bad.data[tmp] = "schema=1\nfps_limit"; const auto b = Guard(bad);
        assert(b.outcome == GuardOutcome::Migrated && bad.data[aside] == "schema=1\nfps_limit" && !bad.data.count(tmp) && bad.data[kBackup] == old && Read(bad.data[kPath]).language == "French");
        // the first aside name taken: the next one; all taken: the file is not written
        UiFiles two; two.data[bak] = old; two.data[tmp] = "x\n"; two.data[aside] = "KEEP"; assert(Guard(two).outcome == GuardOutcome::Migrated && two.data["player-settings.before-3.0-tmp-2.cfg"] == "x\n" && two.data[aside] == "KEEP");
        UiFiles full; full.data[bak] = old; full.data[tmp] = "x\n"; full.data[aside] = "t1"; for (int i = 2; i <= 9; ++i) full.data["player-settings.before-3.0-tmp-" + std::to_string(i) + ".cfg"] = "t";
        assert(Guard(full).outcome == GuardOutcome::OldBackupBlocked && !full.data.count(kPath) && full.data[tmp] == "x\n" && full.data[bak] == old && !full.data.count(kOldBak));   // blocked: the .bak is put back
    }
    // ---- 16. the guard's own likeliest cut (between the store's two renames of ITS write): old-release .bak + migrated current-stamped .tmp, no settings file. The loader ignores the
    // .tmp, so the guard must write the settings file from it (never "Current"); the .bak is kept; a second boot is Current.
    {
        const std::string bak = std::string(kPath) + ".bak", tmp = std::string(kPath) + ".tmp", aside = "player-settings.before-3.0-tmp.cfg";
        const std::string migrated = "# player-settings written by Galaxy 3.0.0\n" + Set(Set(old, "fps_limit", "30"), "perf_mode", "0");
        UiFiles fs; fs.data[bak] = old; fs.data[tmp] = migrated;
        const auto r = Guard(fs); std::cout << r.line << "\n";
        assert(r.outcome == GuardOutcome::Migrated && r.tmpSetAside.empty() && !fs.data.count(aside) && !fs.data.count(tmp));   // renamed straight into place: no copy, no window
        assert(fs.data.count(kPath) && fs.data[kPath] == migrated && Read(fs.data[kPath]).language == "French");
        assert(fs.data[kOldBak] == old && !fs.data.count(kBackup));   // the last committed file kept; no before-3.0 copy of a current file
        assert(Guard(fs).outcome == GuardOutcome::Current);
        // a cut after the set-aside but before the write: only the older .bak is left, the migrated values are in the -tmp copy, and the next boot migrates the .bak
        UiFiles cut; cut.data[bak] = old; cut.data[aside] = migrated;   // a cut after the set-aside: the .bak is still in place (the move waits for the write)
        assert(Guard(cut).outcome == GuardOutcome::Migrated && Read(cut.data[kPath]).language == "French" && cut.data[aside] == migrated);
        // a cut mid-write (partial new .tmp beside the .bak, the first aside name taken): never NoFile; the partial one is set aside byte for byte, the .bak migrated
        UiFiles mid; mid.data[bak] = old; mid.data[aside] = migrated; mid.data[tmp] = migrated.substr(0, migrated.size() / 2);
        const auto m = Guard(mid); assert(m.outcome == GuardOutcome::Migrated && mid.data.count(kPath) && Read(mid.data[kPath]).language == "French" && mid.data["player-settings.before-3.0-tmp-2.cfg"] == migrated.substr(0, migrated.size() / 2));
        // a cut after the write but before the late .bak move: the new file beside the .bak, readable, Current
        UiFiles late; late.data[kPath] = migrated; late.data[bak] = old; late.data[aside] = migrated;
        assert(Guard(late).outcome == GuardOutcome::Current && Read(late.data[kPath]).language == "French" && late.data[bak] == old);
        // the .bak equal to the before-3.0 copy: no duplicate -bak copy, the .bak stays
        UiFiles eq; eq.data[bak] = old; eq.data[kBackup] = old; eq.data[tmp] = migrated;
        assert(Guard(eq).outcome == GuardOutcome::Migrated && !eq.data.count(kOldBak) && eq.data[bak] == old && eq.data[kPath] == migrated);
        // the direct rename of a current .tmp fails: WriteFailed, nothing moved, the .bak (what the loader reads) and the .tmp are untouched
        UiFiles dr; dr.data[bak] = old; dr.data[tmp] = migrated; Faulty drf{&dr}; drf.noRename.insert(tmp); auto drops = drf.ops();
        const auto drr = settings::RunSettingsGuard(kPath, "3.0.0", Api(), drops);
        assert(drr.outcome == GuardOutcome::WriteFailed && dr.data[tmp] == migrated && dr.data[bak] == old && !dr.data.count(kPath) && !dr.data.count(aside) && !dr.data.count(kOldBak) && !drr.oldBackupMoved);
        // the write fails after the set-aside (an older-stamped .tmp, so it is a copy that is written): the set-aside goes back to .tmp, the .bak is untouched, no file is lost
        UiFiles wf; wf.data[bak] = old; wf.data[tmp] = Set(old, "language", "German"); Faulty wff{&wf}; wff.noWrite.insert(tmp); auto wops = wff.ops();
        const auto wr = settings::RunSettingsGuard(kPath, "3.0.0", Api(), wops); std::cout << wr.line << "\n";
        assert(wr.outcome == GuardOutcome::WriteFailed && wf.data[tmp] == Set(old, "language", "German") && !wf.data.count(aside) && wf.data[bak] == old && !wf.data.count(kPath));
        // at no step of a normal run is there neither file nor .bak: a failing late move still ends Migrated, file written, .bak kept
        UiFiles lf; lf.data[bak] = old; lf.data[tmp] = migrated; Faulty lff{&lf}; lff.noRename.insert(bak); auto lops = lff.ops();
        const auto lr = settings::RunSettingsGuard(kPath, "3.0.0", Api(), lops); std::cout << lr.line << "\n";
        assert(lr.outcome == GuardOutcome::Migrated && lf.data[kPath] == migrated && lf.data[bak] == old && !lf.data.count(aside) && Has(lr.line, "could not be moved aside"));
    }
    // ---- 7. a bad version string touches nothing
    { UiFiles fs; fs.data[kPath] = old; assert(Guard(fs, "three").outcome == GuardOutcome::BadVersion && fs.data[kPath] == old && fs.data.size() == 1); }
    // ---- 8. the rule the whole guard rests on: a file with only the REQUIRED keys loads (new optional keys default); no key a 2.2.1 file lacks may be required
    {
        std::string minimal = Fresh(settings::DefaultConfig(Api()));
        for (const char* k : {"mouse_aim", "profile_chosen", "above_native", "mario_first", "anti_aliasing", "picture_sharpness", "hd_textures", "effects_quality", "depth_of_field", "core_plan", "gxm_renderer", "logic_overlap", "material_updates", "far_animation", "game_speed", "motion_level", "motion_source", "perf_mode", "atomic_saves"}) minimal = Drop(minimal, k);
        settings::Document d; assert(settings::ParseDocument(minimal, Api(), d).status == settings::StoreStatus::Ok);
    }
    // ---- 9. setfix31 (emulator run emu31b, N2): a file this build writes carries the release stamp, so the next start does not take it for a 2.2.1 file
    //      (before: a fresh install's first Settings save had no stamp; the next start made player-settings.before-3.0.cfg and -bak.cfg and "stamped" it)
    {
        settings::SetWriterStamp("");
        const std::string r221 = Release221(), freshDefault = Fresh(settings::DefaultConfig(Api()));   // (made before any stamp is set: these stand for files written by older builds)
        auto save = [&](UiFiles& fs, const settings::Config& c) {   // the writers' path: SaveDocument over the file as loaded (the native Settings' Save, its restart, Put Everything Back)
            auto ops = fs.ops(); settings::Document loaded; settings::LoadDocument(kPath, Api(), ops, loaded); loaded.onlyChanged = true;
            settings::Document saved; return settings::SaveDocument(kPath, loaded, c, Api(), ops, saved).status;
        };
        auto start = [&](UiFiles& fs) {   // what settings_runtime.cpp does at each start: the guard, then the writers' stamp when the guard owns the file
            const auto r = Guard(fs);
            settings::SetWriterStamp(settings::GuardFileIsCurrent(r.outcome) ? settings::GuardStampLine("3.0.0") : std::string());
            return r;
        };
        // a fresh install: no file; the first in-game save writes one; the next start finds it current: no backup, no stamp line added later
        UiFiles fs;
        assert(start(fs).outcome == GuardOutcome::NoFile);
        settings::Config c = settings::DefaultConfig(Api()); c.fpsLimit = 30; c.performance60 = false;
        assert(save(fs, c) == settings::StoreStatus::Ok);
        assert(fs.data[kPath].rfind("# player-settings written by Galaxy 3.0.0\n", 0) == 0);
        const auto r2 = start(fs);
        assert(r2.outcome == GuardOutcome::Current && !fs.data.count(kBackup) && !fs.data.count(kOldBak) && r2.line.empty());
        // a second save keeps exactly one stamp
        c.reducedMotion = true; assert(save(fs, c) == settings::StoreStatus::Ok);
        { size_t n = 0, at = 0; while ((at = fs.data[kPath].find("# player-settings written by", at)) != std::string::npos) ++n, ++at; assert(n == 1); }
        assert(Read(fs.data[kPath]).reducedMotion && start(fs).outcome == GuardOutcome::Current);
        // a genuinely old file is still migrated with its backup, exactly as before (the guard stamps it; the writers find it stamped)
        UiFiles old; old.data[kPath] = r221;
        const auto m = start(old);
        assert(m.outcome == GuardOutcome::Migrated && old.data.count(kBackup) && old.data[kBackup] == r221);
        assert(save(old, Read(old.data[kPath])) == settings::StoreStatus::Ok && start(old).outcome == GuardOutcome::Current);
        // an old file the guard could NOT migrate (its backup name is taken by an empty file: not a good copy) is not stamped by a save: the next start tries again, backup first
        UiFiles held; held.data[kPath] = r221; held.data[kBackup] = "";
        const auto b = start(held);
        assert(!settings::GuardFileIsCurrent(b.outcome) && settings::WriterStamp().empty());
        settings::Config hc = Read(held.data[kPath]); hc.tiltLevel = 7;
        assert(save(held, hc) == settings::StoreStatus::Ok && !Has(held.data[kPath], "# player-settings written by Galaxy"));
        held.data.erase(kBackup);
        const auto b2 = start(held);
        assert(b2.outcome == GuardOutcome::Migrated && held.data.count(kBackup));
        // a newer release's stamp is kept as it is
        UiFiles newer; newer.data[kPath] = "# player-settings written by Galaxy 3.2.0\n" + freshDefault;
        assert(start(newer).outcome == GuardOutcome::Current);
        settings::Config nc = Read(newer.data[kPath]); nc.reducedMotion = true; assert(save(newer, nc) == settings::StoreStatus::Ok);
        assert(newer.data[kPath].rfind("# player-settings written by Galaxy 3.2.0\n", 0) == 0 && !Has(newer.data[kPath], "Galaxy 3.0.0"));
        settings::SetWriterStamp("");
    }
    std::cout << "settings guard: all checks passed\n";
    return 0;
}
