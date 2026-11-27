// harvprep31: the shader harvest's menu tour (decomp/include/Game/MP/MpHarvestTour.hpp, driven by MpDemo.cpp on a GXV_SEEDWALK=1 SMG_MP_DEMO=menus run).
// Every screen it shows is shown without its flow and must make no call that reaches a service, the network, the keep-safe copies, a save or the settings
// file, open no window and no keyboard; the Settings walk changes no value and writes nothing; the screens the tour leaves out do make such calls (so the
// check below can see them). The leg runs without network anyway (run_all.sh: a namespace with the loopback only): this is the second wall.
#include "common.hpp"
#include "Game/MP/MpHarvestTour.hpp"

namespace {
void Fresh() { fresh(); fake::cfgReset(true, true, true); }   // (as settings_test.cpp: a native Settings host)
std::string FileText() {
    auto it = fake::cfg.files.data.find(fake::kCfgPath);
    return it == fake::cfg.files.data.end() ? std::string() : it->second;
}
struct Calls {
    int ensure, friendReq, registers, starts, leaves, kicks, acctDel, rejoins, flowSaves, writeBacks, backups, ksMade, ks, ops, ctStarts, ctPumps, ctReports,
        dgBegins, dgChecks, kb, settingsSent, wishes, recoveryDiscards, cfgWrites, gatherBegins;
    bool operator==(const Calls& o) const { return memcmp(this, &o, sizeof *this) == 0; }
};
Calls Snap() {
    return Calls{fake::ensureCalls, fake::friendRequests, fake::registers, fake::starts, fake::leaves, fake::kicks, fake::acctDeletes, fake::rejoins, fake::flowSaves,
                 fake::writeBacks, fake::backups, fake::ks.madeCount, static_cast<int>(fake::ks.calls.size()), static_cast<int>(fake::ops.size()), fake::ct.starts,
                 fake::ct.pumps, fake::ct.reports, fake::dgBegins, fake::dgCheckBegins, fake::kbOpens, fake::settingsSent, fake::wishes, fake::recoveryDiscards,
                 static_cast<int>(fake::cfg.files.writes), mp_gather_fake_begins()};
}
std::string Diff(const Calls& a, const Calls& b) {
    static const char* names[] = {"ensure", "friendReq", "registers", "starts", "leaves", "kicks", "acctDel", "rejoins", "flowSaves", "writeBacks", "backups", "ksMade", "ks",
                                  "ops", "ctStarts", "ctPumps", "ctReports", "dgBegins", "dgChecks", "kb", "settingsSent", "wishes", "recoveryDiscards", "cfgWrites",
                                  "gatherBegins"};
    const int* x = reinterpret_cast<const int*>(&a);
    const int* y = reinterpret_cast<const int*>(&b);
    std::string out;
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i)
        if (x[i] != y[i]) out += std::string(names[i]) + " " + std::to_string(x[i]) + "->" + std::to_string(y[i]) + " ";
    return out;
}
// one screen shown without its flow for n frames over the Multiplayer menu: the calls it made (and whether a window or the keyboard came)
std::string ShowAlone(Scr s, int n, bool* window) {
    fresh();
    S().openFromEntry();
    frames(3);
    const Calls c0 = Snap();
    const int dlg = io.opened;
    S().demoShow(s);
    MpPanel::Model m;
    for (int i = 0; i < n; ++i) { step(); S().buildModel(&m); }
    *window = io.opened != dlg || S().dialogActive() || S().keyboardActive();
    return Diff(c0, Snap());
}
}  // namespace

void testHarvestTour() {
    using namespace MpHarvestTour;
    // 1. every Multiplayer screen of the tour, alone, for 600 frames: no call, no window
    for (Scr s : kMpScreens) {
        bool window = false;
        const std::string d = ShowAlone(s, 600, &window);
        CHECK(d.empty());
        CHECK(!window);
        if (!d.empty() || window) fprintf(stderr, "  harvest tour: screen %s: calls [%s] window %d\n", MpMenu::screenName(s), d.c_str(), window ? 1 : 0);
    }
    // 2. the screens it leaves out for a service: the check sees their calls (so an empty diff above means something)
    {
        bool window = false;
        CHECK(!ShowAlone(Scr::S_FRIENDS, 120, &window).empty() || window);
        CHECK(!ShowAlone(Scr::S_BOARD, 120, &window).empty() || window);
    }
    // 3. the whole Multiplayer walk as MpDemo runs it (60 frames a screen): every screen comes and goes, the menu is back on its first screen, no call
    {
        fresh();
        S().openFromEntry();
        frames(3);
        const Scr first = S().screen();
        const int depth = S().depth();
        const Calls c0 = Snap();
        Walk w;
        bool shown[MpMenu::S_COUNT] = {};
        bool done = false;
        int64_t t = 0;
        for (int i = 0; i < 20000 && !done; ++i, t += 16) {
            MpMenu::Input in;
            in.ok = in.back = in.left = in.right = true;   // (the walk clears every press: nothing of this reaches the menu)
            done = w.step(S(), &in, kMpScreens, t, 1000, shown);
            CHECK(!in.ok && !in.back && !in.left && !in.right && !in.up && !in.down);
            step(in);
        }
        CHECK(done);
        CHECK(S().isOpen() && S().screen() == first && S().depth() == depth);
        CHECK(Snap() == c0);
        CHECK(w.reached + w.missed == sizeof kMpScreens / sizeof *kMpScreens);
        unsigned n = 0;
        for (Scr s : kMpScreens) n += shown[s] ? 1u : 0u;
        CHECK(n == w.reached);
        CHECK(w.reached >= 10);   // (the session screens with no session pop themselves at once: they are counted as missed, not as an error)
        fprintf(stderr, "  harvest tour: multiplayer screens shown %u of %u (popped by themselves: %u)\n", w.reached, static_cast<unsigned>(sizeof kMpScreens / sizeof *kMpScreens), w.missed);
    }
    // 4. the Settings walk: opened from the entry screen's way, every page shown once, Back on the main page closes them; no value changed, nothing written
    {
        Fresh();
        const std::string before = FileText();
        CHECK(S().openSettings(MpMenu::Sequence::SO_ENTRY));
        frames(2);
        CHECK(S().screen() == Scr::S_SET_MAIN);
        const Calls c0 = Snap();
        Walk w;
        bool shown[MpMenu::S_COUNT] = {};
        bool done = false;
        int64_t t = 0;
        for (int i = 0; i < 20000 && !done; ++i, t += 16) {
            MpMenu::Input in;
            done = w.step(S(), &in, kSetScreens, t, 1000, shown);
            step(in);
        }
        CHECK(done && w.reached == sizeof kSetScreens / sizeof *kSetScreens && w.missed == 0);
        for (Scr s : kSetScreens) CHECK(shown[s]);
        CHECK(S().screen() == Scr::S_SET_MAIN && S().settingsOpen());
        back();   // (MpDemo: the menu's own Back on the main page)
        frames(3);
        CHECK(!S().isOpen() && !S().settingsOpen());
        CHECK(logHas("settings: closed") && !logHas("settings: saved") && !logHas("settings: saved (on closing)") && !logHas("settings: leave asked"));
        CHECK(Snap() == c0 && fake::cfg.files.writes == 0 && FileText() == before);
    }
}
