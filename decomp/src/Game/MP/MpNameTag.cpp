// Galaxy 3.0 multiplayer: name tags above the puppets (owner: puppet).
// The game's own GalaxyNamePlate layout (stationed in every scene), created WITHOUT the GalaxyNamePlateDrawer (its 16 entries
// have no bounds check and the Observatory uses many) and connected as an ordinary layout; positioned with setTrans, text in
// the slot colour. Setting nameTags: 0 off, 1 and 2 on.
// (ingame) "On" is now one behaviour (MpPresentLogic.hpp TagState): a tag shows for ~2 s when a player joins, comes back on screen
// after being off it, or changes character, and while their quick-message balloon is up, then fades; never while the game hides
// its HUD (cutscenes, star get, the fly-in). Off screen: no words, the player's head at the picture's edge pointing at them
// (edgeModel, drawn by the ui's panel), clear of the HUD's counters and the party strip. A tag with no visible puppet never draws.
// (perf) Setting 1: a tag is hidden while its puppet is off screen (the puppet's LOD cull) or beyond 6000 units (MpTagVis), and it
// fades in and out over 8 turns (the layout's global alpha) instead of popping; a faded-out plate is dead, so its layout is
// neither moved, calculated nor drawn.
#include "Game/MP/MpCharColor.hpp"   // MP3 touches: one colour per character
#include "Game/MP/MpCoop.hpp"   // MP3 coop: the greeting's hand on the tag
#include "Game/MP/MpLod.hpp"   // MP3 lod2: steady tags skip their layout animation
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpTagLayoutLogic.hpp"   // MP3 tags: overlap resolution
#include "Game/MP/MpGame.hpp"             // MP3 ingame
#include "Game/MP/MpPanelDraw.hpp"        // MP3 ingame: the picture's size in the panel's units
#include "Game/MP/MpPresent.hpp"          // MP3 ingame
#include "Game/MP/MpPresentLogic.hpp"     // MP3 ingame: when a tag shows, the edge heads
#include "Game/MP/MpSpectate.hpp"
#include "Game/MP/MpUi.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Screen/GalaxyNamePlate.hpp"
#include "Game/Screen/LayoutManager.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/FileUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include <cstring>
#include <nw4r/lyt/material.h>
#include <nw4r/lyt/textBox.h>
#include <nw4r/lyt/layout.h>

namespace {
    // MP3 lod2: a name tag's plate. Once its text has been shown for a while (its Appear animation done: the Wait animation only keeps
    // two pane visibilities constant) a tag costs only its placement: no pane animation (movement is stopped), and a layout calc only
    // when it moved (the matrices), not at all when it stands still. Anything shown again (a new name, the greeting's hand) starts
    // the full path for kSteadyTurns turns. SMG_MP_LOD2_TAGS=0: the plain GalaxyNamePlate behaviour.
    class MpTagPlate : public GalaxyNamePlate {
    public:
        MpTagPlate() : GalaxyNamePlate(nullptr, false), mAge(0), mHave(false), mX(0.0f), mY(0.0f) {}
        enum { kSteadyTurns = 100 };
        void restart() {   // (after every show())
            mAge = 0;
            mHave = false;
            MR::releaseAnimFrame(this);
        }
        virtual void calcAnim() {
            if (!MR::isExecuteCalcAnimLayout(this)) {
                return;
            }
            if (!MpLod::tagsOn() || mAge < kSteadyTurns) {
                if (mAge < kSteadyTurns) {
                    mAge++;
                }
                GalaxyNamePlate::calcAnim();
                MpLod::count(MpLod::C_TAG_ACTIVE);
                return;
            }
            const TVec2f t = getTrans();
            if (mHave && t.x == mX && t.y == mY) {
                MpLod::count(MpLod::C_TAG_STEADY);   // nothing moved: the matrices of the last calc stand
                return;
            }
            mHave = true;
            mX = t.x;
            mY = t.y;
            MR::stopAnimFrame(this);   // (LayoutActor::movement skips the pane animations from now on)
            LayoutManager* lm = getLayoutManager();
            lm->mLayout->CalculateMtx(lm->mDrawInfo);   // LayoutManager::calcAnimWithoutLocationAdjust minus animateRecursive
            MpLod::count(MpLod::C_TAG_STEADY);
        }
        u16 mAge;
        bool mHave;
        f32 mX, mY;
    };

