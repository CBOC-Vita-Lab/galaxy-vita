// The stale-settings guard: see port/settings_guard.h.
#include "port/settings_guard.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace port::settings {
namespace {
const char kStampPrefix[] = "# player-settings written by Galaxy ";

struct Ver { int v[3] = {0, 0, 0}; bool valid = false; };
Ver ParseVer(const char* t) {
    Ver r; if (!t || !*t) return r;
    int n = 0; const char* p = t;
    while (*p) {
        if (*p < '0' || *p > '9' || n >= 3) return Ver();
        int value = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { value = value * 10 + (*p - '0'); if (++digits > 6) return Ver(); ++p; }
        r.v[n++] = value;
        if (*p == '.') { ++p; if (!*p) return Ver(); } else break;
    }
    if (*p) return Ver();
    r.valid = n >= 1; return r;
}
int Cmp(const Ver& a, const Ver& b, int parts = 3) { for (int i = 0; i < parts; ++i) if (a.v[i] != b.v[i]) return a.v[i] < b.v[i] ? -1 : 1; return 0; }

std::vector<std::string> SplitLines(const std::string& text) {   // each element keeps its own line ending
    std::vector<std::string> out;
    for (size_t at = 0; at < text.size();) { size_t end = text.find('\n', at); end = end == std::string::npos ? text.size() : end + 1; out.push_back(text.substr(at, end - at)); at = end; }
    return out;
}
std::string Body(const std::string& raw) {   // the line without ending, trimmed
    size_t b = raw.find_first_not_of(" \t\r\n"); if (b == std::string::npos) return "";
    size_t e = raw.find_last_not_of(" \t\r\n"); return raw.substr(b, e - b + 1);
}
bool KeyOf(const std::string& raw, std::string& key, std::string& value) {
    const std::string b = Body(raw);
    if (b.empty() || b[0] == '#' || b[0] == ';') return false;
    const size_t eq = b.find('='); if (eq == std::string::npos) return false;
    key = Body(b.substr(0, eq)); value = Body(b.substr(eq + 1)); return true;
}
bool IsStamp(const std::string& raw) { return raw.compare(0, sizeof(kStampPrefix) - 1, kStampPrefix) == 0; }
Ver ReadStamp(const std::string& text) {
    const auto lines = SplitLines(text);
    for (size_t i = 0; i < lines.size() && i < 8; ++i)
        if (IsStamp(lines[i])) return ParseVer(Body(lines[i].substr(sizeof(kStampPrefix) - 1)).c_str());
    return Ver();
}

bool ReadAll(const FileOps& f, const char* path, std::string& out, uint64_t limit) {
    FileInfo info{}; if (f.stat(f.context, path, info) < 0 || !info.regular || info.size > limit) return false;
    const int64_t h = f.openRead(f.context, path); if (h < 0) return false;
    out.assign(static_cast<size_t>(info.size), '\0'); size_t at = 0; bool ok = true;
    while (at < out.size()) { const auto n = f.read(f.context, h, &out[at], out.size() - at); if (n <= 0 || static_cast<uint64_t>(n) > out.size() - at) { ok = false; break; } at += static_cast<size_t>(n); }
    char extra = 0; if (ok && f.read(f.context, h, &extra, 1) != 0) ok = false;
    if (f.close(f.context, h) < 0) ok = false;
    return ok;
}
// Name states: Free only when stat says "no such file"; Good only for a regular, non-empty file that reads back; everything else (a directory, an empty file, a read or stat
// error) is Bad: it is never overwritten and never taken for a backup.
enum class Name { Free, Good, Bad };
constexpr uint64_t kCopyLimit = 1u << 20;   // the largest file the guard will copy byte for byte
constexpr int kMaxOldBackups = 9;           // -bak ... -bak-9
Name NameState(const FileOps& f, const char* path) {
    FileInfo i{}; const int st = f.stat(f.context, path, i);
    if (st == -ENOENT) return Name::Free;
    if (st < 0 || !i.regular || i.size == 0 || i.size > kCopyLimit) return Name::Bad;
    std::string back; return ReadAll(f, path, back, kCopyLimit) && !back.empty() ? Name::Good : Name::Bad;
}
bool Exists(const FileOps& f, const char* path) { return NameState(f, path) != Name::Free; }   // anything but "no such file" counts as present: never overwrite

// A new file made complete under a temporary name, synced, read back and compared, then renamed to a destination that does not exist. Nothing existing is replaced or deleted
// (only this function's own temporary on failure). `text` may be up to kCopyLimit bytes.
bool WriteNewFile(const FileOps& f, const std::string& dest, const std::string& text) {
    if (Exists(f, dest.c_str())) return false;
    const std::string tmp = dest + ".tmp";
    const int64_t h = f.openWrite(f.context, tmp.c_str()); if (h < 0) return false;
    size_t at = 0; bool ok = true;
    while (at < text.size()) { const auto n = f.write(f.context, h, text.data() + at, text.size() - at); if (n <= 0 || static_cast<uint64_t>(n) > text.size() - at) { ok = false; break; } at += static_cast<size_t>(n); }
    if (ok && f.sync(f.context, h) < 0) ok = false;
    if (f.close(f.context, h) < 0) ok = false;
    std::string back;
    if (ok && (!ReadAll(f, tmp.c_str(), back, text.size() + 1) || back != text)) ok = false;   // (an empty text is never written: see NameState)
    if (ok && f.rename(f.context, tmp.c_str(), dest.c_str()) < 0) ok = false;
    if (!ok) f.remove(f.context, tmp.c_str());
    return ok;
}

std::string MajorMinor(const char* version) { const Ver v = ParseVer(version); char b[32]; std::snprintf(b, sizeof b, "%d.%d", v.v[0], v.v[1]); return b; }
std::string WithSuffix(const char* path, const std::string& tag) {   // "x/player-settings.cfg" + "before-3.0" -> "x/player-settings.before-3.0.cfg"
    std::string p = path; const size_t dot = p.rfind('.'), slash = p.find_last_of("/:\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) return p.substr(0, dot) + "." + tag + p.substr(dot);
    return p + "." + tag;
}

// "key=value" lines this build writes for `cfg`, one per key (from the store's own serializer, so the spelling is the one every build reads).
std::map<std::string, std::string> Canonical(const Config& cfg, const controls::ControlApi& api) {
    std::map<std::string, std::string> out; std::string text;
    if (SerializeDocument({DefaultConfig(api), "schema=1\n", true}, cfg, api, text).status != StoreStatus::Ok) return out;
    for (const auto& l : SplitLines(text)) { std::string k, v; if (KeyOf(l, k, v)) out[k] = v; }
    return out;
}
// Sets (replaces or appends) `key` to `value`; the line keeps the file's ending style. Returns true when the text changed.
bool SetKey(std::vector<std::string>& lines, const std::string& key, const std::string& value, const std::string& ending) {
    for (auto& l : lines) {
        std::string k, v; if (!KeyOf(l, k, v) || k != key) continue;
        if (v == value) return false;
        const bool nl = !l.empty() && l.back() == '\n';
        l = key + "=" + value + (nl ? ending : ""); return true;
    }
    if (!lines.empty() && lines.back().back() != '\n') lines.back() += ending;
    lines.push_back(key + "=" + value + ending); return true;
}
bool Has(const std::vector<std::string>& lines, const char* key) { for (const auto& l : lines) { std::string k, v; if (KeyOf(l, k, v) && k == key) return true; } return false; }
std::string Value(const std::vector<std::string>& lines, const char* key) { for (const auto& l : lines) { std::string k, v; if (KeyOf(l, k, v) && k == key) return v; } return ""; }
std::string EndingOf(const std::vector<std::string>& lines) { for (const auto& l : lines) if (l.size() >= 2 && l[l.size() - 2] == '\r' && l.back() == '\n') return "\r\n"; return "\n"; }
std::string Join(const std::vector<std::string>& lines) { std::string s; for (const auto& l : lines) s += l; return s; }

// 3.0, step 1: the picture mode pair. 2.2.x read "fps_limit=0 and no perf_mode" as the Quality (30) default; this build reads it as the 60 mode unless perf_mode says so. The
// file keeps its meaning by getting the explicit pair; an unreadable value is written as what it reads as now.
std::string PictureModePair(const std::string& text, const Config& cfg, const controls::ControlApi& api, std::vector<std::string>& notes, unsigned& changed) {
    auto lines = SplitLines(text); const std::string end = EndingOf(lines);
    const std::string fps = Value(lines, "fps_limit"), perf = Value(lines, "perf_mode");
    const bool fpsKnown = fps == "0" || fps == "30" || fps == "steady30", perfKnown = perf == "0" || perf == "1";
    if (Has(lines, "perf_mode") && perfKnown && fpsKnown) return text;
    const auto want = Canonical(cfg, api); if (!want.count("fps_limit") || !want.count("perf_mode")) return text;
    unsigned n = 0;
    n += SetKey(lines, "fps_limit", want.at("fps_limit"), end);
    n += SetKey(lines, "perf_mode", want.at("perf_mode"), end);
    if (n) { notes.push_back("picture mode " + std::string(Mode30(cfg) ? "Quality (30)" : "Performance (60)") + " written as fps_limit=" + want.at("fps_limit") + " perf_mode=" + want.at("perf_mode")); changed += n; }
    return Join(lines);
}
// 3.0, step 2: a 1.2 beta's motion_gain (a percentage) gets the motion_level it reads as; the motion_gain line stays (motion_level wins when both are there).
std::string MotionLevel(const std::string& text, const Config& cfg, const controls::ControlApi& api, std::vector<std::string>& notes, unsigned& changed) {
    auto lines = SplitLines(text);
    if (!Has(lines, "motion_gain") || Has(lines, "motion_level")) return text;
    const auto want = Canonical(cfg, api); if (!want.count("motion_level")) return text;
    if (SetKey(lines, "motion_level", want.at("motion_level"), EndingOf(lines))) { notes.push_back("motion_gain kept, motion_level=" + want.at("motion_level") + " added"); ++changed; }
    return Join(lines);
}
// Add the next release's steps here (since = the first release that needs them); keep each one idempotent and Config-preserving.
const GuardStep kSteps[] = {
    {"3.0.0", "picture mode pair", PictureModePair},
    {"3.0.0", "motion level", MotionLevel},
};

std::string Stamp(const std::string& text, const char* version, const std::string& ending) {
    std::string out = std::string(kStampPrefix) + version + ending;
    for (const auto& l : SplitLines(text)) if (!IsStamp(l)) out += l;
    return out;
}
std::string Say(const char* what) { return std::string("settings guard: ") + what; }
}  // namespace

std::string GuardStampLine(const char* version) { return std::string(kStampPrefix) + (version ? version : ""); }
std::string GuardBackupPath(const char* path, const char* version) { return WithSuffix(path, "before-" + MajorMinor(version)); }
std::string GuardOldBackupPath(const char* path, const char* version, int n) { return WithSuffix(path, "before-" + MajorMinor(version) + "-bak" + (n > 1 ? "-" + std::to_string(n) : std::string())); }

GuardReport RunSettingsGuard(const char* path, const char* version, const controls::ControlApi& api, const FileOps& f) {
    return RunSettingsGuardSteps(path, version, api, f, kSteps, sizeof kSteps / sizeof kSteps[0]);
}

GuardReport RunSettingsGuardSteps(const char* path, const char* version, const controls::ControlApi& api, const FileOps& f, const GuardStep* steps, size_t stepCount) {
    GuardReport r;
    const Ver mine = ParseVer(version);
    if (!mine.valid) { r.outcome = GuardOutcome::BadVersion; r.line = Say("this build's version is not a version; settings file not touched"); return r; }
    FileInfo info{}; int st = f.stat(f.context, path, info);
    // No settings file but a .bak (a cut between the store's two renames): the loader would read that .bak, so it is migrated like the file ("from the .bak"): the same backup-first rules,
    // the .bak itself is the source and stays where it is.
    const std::string bakName = std::string(path) + ".bak";
    const std::string tmpName = std::string(path) + ".tmp";
    bool fromBak = false, tmpExists = false, fromTmp = false; std::string src = path;
    if (st == -ENOENT) {
        st = f.stat(f.context, bakName.c_str(), info);
        if (st == -ENOENT) return r;
        fromBak = true; src = bakName;
        // A cut save may have left path.tmp, complete and newer than the .bak: when it parses fully it is the migration source. Either way it is set aside (below) before the guard
        // writes its own temporary under that name, so it is never truncated.
        FileInfo ti{}; std::string tmpRaw; Document tmpDoc;
        if (f.stat(f.context, tmpName.c_str(), ti) != -ENOENT) {
            tmpExists = true;
            if (f.stat(f.context, tmpName.c_str(), ti) >= 0 && ti.regular && ti.size > 0 && ti.size <= kDocumentLimit && ReadAll(f, tmpName.c_str(), tmpRaw, kDocumentLimit) &&
                ParseDocument(tmpRaw, api, tmpDoc).status == StoreStatus::Ok) { src = tmpName; info = ti; fromTmp = true; }
        }
    }
    if (st < 0 || !info.regular) { r.outcome = GuardOutcome::IoError; r.line = Say("settings file could not be examined; left as it is"); return r; }
    const std::string unreadable = WithSuffix(path, "unreadable-" + MajorMinor(version));
    // A byte copy of a file this build will not touch, once, under a name that is free (never over anything).
    auto keepCopy = [&](const std::string& bytes) { if (NameState(f, unreadable.c_str()) == Name::Free && WriteNewFile(f, unreadable, bytes)) { r.backupPath = unreadable; r.backupMade = true; } };
    std::string raw;
    if (info.size > kDocumentLimit) {
        r.outcome = GuardOutcome::Unreadable;
        if (info.size <= kCopyLimit && ReadAll(f, src.c_str(), raw, kCopyLimit)) keepCopy(raw);
        r.line = Say((std::string("settings file is too large to read; left as it is") + (r.backupMade ? ", a copy was kept" : "")).c_str());
        return r;
    }
    if (!ReadAll(f, src.c_str(), raw, kDocumentLimit)) { r.outcome = GuardOutcome::IoError; r.line = Say("settings file could not be read; left as it is"); return r; }

    const Ver stamp = ReadStamp(raw);
    // A file that does not parse is never rewritten, stamped or not.
    Document old; const auto parsed = ParseDocument(raw, api, old);
    if (parsed.status == StoreStatus::UnsupportedVersion) { r.outcome = GuardOutcome::NewerFormat; r.line = Say("settings file is from a newer format; left alone"); return r; }
    if (parsed.status != StoreStatus::Ok) {
        r.outcome = GuardOutcome::Unreadable;
        if (!raw.empty()) keepCopy(raw);   // (an empty file has nothing to keep)
        r.line = Say(("settings file " + std::string(raw.empty() ? "is empty" : "cannot be read (" + parsed.message + ")") + "; left as it is (the loader uses the .bak copy or the defaults)" + (r.backupMade ? ", a copy was kept" : raw.empty() ? ", no copy kept (nothing in it)" : "")).c_str());
        return r;
    }

    // Steps owed: since > the stamped version (every step when there is no stamp), by the full version compare, and not past this build.
    std::vector<const GuardStep*> owed;
    for (size_t i = 0; i < stepCount; ++i) {
        const Ver since = ParseVer(steps[i].since);
        if (!since.valid || Cmp(since, mine) > 0) continue;
        if (!stamp.valid || Cmp(since, stamp) > 0) owed.push_back(&steps[i]);
    }
    const bool crossing = !stamp.valid || Cmp(stamp, mine, 2) < 0;   // the file comes from an older release line: this is the first boot of this release on it
    if (owed.empty() && !crossing && !fromTmp) { r.outcome = GuardOutcome::Current; return r; }   // (a patch release without a step does not rewrite the file again; but a source that is the unfinished save .tmp is never "Current": the loader ignores the .tmp, so the settings file is missing and must be written from it)

    std::string text = raw; unsigned changed = 0; std::vector<std::string> notes;
    for (const GuardStep* s : owed) text = s->run(text, old.config, api, notes, changed);
    if (!(fromTmp && owed.empty() && !crossing)) text = Stamp(text, version, EndingOf(SplitLines(raw)));   // (a current .tmp is already what the store would have committed: kept byte for byte)
    if (text.size() > kDocumentLimit) { r.outcome = GuardOutcome::Rejected; r.line = Say("settings file would become too large; left as it is"); return r; }
    Document check; const auto again = ParseDocument(text, api, check);
    if (again.status != StoreStatus::Ok || !Equal(check.config, old.config)) { r.outcome = GuardOutcome::Rejected; r.line = Say("migrated text would read differently; left as it is"); return r; }

    if (crossing) {   // (the copy; the player's .bak step below also runs for a .tmp source)
        // The one-time copy of the file as the older release left it.
        r.backupPath = GuardBackupPath(path, version);
        switch (NameState(f, r.backupPath.c_str())) {
            case Name::Good: r.backupExisted = true; break;
            case Name::Free: if (WriteNewFile(f, r.backupPath, raw)) { r.backupMade = true; break; } [[fallthrough]];
            default: r.outcome = GuardOutcome::BackupFailed; r.backupPath.clear(); r.line = Say("backup copy could not be written (or its name is taken by something that is not a copy); settings file left as it is"); return r;
        }
    }
    std::string lateBakNote;   // a .tmp source moves the .bak only after the write succeeded: until then the .bak is the loader's fallback and must stay
    auto moveOldBak = [&]() -> bool {
      if (crossing || fromTmp) {
        // The player's own .bak is not removed by this rewrite: it is moved to a second before-release name (-bak, then -bak-2 ... -bak-9), the first one that is free. If the .bak is
        // byte for byte a copy that already exists (the before-release copy or one of those names) nothing is moved and the ordinary rotation goes on. With no settings file the
        // .bak is the source being migrated and stays where it is, unless the source is the .tmp: then the .bak is the last committed file and is kept like any other.
        FileInfo bi{}; const int bst = (fromBak && !fromTmp) ? -ENOENT : f.stat(f.context, bakName.c_str(), bi);
        if (bst != -ENOENT) {
            std::string bakBytes; const bool known = bst >= 0 && ReadAll(f, bakName.c_str(), bakBytes, kCopyLimit);
            std::vector<std::string> names; names.push_back(GuardBackupPath(path, version)); for (int n = 1; n <= kMaxOldBackups; ++n) names.push_back(GuardOldBackupPath(path, version, n));
            bool identical = false;   // (the before-release copy is compared for a same-release .tmp source too: an equal .bak is not copied again)
            if (known) for (const auto& nm : names) { std::string other; if (!nm.empty() && NameState(f, nm.c_str()) == Name::Good && ReadAll(f, nm.c_str(), other, kCopyLimit) && other == bakBytes) { identical = true; break; } }
            if (!identical) {
                std::string target;
                if (bst >= 0) for (size_t i = 1; i < names.size() && target.empty(); ++i) if (NameState(f, names[i].c_str()) == Name::Free) target = names[i];
                if (target.empty() || f.rename(f.context, bakName.c_str(), target.c_str()) < 0) {
                    if (fromTmp) { lateBakNote = "; your earlier .bak could not be moved aside (" + std::string(target.empty() ? "no free before-release name" : "the move to " + target + " failed") + ") and stays as it is"; return false; }
                    r.outcome = GuardOutcome::OldBackupBlocked;
                    r.line = Say(("your earlier backup (" + bakName + ") could not be moved aside (" + (target.empty() ? "no free before-release name" : "the move to " + target + " failed") + "); it and the settings file are left as they are").c_str());
                    return false;
                }
                r.oldBackupMoved = true; r.oldBackupPath = target;
            }
        }
      }
      return true;
    };
    if (!fromTmp && !moveOldBak()) return r;

    // A cut after this set-aside but before the settings file is written leaves the .bak in place (a .tmp source never moves it before the write) and the newer values in the -tmp copy.
    // A current .tmp that owes nothing is the very text to be committed: it is renamed into place (no copy, no window).
    const bool direct = fromTmp && owed.empty() && !crossing && text == raw;
    if (fromBak && tmpExists && !direct) {   // never delete, never truncate: a leftover temporary goes to the first free before-release name
        std::string target;
        for (int n = 1; n <= kMaxOldBackups && target.empty(); ++n) { const std::string nm = WithSuffix(path, "before-" + MajorMinor(version) + "-tmp" + (n > 1 ? "-" + std::to_string(n) : std::string())); if (NameState(f, nm.c_str()) == Name::Free) target = nm; }
        if (target.empty() || f.rename(f.context, tmpName.c_str(), target.c_str()) < 0) {
            r.outcome = GuardOutcome::OldBackupBlocked;
            if (r.oldBackupMoved && f.stat(f.context, bakName.c_str(), info) == -ENOENT && f.rename(f.context, r.oldBackupPath.c_str(), bakName.c_str()) >= 0) { r.oldBackupMoved = false; r.oldBackupPath.clear(); }   // the loader still has its .bak
            r.line = Say(("a leftover " + tmpName + " could not be set aside (" + (target.empty() ? "no free name" : "the move to " + target + " failed") + "); the settings file is not written").c_str());
            return r;
        }
        r.tmpSetAside = target;
    }

    Document saved; StoreResult put{StoreStatus::Ok, ""};
    if (direct) { put.status = f.rename(f.context, tmpName.c_str(), path) >= 0 ? StoreStatus::Ok : StoreStatus::IoError; if (put.status != StoreStatus::Ok) put.message = "the move of " + tmpName + " into place failed"; }
    else put = ReplaceDocumentText(path, text, check.config, api, f, saved);
    const std::string from = std::string(fromTmp ? "its unfinished save " + tmpName + " (the settings file was missing), " : fromBak ? "its .bak copy (the settings file was missing), " : "") + (stamp.valid ? "an older build" : "2.2.1 or earlier (no stamp)");
    if (put.status != StoreStatus::Ok) {
        r.outcome = GuardOutcome::WriteFailed;
        // Put the player's .bak back under its name when nothing took that place meanwhile.
        if (r.oldBackupMoved && f.stat(f.context, bakName.c_str(), info) == -ENOENT && f.rename(f.context, r.oldBackupPath.c_str(), bakName.c_str()) >= 0) { r.oldBackupMoved = false; r.oldBackupPath.clear(); }
        // The set-aside copy goes back under the .tmp name when that name is free; otherwise the log names where the newer values are.
        std::string asideNote;
        if (!r.tmpSetAside.empty()) {
            if (f.stat(f.context, tmpName.c_str(), info) == -ENOENT && f.rename(f.context, r.tmpSetAside.c_str(), tmpName.c_str()) >= 0) { r.tmpSetAside.clear(); }
            else asideNote = "; your unfinished save is kept as " + r.tmpSetAside;
        }
        r.line = Say(("settings file from " + from + " could not be rewritten (" + put.message + "); left as it is" + asideNote +
                      (r.oldBackupMoved ? "; your earlier .bak is now at " + r.oldBackupPath + " (it could not be put back)" : std::string())).c_str()); return r;
    }
    if (fromTmp) moveOldBak();   // after the write: a failure only leaves the .bak where it is (logged below); the outcome stays Migrated
    r.outcome = GuardOutcome::Migrated; r.changedLines = changed;
    std::string what; for (const auto& n : notes) what += (what.empty() ? "" : "; ") + n;
    const std::string bakNote = !crossing ? std::string("same release, no new backup copy") + (r.oldBackupMoved ? "; your earlier .bak was moved to " + r.oldBackupPath : std::string()) + (r.tmpSetAside.empty() ? std::string() : "; a leftover " + tmpName + " was kept as " + r.tmpSetAside) + lateBakNote : std::string("backup ") + (r.backupExisted ? "was already there (kept)" : "made") + " (" + r.backupPath + ")" +
                                (r.oldBackupMoved ? "; your earlier .bak was moved to " + r.oldBackupPath : std::string()) + (r.tmpSetAside.empty() ? std::string() : "; a leftover " + tmpName + " was kept as " + r.tmpSetAside) + lateBakNote;
    r.line = Say(("settings file from " + from + " stamped " + version + "; " + bakNote + "; " +
                  (changed ? std::to_string(changed) + " line(s) written: " + what : std::string("no line changed")) + "; new settings start at their defaults, nothing you set was lost").c_str());
    return r;
}
}
