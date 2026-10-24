// The settings of the handheld ports under the game's pause menu: camera zoom (L / R) and rear touch aiming (Triangle).
//
// PauseMenu::draw calls port_settings_panel_draw every picture; the panel shows while the menu waits for a choice
// (port_zoom_menu_active, platform/src/port/perf.cpp, which also keeps the values and saves them in settings.ini; the buttons
// are read by vita/runtime/input.cpp). The layout and look are in settings_panel_plan.cpp; here the plan is drawn with GX in the
// game's 2D pass: shapes as coloured triangles, words in the game's message font and the buttons from its picture font (whose
// Wii buttons the handheld patch has replaced by the console's: handheld_ui.cpp), both through nw4r::ut::CharWriter, the
// same writer the layouts' text boxes use.
//
// port_settings_panel_turn (from port_zoom_menu, game thread, once a turn) plays the game's system sounds when a setting changes
// and steps the panel's animations. Switches (env.txt, read once):
//   PETARI_SETTINGS_PANEL=0   no panel (L / R / Triangle still change the settings in the pause menu)       default 1
//   PETARI_SETTINGS_SE=0      no sounds when a setting changes                                              default 1
#include <math.h>
#include <stdlib.h>

#include <revolution/gx.h>
#include <revolution/mtx.h>

#include "Game/System/Language.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "nw4r/ut/CharWriter.h"
#include "nw4r/ut/Font.h"

#include "port/port.h"
#include "settings_panel_plan.h"

extern "C" int port_handheld(void);
extern "C" int port_zoom_menu_active(void);
extern "C" int port_zoom_tenths(void);
extern "C" int port_rear_aim_enabled(void);
extern "C" unsigned port_zoom_limit_hits(void);

namespace {
namespace sp = settings_panel;

bool EnvOn(const char* name) {
    const char* e = getenv(name);
    return !(e && *e) || atoi(e) != 0;
}
bool PanelOn() {
    static const bool on = EnvOn("PETARI_SETTINGS_PANEL");
    return on;
}
bool SoundOn() {
    static const bool on = EnvOn("PETARI_SETTINGS_SE");
    return on;
}

// ---- the panel's animation state (game thread only: the turn hook and the draw both run there)
bool sActive = false;
float sFade = 0.0f, sKnob = 0.0f, sPulse = 0.0f;
int sLastZoom = -1, sLastRear = -1;
unsigned sLastLimitHits = 0;
int sTurnsSinceLimitSound = 1000;
int sLanguage = -1;
bool sLogged = false;

void Sound(const char* name) {
    if (SoundOn()) MR::startSystemSE(name, -1, -1);
}

// ---- fonts as the plan sees them
float Advance(void* font, uint16_t code) { return static_cast<float>(static_cast<const nw4r::ut::Font*>(font)->GetCharWidth(code)); }
bool Has(void* font, uint16_t code) { return static_cast<const nw4r::ut::Font*>(font)->HasGlyph(code); }
sp::FontMetrics Metrics(const nw4r::ut::Font* f) {
    sp::FontMetrics m;
    m.height = static_cast<float>(f->GetHeight());
    m.baseline = static_cast<float>(f->GetBaselinePos());
    m.context = const_cast<nw4r::ut::Font*>(f);
    m.advance = Advance;
    m.has = Has;
    return m;
}

// The picture's square units (y down) over the whole frame buffer: the game draws 16:9 into the same frame buffer as 4:3.
void Projection(float width, float height) {
    Mtx44 projection;
    C_MTXOrtho(projection, 0.0f, height, 0.0f, width, -1000.0f, 1000.0f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    Mtx identity;
    PSMTXIdentity(identity);
    GXLoadPosMtxImm(identity, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
}

void DrawShapes(const sp::Plan& plan) {
    if (plan.vertexCount <= 0) return;
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumIndStages(0);
    GXSetNumTevStages(1);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    const GXColor black = {0, 0, 0, 0};
    GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, black);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_SET);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
    // (in pieces of at most 3,000 vertices: GXBegin's count is 16 bits, and a smaller batch is kinder to the command buffer)
    for (int start = 0; start < plan.vertexCount; start += 3000) {
        const int n = plan.vertexCount - start < 3000 ? plan.vertexCount - start : 3000;
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast<u16>(n));
        for (int i = start; i < start + n; i++) {
            const sp::Vertex& v = plan.vertices[i];
            GXPosition2f32(v.x, v.y);
            GXColor4u8(v.c.r, v.c.g, v.c.b, v.c.a);
        }
        GXEnd();
    }
}