    struct Tag {
        GalaxyNamePlate* plate;   // scene object: valid only between create() and the scene's end (MpPuppets clears)
        char name[MP_NAME_BYTES + 1];
        wchar_t text[MP_NAME_BYTES + 6];       // (MP3 touches: room for " zzz")
        wchar_t iconText[MP_NAME_BYTES + 10];  // MP3 coop: the picture font's open hand (picture 38), a space, the name
        bool icon;
        uint8_t look = 0xFF;      // MP3 touches: the look the plate was coloured for
        bool away = false;        // MP3 touches: "zzz" after the name
        bool shown;
        MpTagVis vis;             // (perf) wanted + fade
        f32 x, y;                 // last screen position (a fading-out tag stays where it was)
        f32 w;                    // MP3 tags: estimated plate width (px), from the name length (set on rename)
        bool want, edge, skip;    // MP3 tags: this picture's verdict of the first pass
        MpPresent::TagState ts;   // MP3 ingame: when the name shows
        f32 fade;                 // MP3 ingame: this picture's tag alpha
        bool edgeLogged;          // MP3 ingame (log once per appearance)
        bool tagLogged;
    };
    Tag sTags[MP_MAX_PLAYERS];
    MpPanel::Edge sEdge;          // MP3 ingame: the off-screen players' heads this picture
    bool sEdgeOn = false;
    MpTagLayout::State sLayout;   // MP3 tags: the stack offsets (MP_MAX_PLAYERS <= MpTagLayout::kMax)
    bool sEnabled;

    // touches: the character's colour (MpCharColor.hpp: the one table of the name tags, pointers, strip and lists)
    // The game's text draws through its colour mapping (the material's two TEV colours: nothing -> text); the character's colour goes in
    // there (the dark shade, readable on the white plate). The text box sits somewhere below the named pane, so this walks the children.
    static void tintText(nw4r::lyt::Pane* pane, const MpCharColor::C& c, int depth = 0) {
        if (pane == nullptr || depth > 6) return;
        nw4r::lyt::TextBox* box = nw4r::ut::DynamicCast< nw4r::lyt::TextBox* >(pane);
        nw4r::lyt::Material* mat = box != nullptr ? box->GetMaterial() : nullptr;
        if (mat != nullptr) {
            for (u32 k = 0; k < 2; k++) {
                GXColorS10 col = mat->GetTevColor(k);
                col.r = c.r; col.g = c.g; col.b = c.b;
                mat->SetTevColor(k, col);
            }
        }
        for (nw4r::lyt::PaneList::Iterator it = pane->GetChildList().GetBeginIter(); it != pane->GetChildList().GetEndIter(); ++it) tintText(&*it, c, depth + 1);
    }

    void setColor(GalaxyNamePlate* plate, int slot) {
        const MpCharColor::C cc = MpCharColor::ink(static_cast<int>(mp_slot_look(slot)));
        tintText(MR::getPane(plate, "GalaxyName"), cc);
        tintText(MR::getPane(plate, "GalaxyNameU"), cc);
    }

    void hide(Tag& t) {
        if (t.plate != nullptr && !MR::isDead(t.plate)) {
            t.plate->kill();
        }
        t.shown = false;
        t.vis.alpha = 0.0f;
    }

    // (perf) the fade: every pane's alpha times `a`, through the layout's draw info (whatever the plate's own animations do to its
    // panes). 1 = exactly the plate's own look.
    void setFade(GalaxyNamePlate* plate, f32 a) {
        LayoutManager* lm = plate->getLayoutManager();
        if (lm == nullptr) {
            return;
        }
        lm->mDrawInfo.SetGlobalAlpha(a);
        lm->mDrawInfo.SetInfluencedAlpha(a < 1.0f);
    }
}  // namespace

