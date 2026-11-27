// menuback32: Back on the file select's heads returns to the entry screen. The pure parts: the entry state machine coming up "from the heads"
// (cursor on Single Player, nothing written, every pill and Back behaving as on the first visit) and the rule that says when the heads may offer
// Back. The FileSelector wiring (nerve, wipe, camera) is checked by tools/test_heads_back.sh (source) and in the emulator (see the report).
#include "common.hpp"

static void FreshSet() { fresh(); fake::cfgReset(true, true, true); }   // (a native Settings host: the third pill)

void testHeadsBack() {
    using namespace MpMenu;
    using MpEntry::Logic;
    // the rule: the entry screen exists (SMG_MP_MENU is not 0) and no Multiplayer window is up
    CHECK(MpEntry::backFromHeadsAllowed(true, false));
    CHECK(!MpEntry::backFromHeadsAllowed(false, false));   // SMG_MP_MENU=0: the heads have no Back, as in the original
    CHECK(!MpEntry::backFromHeadsAllowed(true, true));
    CHECK(!MpEntry::backFromHeadsAllowed(false, true));

    // Single Player -> heads -> Back: the entry again, the cursor on Single Player, nothing written, no menu opened
    {
        fresh(); EntryRig r; r.begin(); r.settle();
        r.ok(); r.settle();
        CHECK(r.has(MpEntry::CMD_SINGLE) && r.logic.state() == Logic::ST_DONE);
        const int saves = r.host.saves, opens = r.host.opens;
        EntryRig b; b.host.saved = 0; b.logic.begin(&b.host, false, true); b.settle();
        CHECK(b.logic.state() == Logic::ST_CHOOSE && b.logic.cursor() == MpEntry::CHOICE_SINGLE && b.has(MpEntry::CMD_APPEAR));
        CHECK(logHas("entry: screen shown cursor=single (back from the heads)"));
        CHECK(b.host.saves == 0 && b.host.opens == 0 && b.host.persists == 0 && saves == 0 && opens == 0 && !S().isOpen());
        // B on it is the title, as from any visit (the title screen's own behaviour is the same)
        b.back(); b.settle();
        CHECK(b.cmds.back() == MpEntry::CMD_TITLE && b.logic.state() == Logic::ST_OFF && b.host.saves == 0);
    }
    // ... even if Multiplayer is the remembered default: the pill the player just left is the cursor
    {
        fresh(); EntryRig r; r.host.saved = 1; r.logic.begin(&r.host, false, true); r.settle();
        CHECK(r.logic.cursor() == MpEntry::CHOICE_SINGLE);
        CHECK(r.host.saved == 1 && r.host.saves == 0);
    }
    // "after a session" still wins over nothing else: its cursor is Multiplayer; the plain visit is the remembered one (the title's behaviour unchanged)
    {
        fresh(); EntryRig a; a.begin(true); a.settle();
        CHECK(a.logic.cursor() == MpEntry::CHOICE_MULTI);
        EntryRig p; p.begin(); p.settle();
        CHECK(p.logic.cursor() == MpEntry::CHOICE_SINGLE);
        EntryRig q; q.host.saved = 1; q.begin(); q.settle();
        CHECK(q.logic.cursor() == MpEntry::CHOICE_MULTI);
    }
    // from there: Single Player again goes on to the heads; Multiplayer opens its menu and Back from it is the pills (the room is left by the menu's own path)
    {
        fresh(); EntryRig r; r.logic.begin(&r.host, false, true); r.settle();
        r.ok(); r.settle();
        CHECK(r.has(MpEntry::CMD_SINGLE) && r.logic.state() == Logic::ST_DONE && r.host.persists == 0);
    }
    {
        fresh(); EntryRig r; r.logic.begin(&r.host, false, true); r.settle();
        r.down(); CHECK(r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.ok(); r.settle(); frames(2);
        CHECK(S().isOpen() && S().screen() == S_MAIN && S().fromEntry() && r.logic.state() == Logic::ST_MENU);
        r.back(); r.settle();
        CHECK(!S().isOpen() && r.logic.state() == Logic::ST_CHOOSE && r.logic.cursor() == MpEntry::CHOICE_MULTI);
        r.back(); r.settle();
        CHECK(r.cmds.back() == MpEntry::CMD_TITLE);
    }
    // Settings from there, and Back from it: the pills, the cursor on Settings
    {
        FreshSet(); EntryRig r; r.logic.begin(&r.host, false, true); r.settle();
        r.down(2); CHECK(r.logic.cursor() == MpEntry::CHOICE_SETTINGS);
        r.ok(); r.settle(); frames(2);
        CHECK(r.host.settingsOpens == 1 && r.logic.state() == Logic::ST_MENU);
        r.back(); r.settle();
        CHECK(r.logic.state() == Logic::ST_CHOOSE && r.logic.cursor() == MpEntry::CHOICE_SETTINGS && r.host.saves == 0);
    }
    // the same visit twice (Single, Back, Single, Back): nothing accumulates
    {
        fresh(); EntryRig r; r.begin(); r.settle(); r.ok(); r.settle();
        for (int i = 0; i < 3; i++) {
            EntryRig b; b.logic.begin(&b.host, false, true); b.settle();
            CHECK(b.logic.cursor() == MpEntry::CHOICE_SINGLE && b.host.saves == 0 && b.host.opens == 0);
            b.ok(); b.settle();
            CHECK(b.has(MpEntry::CMD_SINGLE));
        }
    }
}
