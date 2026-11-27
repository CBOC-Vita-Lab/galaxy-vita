// menus4 item 4: the verifier's (chk-menus) scripted checks, as asserts: the gathering's big buttons never shrink under 0.9, Back out of the
// file questions leaves, a code that cannot work keeps the typed text, Play with Anyone failing lands on the gathering screen again, the
// Messages tile points on its first tap (the "You" plate has its own test: testMeMark).
#include "common.hpp"
#include "port/mp_gather.h"

namespace {
MpPanel::Model sModel;
// the scales of the screen's big buttons composed at the console's width and at 4:3
std::vector<float> ButtonScales(float width) {
    MpPanel::FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static MpPanel::Plan plan;
    static MpPanel::Scene scene;
    MpPanel::Screen scr;
    scr.width = width;
    S().buildModel(&sModel);
    MpPanel::beginScene(&scene);
    MpPanel::begin(&plan);
    MpPanel::compose(sModel, scr, fm, 0.25f, &scene, &plan);
    std::vector<float> v;
    for (int i = 0; i < scene.useCount; i++)
        if (scene.uses[i].kind == MpPanel::USE_BUTTON) v.push_back(scene.uses[i].w);
    return v;
}
void CheckScales(const char* what) {
    for (float width : {810.6667f, 608.0f})
        for (float w : ButtonScales(width)) {
            if (w < 0.9f) fprintf(stderr, "  button scale %.2f at %.0f wide: %s\n", w, width, what);
            CHECK(w >= 0.9f);
        }
}
}  // namespace

void testScriptedChecks() {
    for (int lang = 0; lang < MpMsgText::LANG_COUNT; lang++) {
        MpMsgText::setLanguage(lang);
        // T1: the gathering screen's big buttons, in the situations that put the most of them there
        fresh(); fake::codeAvailable = true; toGather(false); gatherHeads(1, 1); CheckScales("Together alone, code ok, svc");
        frames(20 * 60 + 5); CheckScales("+20 s nobody found");
        fresh(); fake::codeAvailable = true; toGather(false); gatherHeads(1, 1); mp_gather_fake_add_playing(77, "Ros", MP_MODE_TOGETHER, 2); frames(2); CheckScales("a game nearby");
        frames(20 * 60 + 5); CheckScales("a game nearby, +20 s");
        fresh(); toGather(true); gatherHeads(1, 1); mp_gather_fake_add_playing(77, "Ros", MP_MODE_RACE, 2); frames(2); CheckScales("Star Race, a game nearby");
        fresh(); fake::codeAvailable = true; toGather(true); gatherHeads(1, 1); frames(2); CheckScales("Star Race alone");
        fresh(); hostLobby(); CheckScales("as the leader");
        fresh(); guestLobby(); CheckScales("as a friend");
    }
    MpMsgText::setLanguage(0);
    // T8: Back out of "where to save" with the answer No leaves within a few presses (no loop)
    {
        fresh(); fake::coopFull = true; toSave(); down(6); ok(); ok(); ok();
        CHECK(S().screen() == Scr::S_SLOT);
        io.answer = MpMenu::DlgState::No;
        int n = 0;
        while (n++ < 4 && S().screen() == Scr::S_SLOT) back();
        CHECK(S().screen() != Scr::S_SLOT);
        io.answer = MpMenu::DlgState::Yes;
    }
    // T4: a code with no Wi-Fi opens no keyboard and says why; a code that does not decode brings the keyboard back with the text
    {
        fresh(); fake::wifi = false; openMain(); down(2); fake::kbOpens = 0; ok(); frames(2);
        CHECK(fake::kbOpens == 0 && S().screen() == Scr::S_MAIN);
        fresh(); openMain(); down(2); snprintf(fake::kbText, sizeof fake::kbText, "BADCODE"); fake::kbResult = 1; fake::kbOnce = true; fake::kbOpens = 0; ok(); frames(3);
        CHECK(fake::kbOpens >= 1 && std::string(fake::kbInitial) == "BADCODE");
    }
    // T5: Play with Anyone that did not work puts the player back on the gathering screen, never stranded on the Multiplayer menu
    for (int err : {MP_ERR_NO_WIFI, MP_ERR_SERVICE_DOWN, MP_ERR_RATE_LIMITED, MP_ERR_TIMEOUT}) {
        fresh(); toGather(); gatherHeads(1, 1); ok(); frames(2);
        io.answer = MpMenu::DlgState::No;
        fake::status.phase = MP_UI_FAILED; fake::status.error = static_cast<MpUiError>(err);
        frames(14);
        fake::status.phase = MP_UI_IDLE; fake::status.error = MP_ERR_NONE;
        frames(6);
        io.answer = MpMenu::DlgState::Yes;
        if (S().screen() != Scr::S_GATHER && S().screen() != Scr::S_MAIN) fprintf(stderr, "  Play with Anyone error %d: %s\n", err, MpMenu::screenName(S().screen()));
        CHECK(S().isOpen());
    }
    // T6: the Messages tile points on its first tap, sends on the second
    {
        fresh(); session(true, 0, MP_UI_PLAYING); frames(2); S().openSession(); frames(2); press(4);
        CHECK(S().screen() == Scr::S_MESSAGES);
        const bool a = S().firstTapPoints(4), b = S().firstTapPoints(4);
        CHECK(a && !b);
    }
}