namespace MpNameTags {
    void create() {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            sTags[i].plate = nullptr;
            sTags[i].name[0] = '\0';
            sTags[i].text[0] = 0;
            sTags[i].iconText[0] = 0;
            sTags[i].icon = false;
            sTags[i].look = 0xFF;
            sTags[i].away = false;
            sTags[i].shown = false;
            sTags[i].vis.reset();
            sTags[i].x = sTags[i].y = 0.0f;
            sTags[i].w = 60.0f;
            sTags[i].want = sTags[i].edge = sTags[i].skip = false;
            sTags[i].ts.reset();
            sTags[i].fade = 0.0f;
            sTags[i].edgeLogged = sTags[i].tagLogged = false;
        }
        sLayout.reset();
        sEdge.count = 0;
        sEdgeOn = false;
        sEnabled = MR::isLoadedObjectOrLayoutArchive("GalaxyNamePlate");
        if (!sEnabled) {
            mp_log("puppet nametag off reason=noarchive");
            return;
        }
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            // (drop-in: with spares, every other slot gets a plate now; a joiner's puppet is bound mid-scene)
            if (i == mp_local_slot() || (MpPuppetsInt::puppet(i) == nullptr && MpPuppetsInt::spareCount() == 0 && !mp_slot_present(i))) {   // memfix: (a present player without a puppet still gets his tag)
                continue;
            }
            GalaxyNamePlate* plate = new MpTagPlate();
            MR::connectToSceneLayout(plate);
            plate->initWithoutIter();
            plate->setShowBalloonNozzle(true);
            plate->kill();
            sTags[i].plate = plate;
        }
    }

    // memfix: where a tag hangs. A puppet: its head. A present player with NO puppet (the scene heap could not hold it, or his
    // character has no spare): a point above his last pose of this scene, never culled. False: nothing to hang a tag on.
    bool tagHead(int slot, MpPuppetBase* p, TVec3f* head, bool* culled) {
        if (p != nullptr) {
            if (!p->isDrawable()) return false;
            p->capPosition(head);   // touches: the tag hangs from the top of the cap
            *culled = p->isCulled();
            return true;
        }
        MpPose pose;
        if (!mp_slot_present(slot) || !MpPuppets::lastPose(slot, &pose) || pose.sceneSerial != mp_scene_serial() || (pose.flags & MP_POSE_VISIBLE) == 0) return false;
        *head = TVec3f(pose.pos[0], pose.pos[1] + 170.0f, pose.pos[2]);
        *culled = false;
        return true;
    }

    // MP3 puppetprep (MpWarm): one plate (the first that exists) shows the greeting hand and the local name far off screen for a few
    // pictures, so the layout's textures and the font's glyph page are used once before a real tag needs them. `on` = false: it goes.
    bool warm(bool on) {
        if (!sEnabled) {
            return false;
        }
        Tag* t = nullptr;
        for (int i = 0; i < MP_MAX_PLAYERS && t == nullptr; i++) {
            if (sTags[i].plate != nullptr && !sTags[i].shown && MR::isDead(sTags[i].plate) == on) {
                t = &sTags[i];
            }
        }
        if (t == nullptr) {
            return false;
        }
        const int slot = (int)(t - sTags);
        if (!on) {
            if (!MR::isDead(t->plate)) {
                t->plate->kill();
            }
            return true;
        }
        const int me = mp_local_slot();
        const char* name = mp_slot_name(me >= 0 && me < MP_MAX_PLAYERS ? me : 0);
        wchar_t text[MP_NAME_BYTES + 5];
        text[0] = 0x1A; text[1] = 0x0306; text[2] = 38; text[3] = L' ';   // the picture font's open hand (as the greeting's tag)
        mp_utf8_to_utf16(name != nullptr ? name : "", &text[4], MP_NAME_BYTES + 1);
        t->plate->setShowBalloonNozzle(true);
        setColor(t->plate, slot);
        t->plate->show(text, true);
        static_cast< MpTagPlate* >(t->plate)->restart();   // MP3 lod2
        setColor(t->plate, slot);
        setFade(t->plate, 1.0f);
        t->plate->setTrans(TVec2f(-2000.0f, -2000.0f));
        return true;
    }

    // MP3 ingame: the game shows its own HUD (and nothing else of ours or the game's is over the play)
    static bool hudNow() {
        return MpTeam::hudShown() && MpGame::isGameSceneLive() && !MR::isDemoActive() && !MpUi::isMenuOpen() && !MpSpectate::watching();
    }

    int shown() {   // MP3 demo
        int n = 0;
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (sTags[i].plate != nullptr && !MR::isDead(sTags[i].plate) && sTags[i].shown) n++;
        }
        return n;
    }

    void update() {
        sEdge.count = 0;
        sEdgeOn = false;
        if (!sEnabled) {
            return;
        }
        const int mode = mp_settings()->nameTags;
        const bool hud = mode != 0 && hudNow();
        const f32 sw = (f32)MR::getScreenWidth(), sh = (f32)MR::getScreenHeight();
        const MpPanel::Screen scr = MpPanelDraw::screen();
        const f32 toSq = sw > 0.0f ? scr.width / sw : 1.0f;   // the panel's square units across (the height is the same)
        MpPresent::Rect hudRect[MpPresent::kHudRects];
        int others = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s != mp_local_slot() && mp_slot_present(s)) others++;
        }
        const int hudCount = MpPresent::hudRects(scr.width, scr.height, others > MpPanel::kMaxStrip ? MpPanel::kMaxStrip : others, hudRect);
        // MP3 tags: pass 1 projects every tag and decides whether it is wanted; the overlap push runs once on the results; pass 2 is
        // the old per-tag work (text, fade, placement) at the pushed place.
        MpTagLayout::Item items[MpTagLayout::kMax];
        for (int i = 0; i < MpTagLayout::kMax; i++) {
            items[i].active = false; items[i].x = items[i].y = 0.0f; items[i].w = 60.0f; items[i].d2 = 0.0f;
        }
        for (int slot = 0; slot < MP_MAX_PLAYERS && slot < MpTagLayout::kMax; slot++) {
            Tag& t = sTags[slot];
            t.skip = true;
            t.fade = 0.0f;
            if (t.plate == nullptr) {
                continue;
            }
            MpPuppetBase* p = MpPuppetsInt::puppet(slot);
            const bool present = mp_slot_present(slot);
            TVec3f head;
            bool culled = false;
            const bool drawable = present && tagHead(slot, p, &head, &culled);   // memfix: a present player with no puppet hangs the tag on his last pose
            bool onScreen = false, behind = false, far = false;
            f32 x = t.x, y = t.y, d2 = 0.0f, sx = 0.0f, sy = 0.0f;
            if (drawable) {
                const TVec3f cam = MR::getCamPos();
                const f32 dx = head.x - cam.x, dy = head.y - cam.y, dz = head.z - cam.z;
                d2 = dx * dx + dy * dy + dz * dz;
                TVec3f view;
                MR::getCameraViewMtx().mult(head, view);
                behind = view.z > 0.0f;   // the camera looks down -z
                TVec3f screen;
                MR::calcScreenPosition(&screen, head);
                sx = screen.x;
                sy = screen.y;
                // (ingame: well inside the picture: a head at the very edge, the character mostly out of view, gets the edge head, never a
                // name hanging over the HUD's corner)
                const f32 m = 48.0f * (sh / 456.0f);
                onScreen = !behind && !culled && sx >= m && sx <= sw - m && sy >= m && sy <= sh - m;
                far = !t.vis.wanted(1, true, false, d2, false);   // (the far rule of MpTagVis: beyond 6000 units, back within 5500)
                x = sx;
                y = sy + 24.0f * (sh / 456.0f);   // touches: the plate's beak ends ~5 px above the cap (the plate sits ~29 px above its anchor)
            }
            MpPresent::TagIn in;
            in.present = present;
            in.drawable = drawable && mode != 0;
            in.onScreen = onScreen;
            in.hud = hud;
            in.balloon = MpChat::balloonUp(slot);
            in.look = static_cast<uint8_t>(mp_slot_look(slot));
            const MpPresent::TagOut o = t.ts.step(in);
            if (o.tag > 0.0f && !t.tagLogged) {
                t.tagLogged = true;
                mp_log("puppet tag shown slot=%d", slot);
            } else if (o.tag <= 0.0f) {
                t.tagLogged = false;
            }
            // the edge head: the player's head where they are, at the picture's edge (square units), clear of the HUD
            if (o.edge > 0.0f && sEdge.count < MpPanel::kMaxEdges) {
                const MpPresent::EdgePlace ep = MpPresent::edgePlace(sx * toSq, sy, behind, scr.width, scr.height, 30.0f,
                                                                     MpPanel::kEdgeHeadSize * 0.5f + 8.0f, hudRect, hudCount);
                MpPanel::EdgeHead& e = sEdge.e[sEdge.count++];
                e.look = in.look < MpPanel::kCharacters ? in.look : 0;
                e.x = ep.x;
                e.y = ep.y;
                e.ax = ep.ax;
                e.ay = ep.ay;
                e.alpha = o.edge;
                sEdgeOn = true;
                if (!t.edgeLogged) {
                    t.edgeLogged = true;
                    mp_log("puppet edge head slot=%d at=%.0f,%.0f behind=%d", slot, ep.x, ep.y, behind ? 1 : 0);
                }
            } else if (o.edge <= 0.0f) {
                t.edgeLogged = false;
            }
            const bool want = o.tag > 0.0f && !far;
            if (!want && MR::isDead(t.plate)) {
                t.shown = false;
                t.vis.alpha = 0.0f;
                continue;
            }
            t.skip = false;
            t.want = want;
            t.edge = false;
            t.fade = want ? o.tag : 0.0f;
            items[slot].active = want;
            items[slot].x = x;
            items[slot].y = y;
            items[slot].w = t.w;
            items[slot].d2 = d2;
        }
        const f32 scale = sh / 456.0f;
        f32 outY[MpTagLayout::kMax];
        MpTagLayout::resolve(sLayout, items, MP_MAX_PLAYERS < MpTagLayout::kMax ? MP_MAX_PLAYERS : MpTagLayout::kMax, 28.0f * scale,
                             24.0f * scale, sh - 24.0f * scale, outY);
        for (int slot = 0; slot < MP_MAX_PLAYERS; slot++) {
            Tag& t = sTags[slot];
            if (t.skip) {
                continue;
            }
            const bool want = t.want;
            const f32 x = items[slot].x;
            const f32 y = outY[slot];   // MP3 tags: the stacked place (the projected y plus the smoothed push)
            if (!want) {
                hide(t);   // MP3 ingame: the fade is TagState's; a tag that is not wanted (off screen, the HUD hidden) is gone at once
                continue;
            }
            const f32 a = t.fade;
            t.vis.alpha = a;

            const char* name = mp_slot_name(slot);
            const bool rename = strncmp(name, t.name, MP_NAME_BYTES) != 0;
            if (rename) {   // (ingame: no "zzz" after an away player's name: the strip's head dims for it)
                strncpy(t.name, name, MP_NAME_BYTES);
                t.name[MP_NAME_BYTES] = '\0';
                mp_utf8_to_utf16(t.name, t.text, MP_NAME_BYTES + 1);
                int len = 0;
                while (len < MP_NAME_BYTES && t.text[len] != 0) {
                    len++;
                }
                t.w = 24.0f + 8.0f * (f32)(len < 4 ? 4 : len);   // MP3 tags: plate width estimate (px) for the overlap push
            }
            // MP3 coop: a greeting flashes the open hand in front of the name (a picture tag: 0x1A, size 6 | group 3 << 8, 38)
            const bool icon = MpCoop::nameTagIcon(slot);
            if (icon && (rename || !t.icon)) {
                t.iconText[0] = 0x1A;
                t.iconText[1] = 0x0306;
                t.iconText[2] = 38;
                t.iconText[3] = L' ';
                memcpy(&t.iconText[4], t.text, sizeof t.text);
                t.iconText[MP_NAME_BYTES + 9] = 0;
            }
            const bool iconChanged = icon != t.icon;
            t.icon = icon;
            const uint8_t look = static_cast<uint8_t>(mp_slot_look(slot));
            const bool lookChanged = look != t.look;   // MP3 touches: a character change recolours the tag
            t.look = look;
            if (MR::isDead(t.plate) || rename || iconChanged || lookChanged || !t.shown) {
                t.plate->setShowBalloonNozzle(true);
                setColor(t.plate, slot);
                t.plate->show(icon ? t.iconText : t.text, true);
                static_cast< MpTagPlate* >(t.plate)->restart();   // MP3 lod2: the full path again for a while
                setColor(t.plate, slot);
                t.shown = true;
            } else {
                t.plate->_24 = true;   // keep it alive this turn (the plate kills itself when not shown for a turn)
            }
            setFade(t.plate, a);
            t.x = x;
            t.y = y;
            t.plate->setTrans(TVec2f(x, y));
        }
    }

    bool edgeModel(MpPanel::Edge* out) {   // MP3 ingame
        if (out == nullptr || !sEdgeOn) {
            return false;
        }
        *out = sEdge;
        return sEdge.count > 0;
    }

    bool edgeOf(int slot, f32* x, f32* y) {   // MP3 ingame: (square units) the edge head of that slot this picture
        int k = 0;
        for (int s = 0; s < MP_MAX_PLAYERS && k < sEdge.count; s++) {
            if (sTags[s].ts.edgeA > 0.0f) {
                if (s == slot) {
                    *x = sEdge.e[k].x;
                    *y = sEdge.e[k].y;
                    return true;
                }
                k++;
            }
        }
        return false;
    }
}  // namespace MpNameTags

namespace MpPuppetsInt {
    void nameTagsEndScene() {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            sTags[i].plate = nullptr;
            sTags[i].shown = false;
            sTags[i].icon = false;
            sTags[i].name[0] = '\0';
            sTags[i].vis.reset();
            sTags[i].ts.reset();
        }
        sLayout.reset();
        sEdge.count = 0;
        sEdgeOn = false;
        sEnabled = false;
    }
}  // namespace MpPuppetsInt
