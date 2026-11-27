// Galaxy 3.0 multiplayer: the menus, the character select (the 3D heads: MpHead3D draws them) (flow split of MpMenuSequence.cpp, 11 Nov).
// Owner of this family's screens: see MpMenuSequence.cpp "the file map". Pure code, as MpMenuSequence.cpp.
#include "MpMenuInternal.hpp"

extern "C" bool mp_update_take_notice(void) __attribute__((weak));   // release3: port/mp_update.h (platform side: not in the game's include path)

namespace MpMenu {

void Sequence::openChars(int fromLobby) {
    // the cursor starts on the character this console has (lobby) or wished last
    mCharWaiting = false;
    push(S_CHARS, static_cast<uint8_t>(fromLobby));
    const int have = mp_active() ? static_cast<int>(mp_slot_look(mp_local_slot())) : mLook;
    top().cursor = static_cast<int8_t>(have >= 0 && have < MP_LOOK_COUNT ? have : 0);
}

int Sequence::charOwner(int look) const {
    if (!mp_active()) return -1;   // (before a room exists nothing is taken; the host decides at join)
    for (int k = 0; k < MP_MAX_PLAYERS; k++)
        if (mp_slot_present(k) && static_cast<int>(mp_slot_look(k)) == look) return k;
    return -1;
}

void Sequence::pickChar(int look) {
    const int owner = charOwner(look);
    const int local = mp_local_slot();
    if (owner >= 0 && owner != local) {
        if (mIo) mIo->sound(Se::Buzzer);
        mCharTakenAt = mFrame;
        mp_log("ui: character %d already taken by slot %d", look, owner);
        return;
    }
    if (mIo) mIo->sound(Se::Decide);
    mLook = look;
    // (13 Nov, default name) no name is passed: the name in mp.cfg stays (the character's own name until the player types one, and it
    // follows this choice); the online account's name must not be taken for a typed one.
    mp_ui_set_profile("", static_cast<MpLook>(look));
    invalidateGatherCache();
    mp_log("ui: character wish %d", look);
    {   // flow: shown once (the first play); from now on the last character is taken at once
        MpFlowStore fs = FlowStore();
        if (!fs.charPicked) {
            fs.charPicked = 1;
            SaveFlowStore(fs);
        }
    }
    if (top().param == 0) {   // (the first play: Join / Host next; Back from there is the Multiplayer menu)
        pop();
        afterFirstChars();   // flow (one door): the gathering screen (or Join with a Code's keyboard)
        return;
    }
    if (owner == local) {   // (already ours)
        pop();
        return;
    }
    mCharWaiting = true;   // the host decides; the roster tells
    mCharAskedAt = mFrame;
}

void Sequence::tickChars() {
    if (!mCharWaiting || mDepth == 0 || top().scr != S_CHARS || top().param == 0) return;
    if (mp_active() && static_cast<int>(mp_slot_look(mp_local_slot())) == mLook) {
        mCharWaiting = false;
        if (mIo) mIo->sound(Se::Success);
        mp_log("ui: character %d granted", mLook);
        invalidateGatherCache();
        pop();
        return;
    }
    if (mFrame - mCharAskedAt > kCharWait) {
        mCharWaiting = false;
        mCharTakenAt = mFrame;
        if (mIo) mIo->sound(Se::Buzzer);
        mp_log("ui: character %d refused", mLook);
    }
}

bool Sequence::itemsChars(Scr s, uint8_t param, int* out, int* np) const {
    int& n = *np;
    auto add = [&](int i) { if (n < kItems) out[n++] = i; };
    switch (s) {
    case S_CHARS:
        for (int i = 0; i < MP_LOOK_COUNT; i++) add(I_CHAR0 + i);
        break;
    default: return false;
    }
    (void)param, (void)add;
    return true;
}

bool Sequence::chooseChars(int item) {
    Io* io = mIo;
    auto decide = [io] { if (io) io->sound(Se::Decide); };
    (void)decide;
    if (item >= I_CHAR0 && item < I_CHAR0 + static_cast<int>(MP_LOOK_COUNT)) {
        pickChar(item - I_CHAR0);
        return true;
    }
    return false;
}

bool Sequence::dialogChars(int purpose, bool yes, int param) {
    (void)purpose;
    (void)yes;
    (void)param;
    return false;
}

bool Sequence::keyboardChars(int purpose, const char* text) {
    (void)purpose;
    (void)text;
    return false;
}

bool Sequence::tickChars(Scr scr, const Input& in, bool* stop) {
    switch (scr) {
    case S_CHARS:
        tickChars();
        if (mDepth == 0 || top().scr != S_CHARS) { *stop = true; return true; }
        if (mCharWaiting) { *stop = true; return true; }   // (the host is asked: no buttons meanwhile)
        break;
    default: return false;
    }
    (void)in;
    return true;
}

bool Sequence::labelChars(int item, MpPanel::Row& r, const Frame& f) const {
    using namespace MpPanel;
    (void)f;
    if (item >= I_CHAR0 && item < I_CHAR0 + static_cast<int>(MP_LOOK_COUNT)) {
        const int look = item - I_CHAR0;
        Text(r.label, kLabel, static_cast<MpText>(MP_T_CHAR_MARIO + look));
        r.icon = static_cast<uint8_t>(ICON_MARIO + look);
        r.owner = static_cast<int8_t>(charOwner(look));
        if (r.owner >= 0) {
            txt::utf8To16(r.value, kValue, mp_slot_name(r.owner));
            if (r.owner != mp_local_slot()) r.marks |= MARK_DISABLED;
            else r.marks |= MARK_YOU;
        }
        return true;
    }
    return false;
}

bool Sequence::modelChars(MpPanel::Model* m, const Frame& f, int cursorItem, int count, const int* it) const {
    using namespace MpPanel;
    auto button = [m](int pic, MpText label) { ModelButton(m, pic, label); };
    auto hint = [m](MpText t) { Text(m->hint, 80, t); };
    auto label = [this, &f](int item, Row& r) { rowLabel(item, r, f); };
    (void)button, (void)hint, (void)label, (void)cursorItem, (void)count, (void)it;
    switch (f.scr) {
    case S_CHARS:
        {
        m->kind = KIND_CHARS;
        Text(m->title, 48, MP_T_TITLE_CHARS);
        // uxfix #9: "Already taken" stays for as long as the cursor is on a head another player has (it was gone after two seconds)
        const int cl = cursorItem - I_CHAR0;
        const int co = cl >= 0 && cl < static_cast<int>(MP_LOOK_COUNT) ? charOwner(cl) : -1;
        const bool onTaken = co >= 0 && co != mp_local_slot();
        const bool taken = onTaken || (mCharTakenAt != 0 && mFrame - mCharTakenAt < 120);
        hint(mCharWaiting ? MP_T_WAIT_CHAR : (taken ? MP_T_TAKEN : MP_T_DESC_CHARS));
        m->localSlot = static_cast<int8_t>(mp_active() ? mp_local_slot() : -1);
        for (int k = 0; k < MP_MAX_PLAYERS; k++)
            if (mp_active() && mp_slot_present(k)) m->cursors[k] = static_cast<int8_t>(mp_slot_look(k));
        if (mCharWaiting) m->spinner = (mFrame % 90) / 90.0f;
        button(txt::PIC_CROSS, MP_T_SELECT), button(txt::PIC_CIRCLE, MP_T_BACK);
        break;
    }
    default: return false;
    }
    return true;
}

// Input this family takes before the shared rules (stage 0: a direction, for grids; stage 1: Square / Triangle). True: taken.
bool Sequence::pressChars(Scr scr, const Input& in, int item, int count, int stage) {
    if (stage == 0 && scr == S_CHARS && (in.up || in.down || in.left || in.right)) {
        // a grid of two rows of three: left / right within the row, up / down between the rows
        const int c = top().cursor < 0 ? 0 : top().cursor;
        int n = c;
        if (in.left) n = (c / 3) * 3 + (c % 3 + 2) % 3;
        else if (in.right) n = (c / 3) * 3 + (c % 3 + 1) % 3;
        else n = (c + 3) % 6;
        if (n >= count) n = c;
        if (n != c) {
            top().cursor = static_cast<int8_t>(n);
            mPulse = 1.0f;
            if (mIo) mIo->sound(Se::Cursor);
        }
        return true;
    }
    (void)scr, (void)in, (void)item, (void)count, (void)stage;
    return false;
}

// Back (Circle) on one of this family's screens: phase 0 before anything (true: taken, nothing else happens), 1 just before the
// screen goes, 2 just after.
bool Sequence::leaveChars(Scr scr, uint8_t param, int phase) {
    if (phase == 1 && scr == S_CHARS) mCharWaiting = false;
    (void)scr, (void)param, (void)phase;
    return false;
}

}  // namespace MpMenu
