// modes26 (12 Nov): the two picture modes in the settings file. Old files migrate (never an error, never a lost line), the file format keeps every old key, and a save
// writes the pair (fps_limit, perf_mode) an older build reads as the same mode. Build: tools/test_player_settings.sh modes
#include "port/player_settings.h"
#include "player_settings_files.h"
#include <cassert>
#include <iostream>
#include <string>
using namespace port;
namespace {
const controls::ControlApi& Api() { return controls::GetControlApi(); }
// A complete file as the build writes it, then edited line by line like a file from an older build.
std::string Fresh(const settings::Config& c) {
    std::string out;
    settings::Document blank{settings::DefaultConfig(Api()), "schema=1\n", true};
    assert(settings::SerializeDocument(blank, c, Api(), out).status == settings::StoreStatus::Ok);
    return out;
}
std::string Set(std::string text, const std::string& key, const std::string& value) {   // value empty: the line is removed
    const auto at = text.find("\n" + key + "=");
    const size_t from = at == std::string::npos ? std::string::npos : at + 1;
    if (from == std::string::npos) { if (!value.empty()) text.insert(text.find("schema="), key + "=" + value + "\n"); return text; }
    const auto end = text.find('\n', from) + 1;
    text.replace(from, end - from, value.empty() ? "" : key + "=" + value + "\n");
    return text;
}
settings::Config Read(const std::string& text, settings::StoreStatus want = settings::StoreStatus::Ok) {
    settings::Document d;
    const auto r = settings::ParseDocument(text, Api(), d);
    if (r.status != want) std::cerr << "status " << int(r.status) << " " << r.message << "\n";
    assert(r.status == want);
    return d.config;
}
}  // namespace
int main() {
    const auto def = settings::DefaultConfig(Api());
    // The default is Performance (60).
    assert(!settings::Mode30(def) && def.performance60 && def.fpsLimit == 0);
    const std::string text = Fresh(def);
    assert(text.find("fps_limit=0\n") != std::string::npos && text.find("perf_mode=1\n") != std::string::npos);
    // ---- migration table: (fps_limit, perf_mode) -> mode. perf_mode "-" = the key is absent (a file from before the Performance row).
    struct Case { const char* fps; const char* perf; bool mode30; const char* what; };
    const Case cases[] = {
        {"0", "1", false, "old Performance / UP TO 60 -> Performance"},
        {"0", "0", true, "old Quality (the old default) -> Quality"},
        {"0", "-", true, "a file with no perf_mode and fps_limit 0 (1.1 / early 1.2 Quality) -> Quality"},
        {"30", "0", true, "old Locked 30 -> Quality"},
        {"30", "-", true, "old Locked 30 without perf_mode -> Quality"},
        {"30", "1", true, "a lock and Performance together: the lock wins, as every earlier build reads it"},
        {"steady30", "-", true, "1.1 Steady 30 -> Quality"},
        {"steady30", "0", true, "1.1 Steady 30 -> Quality"},
        {"60", "-", false, "unknown fps_limit -> Performance"},
        {"60", "0", false, "unknown fps_limit beats a perf_mode of 0 -> Performance"},
        {"banana", "1", false, "unknown fps_limit -> Performance"},
        {"", "-", false, "empty fps_limit -> Performance"},
        {"0", "2", false, "unknown perf_mode -> Performance"},
        {"0", "yes", false, "unknown perf_mode -> Performance"},
        {"0", "", false, "empty perf_mode -> Performance"},
        {"30", "yes", false, "unknown perf_mode -> Performance, even beside a lock (never silently stuck at 30 on junk)"},
    };
    for (const auto& c : cases) {
        auto t = Set(Set(text, "fps_limit", c.fps[0] ? c.fps : "x"), "perf_mode", std::string(c.perf) == "-" ? "" : (c.perf[0] ? c.perf : "x"));
        if (!c.fps[0]) { const auto at = t.find("fps_limit=x"); t.replace(at, 11, "fps_limit="); }
        if (std::string(c.perf).empty()) { const auto at = t.find("perf_mode=x"); t.replace(at, 11, "perf_mode="); }
        const auto cfg = Read(t);
        if (settings::Mode30(cfg) != c.mode30) std::cerr << "case failed: " << c.what << "\n";
        assert(settings::Mode30(cfg) == c.mode30);
        // The in-memory state is canonical: 60 = (0, true), 30 = (30, false).
        assert(c.mode30 ? (cfg.fpsLimit == 30 && !cfg.performance60) : (cfg.fpsLimit == 0 && cfg.performance60));
    }
    // ---- a file is never lost or corrupted by the migration: reading it changes nothing on disk; saving rewrites only the managed lines, keeps comments and unknown keys,
    // and every hidden-row key keeps its value.
    {
        auto legacy = Set(Set(text, "perf_mode", ""), "fps_limit", "0");
        legacy = "# my notes\nfuture_key=7\n" + legacy;
        legacy = Set(legacy, "above_native", "0");
        legacy = Set(legacy, "mario_first", "0");
        legacy = Set(legacy, "anti_aliasing", "off");
        legacy = Set(legacy, "effects_quality", "full");
        legacy = Set(legacy, "auto_quality", "off");
        settings::Document d;
        assert(settings::ParseDocument(legacy, Api(), d).status == settings::StoreStatus::Ok);
        assert(settings::Mode30(d.config) && d.source == legacy);   // Quality; the source text is untouched
        assert(!d.config.aboveNative && !d.config.prioritizeMario && d.config.antiAliasing == settings::AntiAliasing::Off &&
               d.config.effects == settings::EffectsQuality::Full && d.config.autoQuality == settings::AutoQuality::Disabled);   // hidden rows still read
        std::string saved;
        assert(settings::SerializeDocument(d, d.config, Api(), saved).status == settings::StoreStatus::Ok);
        assert(saved.find("# my notes\nfuture_key=7\n") == 0);
        assert(saved.find("fps_limit=30\n") != std::string::npos && saved.find("perf_mode=0\n") != std::string::npos);
        assert(saved.find("above_native=0\n") != std::string::npos && saved.find("mario_first=0\n") != std::string::npos && saved.find("anti_aliasing=off\n") != std::string::npos &&
               saved.find("effects_quality=full\n") != std::string::npos && saved.find("auto_quality=off\n") != std::string::npos);
        // and it reads back as the same mode with the same hidden rows
        const auto again = Read(saved);
        assert(settings::Mode30(again) && !again.aboveNative && again.autoQuality == settings::AutoQuality::Disabled);
        assert(settings::Equal(again, d.config));
    }
    // ---- saving each mode writes a pair an older build reads the same way
    for (const bool q30 : {false, true}) {
        auto c = def;
        if (q30) { c.fpsLimit = 30; c.performance60 = false; }
        const auto t = Fresh(c);
        assert(t.find(q30 ? "fps_limit=30\n" : "fps_limit=0\n") != std::string::npos && t.find(q30 ? "perf_mode=0\n" : "perf_mode=1\n") != std::string::npos);
        assert(settings::Mode30(Read(t)) == q30);
    }
    // ---- a damaged line is still an error (a cut-short write is how the backup logic catches an interrupted save), only unknown VALUES are tolerated
    { settings::Document cut; assert(settings::ParseDocument("schema=1\nfps_limit", Api(), cut).status == settings::StoreStatus::Malformed); }
    for (size_t cut = 0; cut < text.size(); ++cut) { settings::Document d; assert(settings::ParseDocument(text.substr(0, cut), Api(), d).status != settings::StoreStatus::Ok); }
    // ---- an unknown value is rewritten to a known one only when the player saves
    {
        auto odd = Set(text, "fps_limit", "45");
        settings::Document d;
        assert(settings::ParseDocument(odd, Api(), d).status == settings::StoreStatus::Ok && d.source == odd && !settings::Mode30(d.config));
        std::string saved;
        assert(settings::SerializeDocument(d, d.config, Api(), saved).status == settings::StoreStatus::Ok && saved.find("fps_limit=0\n") != std::string::npos && saved.find("fps_limit=45") == std::string::npos);
    }
    // ---- revD: the whole path through the store (Load -> Save) for the files each earlier build wrote. Reading touches nothing; a save writes the pair 2.2.1 reads.
    {
        // What 2.2.1 (and 1.x) accept for the two keys: anything else was an error there.
        auto oldBuildReads = [](const std::string& t) {
            for (const char* k : {"fps_limit=", "perf_mode="}) {
                const auto at = t.find(std::string("\n") + k);
                assert(at != std::string::npos);
                const auto v = t.substr(at + 1 + std::string(k).size(), t.find('\n', at + 1) - at - 1 - std::string(k).size());
                assert(std::string(k) == "fps_limit=" ? (v == "0" || v == "30" || v == "steady30") : (v == "0" || v == "1"));
            }
        };
        struct File { const char* name; const char* fps; const char* perf; bool mode30; };
        const File files[] = {{"2.2 Quality", "0", "0", true}, {"2.2 Locked 30", "30", "0", true}, {"2.2 Performance", "0", "1", false}, {"1.1 Steady 30", "steady30", "-", true},
                              {"1.1 Quality", "0", "-", true}, {"garbage rate", "45", "1", false}, {"garbage perf_mode", "0", "maybe", false}};
        for (const auto& f : files) {
            auto t = Set(Set(text, "fps_limit", f.fps), "perf_mode", std::string(f.perf) == "-" ? "" : f.perf);
            t = "# keep me\n" + Set(t, "above_native", "0");
            UiFiles fs; fs.data["p.cfg"] = t; const auto before = fs.data; auto ops = fs.ops();
            settings::Document d;
            const auto r = settings::LoadDocument("p.cfg", Api(), ops, d);
            if (r.status != settings::StoreStatus::Ok) std::cerr << f.name << ": status " << int(r.status) << " " << r.message << "\n";
            assert(r.status == settings::StoreStatus::Ok && settings::Mode30(d.config) == f.mode30);
            assert(fs.writes == 0 && fs.data == before);                       // migration is read-only
            // a read-only card: the save fails cleanly and the card is exactly as it was
            fs.failWrite = true;
            settings::Document dummy;
            assert(settings::SaveDocument("p.cfg", d, d.config, Api(), ops, dummy).status == settings::StoreStatus::IoError && fs.data == before);
            fs.failWrite = false;
            // the player saves (here without touching the mode): the pair is one an older build reads, the hidden row and the comment survive, and nothing else is left on the card
            settings::Document saved;
            assert(settings::SaveDocument("p.cfg", d, d.config, Api(), ops, saved).status == settings::StoreStatus::Ok);
            const auto& now = fs.data.at("p.cfg");
            oldBuildReads(now);
            assert(now.find("# keep me\n") == 0 && now.find("above_native=0\n") != std::string::npos);
            assert(settings::Mode30(Read(now)) == f.mode30);
            assert(fs.data.size() == 2 && fs.data.count("p.cfg.bak") && fs.data.at("p.cfg.bak") == before.at("p.cfg"));   // the old file is the backup, byte for byte
            // switching the mode in Settings and saving again keeps every hidden row
            auto c = d.config;
            c.fpsLimit = f.mode30 ? 0 : 30; c.performance60 = f.mode30;
            assert(settings::SaveDocument("p.cfg", saved, c, Api(), ops, saved).status == settings::StoreStatus::Ok);
            oldBuildReads(fs.data.at("p.cfg"));
            assert(settings::Mode30(Read(fs.data.at("p.cfg"))) == !f.mode30 && !Read(fs.data.at("p.cfg")).aboveNative);
        }
        // a cut-short primary falls back to the backup and is not rewritten by the load
        UiFiles fs; const auto good = Set(Set(text, "perf_mode", ""), "fps_limit", "0");
        fs.data["p.cfg"] = good.substr(0, good.size() / 2); fs.data["p.cfg.bak"] = good; const auto before = fs.data; auto ops = fs.ops();
        settings::Document d;
        assert(settings::LoadDocument("p.cfg", Api(), ops, d).status == settings::StoreStatus::Recovered && settings::Mode30(d.config) && fs.data == before && fs.writes == 0);
        // no file at all: the default (Performance), nothing created
        UiFiles none; auto nops = none.ops(); settings::Document nd;
        assert(settings::LoadDocument("p.cfg", Api(), nops, nd).status == settings::StoreStatus::Missing && !settings::Mode30(nd.config) && none.data.empty() && none.writes == 0);
        // the order of the lines in a hand-edited file does not change the reading
        const auto a = Set(Set(text, "fps_limit", ""), "perf_mode", "");
        for (const bool first : {false, true}) {
            auto t = a; const std::string two = first ? "fps_limit=30\nperf_mode=yes\n" : "perf_mode=yes\nfps_limit=30\n";
            t.insert(t.find("schema="), two);
            assert(!settings::Mode30(Read(t)));
        }
    }
    std::cout << "modes: default Performance, " << sizeof cases / sizeof cases[0] << " migration cases, hidden keys kept, pair written for older builds passed\n";
    return 0;
}