void DrawRuns(const sp::Plan& plan, const nw4r::ut::Font* fonts[2]) {
    for (int f = 0; f < 2; f++) {
        bool setUp = false;
        nw4r::ut::CharWriter writer;
        writer.SetFont(*fonts[f]);
        for (int i = 0; i < plan.runCount; i++) {
            const sp::TextRun& run = plan.runs[i];
            if (run.font != f) continue;
            if (!setUp) {
                writer.SetupGX();  // (the TEV of the font's texture format, the vertex format; also forgets the last texture)
                GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
                setUp = true;
            }
            writer.SetScale(run.scaleX, run.scaleY);
            writer.SetTextColor(nw4r::ut::Color((u32(run.color.r) << 24) | (u32(run.color.g) << 16) | (u32(run.color.b) << 8) | run.color.a));
            float x = run.x;
            for (int k = 0; k < run.length; k++) {
                writer.SetCursor(x, run.y, 0.0f);
                x += writer.Print(static_cast<u16>(run.text[k]));
            }
        }
    }
}
}  // namespace

// Game thread, once a turn (from port_zoom_menu): sounds and animation. `selecting`: the pause menu waits for a choice.
extern "C" void port_settings_panel_turn(int selecting) {
    if (!selecting || !port_handheld()) {
        if (sActive) port_log("settings panel: hidden (zoom %d.%d, rear touch aiming %s)", sLastZoom / 10, sLastZoom % 10, sLastRear ? "on" : "off");
        sActive = false;
        sFade = 0.0f;
        sPulse = 0.0f;
        return;
    }
    const int zoom = port_zoom_tenths(), rear = port_rear_aim_enabled() ? 1 : 0;
    const unsigned hits = port_zoom_limit_hits();
    if (!sActive) {
        sActive = true;
        sLastZoom = zoom;
        sLastRear = rear;
        sLastLimitHits = hits;
        sKnob = static_cast<float>(rear);
        sLogged = false;  // (the placement is logged once each time the panel shows)
    }
    if (zoom != sLastZoom) {
        Sound("SE_SY_SELECT_PAUSE_ITEM");  // (the pause menu's own "pointing at a button" sound)
        port_log("settings panel: zoom %d.%d", zoom / 10, zoom % 10);
        sLastZoom = zoom;
        sPulse = 1.0f;
    }
    if (hits != sLastLimitHits) {
        // L at 1.0 or R at 2.0: the game's "the camera can't go there" sound, at most twice a second while the button repeats
        if (sTurnsSinceLimitSound >= 30) {
            Sound("SE_SY_CAMERA_NG");
            port_log("settings panel: zoom already at %d.%d", zoom / 10, zoom % 10);
            sTurnsSinceLimitSound = 0;
        }
        sLastLimitHits = hits;
    }
    if (rear != sLastRear) {
        Sound("SE_SY_TALK_OK");  // (the confirm sound of the game's message windows)
        port_log("settings panel: rear touch aiming %s", rear ? "on" : "off");
        sLastRear = rear;
    }
    if (sTurnsSinceLimitSound < 1000) ++sTurnsSinceLimitSound;
    sFade = sFade + 1.0f / 8.0f > 1.0f ? 1.0f : sFade + 1.0f / 8.0f;
    sKnob += (static_cast<float>(rear) - sKnob) * 0.4f;
    if (fabsf(sKnob - rear) < 0.01f) sKnob = static_cast<float>(rear);
    sPulse *= 0.78f;
    if (sPulse < 0.01f) sPulse = 0.0f;
}

// PauseMenu::draw, every picture. flags: 1 = the Luigi letter button is on screen.
extern "C" void port_settings_panel_draw(int flags) {
    if (!PanelOn() || !port_zoom_menu_active() || !sActive) return;
    const nw4r::ut::Font* fonts[2] = {MR::getFontOnCurrentLanguage(), MR::getPictureFontNW4R()};
    if (!fonts[0] || !fonts[1]) return;
    if (sLanguage < 0) sLanguage = sp::LanguageIndex(MR::getCurrentLanguagePrefix());

    sp::State st;
    st.height = static_cast<float>(MR::getScreenHeight());
    st.width = st.height * (MR::isScreen16Per9() ? 16.0f / 9.0f : 4.0f / 3.0f);
    st.zoomTenths = port_zoom_tenths();
    st.rearAim = port_rear_aim_enabled() != 0;
    st.knob = sKnob;
    st.pulse = sPulse;
    st.alpha = sFade;
    st.compact = (flags & 1) != 0;
    st.language = sLanguage;
    const sp::FontMetrics metrics[2] = {Metrics(fonts[0]), Metrics(fonts[1])};
    static sp::Plan plan;  // (big: kept out of the stack)
    sp::Build(st, metrics, &plan);
    if (!sLogged || plan.overflow) {
        sLogged = true;
        port_log("settings panel: %s, %.0f x %.0f units, panel %.0f,%.0f - %.0f,%.0f, %d vertices, %d runs%s%s", sp::LanguageName(sLanguage),
                 st.width, st.height, plan.x0, plan.y0, plan.x1, plan.y1, plan.vertexCount, plan.runCount, st.compact ? ", compact" : "",
                 plan.overflow ? ", OVERFLOW" : "");
    }

    Projection(st.width, st.height);
    DrawShapes(plan);
    DrawRuns(plan, fonts);
    // (the layouts drawn after this one set their own projection, matrices, vertex format and TEV: MR::setupDrawForNW4RLayout and
    // the panes' materials; the 2D state they assume is put back here all the same)
    GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
}
