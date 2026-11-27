// Galaxy 3.0 multiplayer: the menus with the game's own layouts, plus GX for what no layout shows (owner: ui).
//
// Layouts (LayoutData/*.arc), each a system-level LayoutActor updated and drawn by hand like SaveDataHandleSequence's windows:
//   PauseMenu          the dimmed background, the title band (title, scenario line) and (uinative) the lower band of the counters,
//                      lowered, carrying the button guide; its Appear / End animations
//   RetryButton x5 x2  one big button each (the upper one, NBackUp); ButtonPaneController drives ButtonAppear / Wait / SelectIn /
//                      SelectOut / End, the game's own float and highlight; we point it from the d-pad (manual pointing). uinative:
//                      two banks: when the screen changes the old buttons end (the chosen one plays ButtonDecide first) while the
//                      next screen's buttons appear in the other bank
//   DPDPointer         (uinative) the Star Pointer's hand, its finger pose, resting on the selected item (the menus' cursor)
//   MiiSelect          (uinative) its blue arrows either side of the selected option's value
//   AllStarList        (uinative) its page number and round up / down buttons beside a list that has pages
//   SysInfoWindowMini x5  the message-window frame (WinInfo + its shadow), resized: lists, the busy window, the waiting screen /
//                      results card, three toasts; ButtonAppear / ButtonEnd on open and close
//   MiiIcon x10        character heads in player rows and toasts (its Character animation: Mario, Luigi, Toad; Cosmic Mario is
//                      Mario's head tinted with Cosmic Mario's starry blue: no Cosmic Mario icon exists in LayoutData)
//   MiiIcon x4         the character select's four big floating heads (the file select's icon picker), with ButtonPaneController
// GX (the game's 2D pass, after the layouts): list rows, values, hints and the spinner, in the game's message and picture fonts
// through nw4r::ut::CharWriter, as platform/src/port/settings_panel.cpp draws.
#include "Game/MP/MpPanelDraw.hpp"

#include <math.h>
#include <string.h>

#include "Game/MP/MpHead3D.hpp"   // MP3 heads3d: the character select's heads in 3D (the file select's own)
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpLod.hpp"   // MP3 lod2: heads that did not change skip their layout work
#include "Game/MP/MpUiLayouts.hpp"
#include "port/mp_proto.h"
#include "Game/Screen/ButtonPaneController.hpp"
#include "Game/Screen/LayoutActor.hpp"
#include "Game/Screen/LayoutCoreUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/StarPointerUtil.hpp"
#include "nw4r/lyt/pane.h"
#include "nw4r/lyt/material.h"
#include "nw4r/lyt/textBox.h"

#include <revolution/gx.h>
#include <revolution/mtx.h>

#include "Game/Util/ScreenUtil.hpp"
#include "Game/Util/SystemUtil.hpp"
#include "nw4r/ut/CharWriter.h"
#include "nw4r/ut/Font.h"

namespace MpPanelDraw {
namespace {

float Advance(void* font, uint16_t code) { return static_cast<float>(static_cast<const nw4r::ut::Font*>(font)->GetCharWidth(code)); }
bool Has(void* font, uint16_t code) { return static_cast<const nw4r::ut::Font*>(font)->HasGlyph(code); }

void Projection(float width, float height) {
    Mtx44 projection;
    C_MTXOrtho(projection, 0.0f, height, 0.0f, width, -1000.0f, 1000.0f);
    GXSetProjection(projection, GX_ORTHOGRAPHIC);
    Mtx identity;
    PSMTXIdentity(identity);
    GXLoadPosMtxImm(identity, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
}

void DrawShapes(const MpPanel::Vertex* verts, int vertexCount) {
    if (vertexCount <= 0) return;
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
    for (int start = 0; start < vertexCount; start += 3000) {
        const int n = vertexCount - start < 3000 ? vertexCount - start : 3000;
        GXBegin(GX_TRIANGLES, GX_VTXFMT0, static_cast<u16>(n));
        for (int i = start; i < start + n; i++) {
            const MpPanel::Vertex& v = verts[i];
            GXPosition2f32(v.x, v.y);
            GXColor4u8(v.c.r, v.c.g, v.c.b, v.c.a);
        }
        GXEnd();
    }
}

// The runs in plan order, each font's writer set up when the font changes (so a toast's words stay above its own box, and
// the shapes of the next toast above the words of the one before: shapes and words are drawn in two passes per plan, which is
// enough because the boxes never overlap).
void DrawRuns(const MpPanel::TextRun* runs, int runCount, const char16_t* text, int textCount, const nw4r::ut::Font* fonts[MpPanel::FONT_COUNT]) {
    for (int f = 0; f < MpPanel::FONT_COUNT; f++) {
        if (!fonts[f]) continue;   // (the display face may be missing: the planner then used the message font)
        bool setUp = false;
        nw4r::ut::CharWriter writer;
        writer.SetFont(*fonts[f]);
        for (int i = 0; i < runCount; i++) {
            const MpPanel::TextRun& run = runs[i];
            if (run.font != f || run.start < 0 || run.start + run.length > textCount) continue;
            if (!setUp) {
                writer.SetupGX();
                GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
                setUp = true;
            }
            writer.SetScale(run.scaleX, run.scaleY);
            writer.SetTextColor(nw4r::ut::Color((u32(run.color.r) << 24) | (u32(run.color.g) << 16) | (u32(run.color.b) << 8) | run.color.a));
            float x = run.x;
            for (int k = 0; k < run.length; k++) {
                writer.SetCursor(x, run.y, 0.0f);
                x += writer.Print(static_cast<u16>(text[run.start + k]));
            }
        }
    }
}

}  // namespace

// The game's big menu face (MenuFont64), made with the others from the always-resident system archive Font.arc
// (GameSystemFontHolder::createFontFromFile): nothing to request, no extra memory. Null before the holder has it.
static const nw4r::ut::Font* DisplayFont() { return MR::getMenuFontNW4R(); }

bool fonts(MpPanel::FontMetrics out[MpPanel::FONT_COUNT]) {
    const nw4r::ut::Font* f[MpPanel::FONT_COUNT] = {MR::getFontOnCurrentLanguage(), MR::getPictureFontNW4R(), DisplayFont()};
    if (!f[0] || !f[1]) return false;
    out[MpPanel::FONT_DISPLAY] = MpPanel::FontMetrics();   // (no metrics = not there)
    for (int i = 0; i < MpPanel::FONT_COUNT; i++) {
        if (!f[i]) continue;
        out[i].height = static_cast<float>(f[i]->GetHeight());
        out[i].baseline = static_cast<float>(f[i]->GetBaselinePos());
        out[i].context = const_cast<nw4r::ut::Font*>(f[i]);
        out[i].advance = Advance;
        out[i].has = Has;
    }
    return true;
}

MpPanel::Screen screen() {
    MpPanel::Screen s;
    s.height = static_cast<float>(MR::getScreenHeight());
    s.width = s.height * (MR::isScreen16Per9() ? 16.0f / 9.0f : 4.0f / 3.0f);
    return s;
}

void DrawPlan(const MpPanel::PlanBase& plan) {
    if (plan.vertexCount <= 0 && plan.runCount <= 0) return;
    const nw4r::ut::Font* f[MpPanel::FONT_COUNT] = {MR::getFontOnCurrentLanguage(), MR::getPictureFontNW4R(), DisplayFont()};
    if (!f[0] || !f[1]) return;
    const MpPanel::Screen s = screen();
    Projection(s.width, s.height);
    DrawShapes(plan.vertices, plan.vertexCount);
    DrawRuns(plan.runs, plan.runCount, plan.text, plan.textCount, f);
    // (the layouts drawn after this set their own state; the 2D state they assume is put back all the same)
    GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
}

// MP3 heads: a few triangles and words of the 2D pass outside any plan (the 3D heads' owner badges): the same state as DrawPlan.
void drawMini(const MpPanel::Vertex* vertices, int vertexCount, const MpPanel::TextRun* runs, int runCount, const char16_t* text, int textCount) {
    if (vertexCount <= 0 && runCount <= 0) return;
    const nw4r::ut::Font* f[MpPanel::FONT_COUNT] = {MR::getFontOnCurrentLanguage(), MR::getPictureFontNW4R(), DisplayFont()};
    if (!f[0] || !f[1]) return;
    const MpPanel::Screen s = screen();
    Projection(s.width, s.height);
    DrawShapes(vertices, vertexCount);
    DrawRuns(runs, runCount, text, textCount, f);
    GXSetZMode(GX_FALSE, GX_NEVER, GX_FALSE);
    GXSetCullMode(GX_CULL_NONE);
}

// ------------------------------------------------------------------ the layouts
namespace {

// a text for a text box: pictures as the game's picture tags
const wchar_t* BoxText(const char16_t* text, char16_t* buf, int cap) {
    MpMsgText::toWindowText(buf, cap, text, false);
    return reinterpret_cast<const wchar_t*>(buf);
}

// flow (door run, 12 Nov: "or one t / he game made"): the game's message window breaks a line that is too wide wherever it runs out,
// inside a word (the game's own messages are broken by hand). The line is broken here at the last space that still fits the text
// box, measured with the box's own font, size and spacing; breaks already in the text stay.
const nw4r::lyt::TextBox* FindTextBox(const nw4r::lyt::Pane* p) {   // (the first text box at or under this pane: the window's text)
    if (p == nullptr) return nullptr;
    if (const nw4r::lyt::TextBox* t = nw4r::ut::DynamicCast< const nw4r::lyt::TextBox* >(p)) return t;
    const nw4r::lyt::PaneList& kids = p->GetChildList();
    for (nw4r::lyt::PaneList::ConstIterator it = kids.GetBeginIter(); it != kids.GetEndIter(); ++it)
        if (const nw4r::lyt::TextBox* t = FindTextBox(&*it)) return t;
    return nullptr;
}
// the text in `out` with its lines broken at the last space that fits the box; returns how many lines it has
int WrapForBox(LayoutActor* a, const char* paneName, const char16_t* in, char16_t* out, int cap) {
    int n = 0;
    while (in[n] && n < cap - 1) out[n] = in[n], ++n;
    out[n] = 0;
    int lines = 1;
    for (int i = 0; i < n; i++) lines += out[i] == u'\n' ? 1 : 0;
    const nw4r::lyt::TextBox* box = FindTextBox(MR::getPane(a, paneName));
    if (box == nullptr || box->mpFont == nullptr || box->mpFont->GetWidth() <= 0) return lines;
    const float scale = box->mFontSize.width / static_cast<float>(box->mpFont->GetWidth());
    const float room = box->mSize.width - 2.0f;
    float w = 0;
    int lastSpace = -1;
    lines = 1;
    for (int i = 0; i < n; i++) {
        const char16_t c = out[i];
        if (c == u'\n') { w = 0, lastSpace = -1, ++lines; continue; }
        if (c == u' ') lastSpace = i;
        w += static_cast<float>(box->mpFont->GetCharWidth(static_cast<u16>(c))) * scale + box->mCharSpace;
        if (w > room && lastSpace >= 0) {
            out[lastSpace] = u'\n';
            ++lines;
            i = lastSpace;
            w = 0, lastSpace = -1;
        }
    }
    return lines;
}

void SetRoot(LayoutActor* a, float x, float y) {
    nw4r::lyt::Pane* root = MR::getRootPane(a);
    if (!root) return;
    root->mTranslate.x = x;
    root->mTranslate.y = y;
    root->mTranslate.z = 0.0f;
}

u8 Alpha(float a) { return static_cast<u8>(a <= 0 ? 0 : (a >= 1 ? 255 : a * 255.0f + 0.5f)); }

// uinative2: a text box with no message of its own keeps the string buffer its layout file asked for (RetryButton's Txt1: ten
// characters: "Play Toget", "Leaderboar", "Rooms Near"); every box we write gets room for our longest text (kSceneText, with tags)
// once, when the layout is made
class GrowTextBuffer : public TextBoxRecursiveOperation {
public:
    explicit GrowTextBuffer(u32 chars) : mChars(chars) {}
    virtual void execute(nw4r::lyt::TextBox* box) const { LayoutCoreUtil::reallocateTextBoxMessage(box, mChars); }
    u32 mChars;
};
void GrowText(LayoutActor* a, const char* pane) {
    if (MR::getPane(a, pane)) MR::executeTextBoxRecursive(a, pane, GrowTextBuffer(2 * MpPanel::kSceneText));
}

class MpFrameLayout : public LayoutActor {
public:
    MpFrameLayout() : LayoutActor("MpFrame", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("PauseMenu", 1);
        MR::createAndAddPaneCtrl(this, "BG", 1);
        MR::createAndAddPaneCtrl(this, "PicStagebase", 1);
        MR::createAndAddPaneCtrl(this, "PicStagebase1", 1);
        MR::hidePaneRecursive(this, "Stars");
        MR::hidePaneRecursive(this, "Counter");
        MR::hidePaneRecursive(this, "BackButton");
        MR::hidePaneRecursive(this, "LetterButton");
        MR::hidePane(this, "PicStagebase1");
        if (nw4r::lyt::Pane* band = MR::getPane(this, "PicStagebase")) mBandY = band->mTranslate.y + MpPanel::kBandLift;
        if (nw4r::lyt::Pane* title = MR::getPane(this, "ShaStageTitle")) mTitleY = title->mTranslate.y - MpPanel::kTitleDrop;
        if (nw4r::lyt::Pane* line = MR::getPane(this, "ScenarioTitle")) mLineY = line->mTranslate.y + MpPanel::kLineLift;
        if (nw4r::lyt::Pane* foot = MR::getPane(this, "PicStagebase1")) foot->mTranslate.y -= MpPanel::kFootDrop;
        // uinative2: the band's text boxes are 450 units wide (a galaxy's name) and break a longer line inside a word ("networ / k."):
        // as wide as the picture (less the margins the planner keeps), so a line stays on one line and is only ever made smaller
        const float boxW = (MR::isScreen16Per9() ? 832.0f : 608.0f) - MpPanel::kBandMargin;
        for (const char* name : {"ShaStageTitle", "TxtStageTitle", "ShaScenario", "TxtScenario"})
            if (nw4r::lyt::Pane* p = MR::getPane(this, name)) p->mSize.width = boxW;
        GrowText(this, "ShaStageTitle");
        GrowText(this, "ScenarioTitle");
        kill();
    }
    void use(const MpPanel::LayoutUse& u, const MpPanel::Scene& sc, bool keepWords = false) {
        mWanted = true;
        if (MR::isDead(this) || mClosing) {
            appear();
            MR::startPaneAnim(this, "BG", "Appear", 0);
            MR::startPaneAnim(this, "PicStagebase", "Appear", 0);
            MR::startPaneAnim(this, "PicStagebase1", "Appear", 0);
            mClosing = false;
            mTitle[0] = mLine[0] = 0xFFFF;
        }
        // uinative: the title lower when it stands alone; a title or line too wide for the picture smaller (about its middle; the
        // line's pane is scaled about the band, so its 52-unit drop is kept)
        const bool alone = (u.state & MpPanel::kFrameNoLine) != 0;   // (a title alone: a slimmer band, the title in its middle)
        if (nw4r::lyt::Pane* band = MR::getPane(this, "PicStagebase")) band->mTranslate.y = mBandY + (alone ? MpPanel::kBandShrink : 0.0f) + ((u.state & MpPanel::kFrameTight) ? MpPanel::kBandTight : 0.0f);
        if (nw4r::lyt::Pane* title = MR::getPane(this, "ShaStageTitle")) {
            title->mTranslate.y = mTitleY - (alone ? MpPanel::kTitleAlone : 0.0f) + ((u.state & MpPanel::kFrameTight) ? MpPanel::kTitleTight : 0.0f);
            title->mScale.x = title->mScale.y = u.w > 0.0f && u.w < 1.0f ? u.w : 1.0f;
        }
        if (nw4r::lyt::Pane* line = MR::getPane(this, "ScenarioTitle")) {
            const float k = u.h > 0.0f && u.h < 1.0f ? u.h : 1.0f;
            line->mScale.x = line->mScale.y = k;
            line->mTranslate.y = mLineY - 52.0f * (1.0f - k);
        }
        const bool band = (u.state & MpPanel::kFrameNoTitle) == 0;   // (uinative: a screen with no title has no title band)
        if (band != mBand) {
            mBand = band;
            if (band) MR::showPane(this, "PicStagebase");
            else MR::hidePane(this, "PicStagebase");
        }
        const bool foot = (u.state & MpPanel::kFrameFoot) != 0;   // (the guide's band: only when there is a guide)
        if (foot != mFoot) {
            mFoot = foot;
            if (foot) MR::showPane(this, "PicStagebase1");
            else MR::hidePane(this, "PicStagebase1");
        }
        const char16_t* title = u.text >= 0 ? sc.texts[u.text] : u"";
        const char16_t* line = u.text2 >= 0 ? sc.texts[u.text2] : u"";
        char16_t buf[160];
        if (keepWords && mTitle[0] != 0xFFFF) return;   // uinative4: the band's words change with the buttons (the old buttons still leaving)
        if (MpPanel::copyChanged(mTitle, 96, title)) MR::setTextBoxMessageRecursive(this, "ShaStageTitle", BoxText(title, buf, 160));
        if (MpPanel::copyChanged(mLine, 96, line)) MR::setTextBoxMessageRecursive(this, "ScenarioTitle", BoxText(line, buf, 160));
    }
    void endFrame() {
        if (!mWanted && !MR::isDead(this) && !mClosing) {
            MR::startPaneAnim(this, "BG", "End", 0);
            MR::startPaneAnim(this, "PicStagebase", "End", 0);
            MR::startPaneAnim(this, "PicStagebase1", "End", 0);
            mClosing = true;
        }
        if (mClosing && MR::isPaneAnimStopped(this, "BG", 0)) {
            mClosing = false;
            kill();
        }
        mWanted = false;
    }
    bool mWanted = false, mClosing = false, mFoot = false, mBand = true;
    float mBandY = 0.0f, mTitleY = 0.0f, mLineY = 0.0f;
    char16_t mTitle[96] = {0xFFFF, 0}, mLine[96] = {0xFFFF, 0};
};

// wrap25 (25 Nov console: "Look for Friends Nearby" came out as "Look for Friends Near / by"): the big button's text boxes are 304 units
// wide and the game's text box breaks a line that is too wide wherever it runs out. A label wider than the box is drawn one line smaller
// instead (the font size of its text boxes, from the layout's own size), never below half; only then does the box grow so a word is
// never split. Each line of the text (at '\n') is measured with the box's font, as WrapForBox does.
struct FitBase { const nw4r::lyt::TextBox* box; float fw, fh, bw; };
class FitButtonText : public TextBoxRecursiveOperation {
public:
    FitButtonText(const char16_t* text, FitBase* bases, int* count) : mText(text), mBases(bases), mCount(count) {}
    virtual void execute(nw4r::lyt::TextBox* box) const {
        FitBase* b = nullptr;
        for (int i = 0; i < *mCount; i++) if (mBases[i].box == box) b = &mBases[i];
        if (!b) {
            if (*mCount >= 4) return;
            b = &mBases[(*mCount)++];
            *b = {box, box->mFontSize.width, box->mFontSize.height, box->mSize.width};
        }
        box->mFontSize.width = b->fw, box->mFontSize.height = b->fh, box->mSize.width = b->bw;
        if (box->mpFont == nullptr || box->mpFont->GetWidth() <= 0) return;
        const float scale = b->fw / static_cast<float>(box->mpFont->GetWidth());
        const float room = b->bw - 2.0f;
        float k = 1.0f, widest = 0.0f;
        float g = 0.0f, c = 0.0f;
        for (const char16_t* t = mText;; ++t) {
            if (*t == 0 || *t == u'\n') {
                if (g * k + c > room && g > 0.0f) k = (room - c) / g > 0.5f ? (room - c) / g : 0.5f;
                if (g * k + c > widest) widest = g * k + c;
                g = c = 0.0f;
                if (*t == 0) break;
                continue;
            }
            g += static_cast<float>(box->mpFont->GetCharWidth(static_cast<u16>(*t))) * scale;
            c += box->mCharSpace;
        }
        if (k < 1.0f) box->mFontSize.width = b->fw * k, box->mFontSize.height = b->fh * k;
        if (widest + 2.0f > b->bw) box->mSize.width = widest + 2.0f;
    }
private:
    const char16_t* mText;
    FitBase* mBases;
    int* mCount;
};

class MpButtonLayout : public LayoutActor {
public:
    MpButtonLayout() : LayoutActor("MpButton", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("RetryButton", 1);
        MR::hidePaneRecursive(this, "BG");
        MR::hidePaneRecursive(this, "NBackDown");
        GrowText(this, "Text1");
        mCtrl = new ButtonPaneController(this, "NBackUp", "BoxButton1", 0, true);
        mCtrl->_22 = false;
        mCtrl->_23 = true;   // (pointed by the menu, from the d-pad; the star pointer is read separately)
        kill();
    }
    virtual void control() { mCtrl->update(); }
    void use(const MpPanel::LayoutUse& u, const MpPanel::Scene& sc) {
        mWanted = true;
        const char16_t* text = u.text >= 0 ? sc.texts[u.text] : u"";
        if (MR::isDead(this) || mCtrl->isHidden() || mCtrl->isDisappearing() || mDeciding) {
            appear();
            mCtrl->_22 = false;
            mCtrl->appear();
            mDeciding = false;
            mText[0] = 0xFFFF;
        }
        if (MpPanel::copyChanged(mText, 96, text)) {
            char16_t buf[160];
            MR::setTextBoxMessageRecursive(this, "Text1", BoxText(text, buf, 160));
            MR::executeTextBoxRecursive(this, "Text1", FitButtonText(text, mFit, &mFitCount));   // wrap25: one line, smaller, never a split word
        }
        SetRoot(this, u.x, u.y);
        if (nw4r::lyt::Pane* root = MR::getRootPane(this)) root->mScale.x = root->mScale.y = u.w > 0 ? u.w : 1.0f;   // (four or five: smaller)
        MR::setLayoutAlpha(this, Alpha(u.alpha * (u.state == MpPanel::BUTTON_DISABLED ? 0.45f : 1.0f)));
        if (u.state == MpPanel::BUTTON_SELECTED) mCtrl->onPointing();
    }
    // uinative: this button was chosen and its screen has gone: the game's decide (ButtonDecide, then out), then gone
    void decideOut() {
        if (MR::isDead(this) || mCtrl->isHidden() || mCtrl->isDisappearing() || mDeciding) return;
        mCtrl->_22 = true;   // (after the decide: ButtonEnd, then hidden)
        mCtrl->decide();
        mDeciding = true;
    }
    void endFrame() {
        if (!mWanted && !MR::isDead(this)) {
            if (!mDeciding && !mCtrl->isHidden() && !mCtrl->isDisappearing()) mCtrl->disappear();
            if (mCtrl->isHidden()) {
                mDeciding = false;
                kill();
            }
        }
        mWanted = false;
    }
    ButtonPaneController* mCtrl = nullptr;
    bool mWanted = false, mDeciding = false;
    char16_t mText[96] = {0xFFFF, 0};
    FitBase mFit[4] = {};
    int mFitCount = 0;
};

class MpWindowLayout : public LayoutActor {
public:
    MpWindowLayout() : LayoutActor("MpWindow", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("SysInfoWindowMini", 1);
        MR::createAndAddPaneCtrl(this, "WinInfoWindowM", 1);
        MR::hidePaneRecursive(this, "BG");
        MR::hidePaneRecursive(this, "InfoWindowM");
        MR::hidePaneRecursive(this, "AButtonPosition");
        MR::hidePaneRecursive(this, "SaveIconPosition");
        kill();
    }
    void use(const MpPanel::LayoutUse& u) {
        mWanted = true;
        if (MR::isDead(this) || mClosing) {
            appear();
            MR::startPaneAnim(this, "WinInfoWindowM", "ButtonAppear", 0);
            mClosing = false;
            mWaiting = false;
        }
        if (!mWaiting && MR::isPaneAnimStopped(this, "WinInfoWindowM", 0)) {
            MR::startPaneAnim(this, "WinInfoWindowM", "ButtonWait", 0);
            mWaiting = true;
        }
        for (const char* name : {"WinInfo", "ShaInfo"}) {
            if (nw4r::lyt::Pane* p = MR::getPane(this, name)) {
                p->mSize.width = u.w;
                p->mSize.height = u.h;
            }
        }
        SetRoot(this, u.x, u.y);
        MR::setLayoutAlpha(this, Alpha(u.alpha));
        mLastAlpha = u.alpha;
    }
    void endFrame() {
        // MP3 ingame: a window that faded itself out (a toast, the star card) goes at once. Its ButtonEnd animation would show the
        // frame again, full and empty, for a few frames after its words had gone (the empty bar over the picture).
        if (!mWanted && !MR::isDead(this) && !mClosing && (mLastAlpha < 0.99f || mToast)) {
            kill();
            mWanted = false;
            return;
        }
        if (!mWanted && !MR::isDead(this) && !mClosing) {
            MR::startPaneAnim(this, "WinInfoWindowM", "ButtonEnd", 0);
            mClosing = true;
        }
        if (mClosing && MR::isPaneAnimStopped(this, "WinInfoWindowM", 0)) {
            mClosing = false;
            kill();
        }
        mWanted = false;
    }
    bool mWanted = false, mClosing = false, mWaiting = false;
    float mLastAlpha = 1.0f;   // MP3 ingame
    bool mToast = false;       // MP3 ingame: windows 2.. carry toasts: their words go in one frame, so does the frame
};

// A character's head on a MiiIcon layout: the Character animation (layer 1 of "MarioIcon") picks Mario / Luigi / Toad; Cosmic Mario,
// Cosmic Luigi and Yellow Toad are those heads with their materials tinted (MpPanel::kCharTint).
// Gives a stage-less, indirect-less picture material the TEV swap table (nw4r keeps it only when the layout file asked for it: the
// head's PicMario has none), keeping its texture, coordinate generator, SRT, colour channel and material colour. Material::SetupGX's
// default stage samples with table 0, so (R, R, B, A) there lights the green of whatever is red. False: the material is left as it was.
// uipolish3: it also reserves kToadStages TEV stages (Yellow Toad's red-only swap, SetToadStages): the layout's stage-less default
// (lerp(C0, C1, texture) x colour) is rebuilt by hand around them.
constexpr int kToadStages = 5;
bool EnsureSwapTable(nw4r::lyt::Material* m) {
    if (m->IsTevSwapCap() && m->mGXMemCap.tevStage >= kToadStages) return true;
    const nw4r::lyt::detail::BitGXNums& cap = m->mGXMemCap;
    if (m->IsTevSwapCap() || cap.tevStage || cap.indStage || cap.indSRT || cap.alpComp || cap.blendMode || cap.texMap > 1 || cap.texSRT > 1 || cap.texCoordGen > 1) return false;
    const u8 nTex = m->GetTextureNum(), nGen = cap.texCoordGen ? 1 : 0, nSrt = cap.texSRT;
    nw4r::lyt::TexMap tex;
    nw4r::lyt::TexSRT srt;
    nw4r::lyt::TexCoordGen gen;
    nw4r::lyt::ChanCtrl chan;
    nw4r::ut::Color col = nw4r::ut::Color::WHITE;
    if (nTex) tex = m->GetTexMapAry()[0];
    if (nSrt) srt = m->GetTexSRTAry()[0];
    if (nGen) gen = m->GetTexCoordGenAry()[0];
    const bool hasChan = cap.chanCtrl, hasCol = cap.matCol;
    if (hasChan) chan = m->GetChanCtrlAry()[0];
    if (hasCol) col = m->GetMatColAry()[0];
    m->ReserveGXMem(cap.texMap, nSrt, cap.texCoordGen, kToadStages, true, 0, 0, hasChan, hasCol, false, false);
    if (!m->IsTevSwapCap()) return false;   // (out of layout memory: the head stays plain)
    if (nTex) { m->SetTextureNum(nTex); m->GetTexMapAry()[0].Set(tex); }
    if (nSrt) m->GetTexSRTAry()[0] = srt;
    if (nGen) { m->SetTexCoordGenNum(nGen); m->GetTexCoordGenAry()[0] = gen; }
    if (hasChan) m->GetChanCtrlAry()[0] = chan;
    if (hasCol) m->GetMatColAry()[0] = col;
    return true;
}

// Yellow Toad: the red of the cap's spot reads as yellow, the skin and the white stay. Routing red into green (R, R, B, A) alone turned the
// peach skin yellow too, so the swap is blended in by how red a texel is: a mask from the texel's own green (the spot is pure red, G = 0;
// the skin has G about 0.7, white and the greys have R = G, which the swap leaves alone), clamp((1/2 - G) * 4):
//   0  mask  = clamp((1/2 - TEXC(G, G, G)) * 4)  -> TEV register 2          (swap table 2 = G, G, G, A)
//   1  swapped = TEXC(R, R, B, A)                -> PREV                    (swap table 1)
//   2  PREV = mix(TEXC (R, G, B, A), PREV, mask)                            (swap table 0 = identity)
//   3  PREV = lerp(C0, C1, PREV)                 (the layout's tint stage, as nw4r adds for a stage-less material; ghost wash)
//   4  PREV = PREV x rasterised colour           (the pane's alpha)
// No compare mode is needed; the GX stages and swap tables are what platform/vgx's shader generator already emits.
void SetToadStages(nw4r::lyt::Material* m) {
    using namespace nw4r::lyt;
    m->SetTevSwapMode(0, TevSwapMode(GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA));
    m->SetTevSwapMode(1, TevSwapMode(GX_CH_RED, GX_CH_RED, GX_CH_BLUE, GX_CH_ALPHA));
    m->SetTevSwapMode(2, TevSwapMode(GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA));
    m->SetTevSwapMode(3, TevSwapMode(GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA));
    m->SetTevStageNum(kToadStages);
    TevStage st[kToadStages];
    // 0: the mask
    st[0].SetOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEV_SWAP0, GX_TEV_SWAP2);
    st[0].SetColorIn(GX_CC_TEXC, GX_CC_ZERO, GX_CC_ZERO, GX_CC_KONST);
    st[0].SetColorOp(GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_4, true, GX_TEVREG2, GX_TEV_KCSEL_1_2);
    st[0].SetAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    st[0].SetAlphaOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KASEL_1);
    // 1: the swapped colour
    st[1].SetOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEV_SWAP0, GX_TEV_SWAP1);
    st[1].SetColorIn(GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    st[1].SetColorOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KCSEL_1);
    st[1].SetAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    st[1].SetAlphaOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KASEL_1);
    // 2: the original where the mask is 0, the swapped colour where it is 1
    st[2].SetOrder(GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL, GX_TEV_SWAP0, GX_TEV_SWAP0);
    st[2].SetColorIn(GX_CC_TEXC, GX_CC_CPREV, GX_CC_C2, GX_CC_ZERO);
    st[2].SetColorOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KCSEL_1);
    st[2].SetAlphaIn(GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    st[2].SetAlphaOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KASEL_1);
    // 3: the tint
    st[3].SetOrder(GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL, GX_TEV_SWAP0, GX_TEV_SWAP0);
    st[3].SetColorIn(GX_CC_C0, GX_CC_C1, GX_CC_CPREV, GX_CC_ZERO);
    st[3].SetColorOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KCSEL_1);
    st[3].SetAlphaIn(GX_CA_A0, GX_CA_A1, GX_CA_APREV, GX_CA_ZERO);
    st[3].SetAlphaOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KASEL_1);
    // 4: the rasterised colour (vertex colour: the pane's alpha)
    st[4].SetOrder(GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    st[4].SetColorIn(GX_CC_ZERO, GX_CC_CPREV, GX_CC_RASC, GX_CC_ZERO);
    st[4].SetColorOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KCSEL_1);
    st[4].SetAlphaIn(GX_CA_ZERO, GX_CA_APREV, GX_CA_RASA, GX_CA_ZERO);
    st[4].SetAlphaOp(GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, true, GX_TEVPREV, GX_TEV_KASEL_1);
    for (int i = 0; i < kToadStages; i++) m->SetTevStage(i, st[i]);
}

void SetCharacter(LayoutActor* a, int look, bool ghost = false, bool dark = false) {
    if (look < 0 || look >= MpPanel::kCharacters) look = MP_LOOK_MARIO;
    MR::setPaneAnimFrameAndStop(a, "MarioIcon", MpPanel::kCharacterFrame[look], 1);
    const MpPanel::CharTint& tint = MpPanel::kCharTint[look];
    int paneIndex = -1;
    for (const char* name : {"PicMario", "PicMarioShine"}) {
        ++paneIndex;
        nw4r::lyt::Pane* p = MR::getPane(a, name);
        nw4r::lyt::Material* m = p ? p->GetMaterial() : nullptr;
        if (!m) continue;
        GXColorS10 c0 = {0, 0, 0, 0}, c1 = {255, 255, 255, 255};
        if (tint.on) {
            c0 = {tint.c0[0], tint.c0[1], tint.c0[2], tint.c0[3]};
            c1 = {tint.c1[0], tint.c1[1], tint.c1[2], tint.c1[3]};
        }
        if (ghost) {   // (taken: the ghost wash, over any character's own tint)
            const MpPanel::GhostTint& g = MpPanel::kGhostTint;
            c0 = {g.c0[0], g.c0[1], g.c0[2], g.c0[3]};
            c1 = {g.c1[0], g.c1[1], g.c1[2], g.c1[3]};
        }
        if (dark) {   // (down: the head washed dark grey, over any tint)
            c0 = {0, 0, 0, 0};
            c1 = {110, 114, 130, 255};
        }
        m->SetTevColor(0, c0);
        m->SetTevColor(1, c1);
        // (a head reused for another character: PicMario's own stage-less default again; PicMarioShine's stages are the layout's)
        if (!tint.swapRG && paneIndex == 0 && m->mGXMemNum.tevStage == kToadStages && m->IsTevSwapCap()) m->SetTevStageNum(0);
        // Yellow Toad: the cap's red spot reads as yellow, the skin, white and black stay (SetToadStages; PicMarioShine keeps its own stages)
        if (tint.swapRG && paneIndex == 0 && EnsureSwapTable(m)) SetToadStages(m);
    }
}

// MP3 heads3d: a Cosmic head breathes as the 3D one shimmers (GhostMario.brk: a 180-frame loop of the starry material's brightness):
// the tint's light end swells and fades by about 15 %, every frame the head is shown (two TEV colours; nothing else changes)
uint32_t sShimmerFrame = 0;
void CosmicShimmer(LayoutActor* a, int look) {
    if (look < 0 || look >= MpPanel::kCharacters || !MpPanel::kCharTint[look].on || MpPanel::kCharTint[look].swapRG) return;
    const MpPanel::CharTint& tint = MpPanel::kCharTint[look];
    const float k = 1.0f + 0.15f * sinf((sShimmerFrame % 180) * (2.0f * 3.14159265f / 180.0f));
    GXColorS10 c1;
    c1.r = static_cast<s16>(tint.c1[0] * k > 255.0f ? 255 : tint.c1[0] * k);
    c1.g = static_cast<s16>(tint.c1[1] * k > 255.0f ? 255 : tint.c1[1] * k);
    c1.b = static_cast<s16>(tint.c1[2] * k > 255.0f ? 255 : tint.c1[2] * k);
    c1.a = tint.c1[3];
    if (nw4r::lyt::Pane* p = MR::getPane(a, "PicMario"))
        if (nw4r::lyt::Material* m = p->GetMaterial()) m->SetTevColor(1, c1);
}

void InitCharacterLayout(LayoutActor* a) {
    a->initLayoutManager("MiiIcon", 1);
    MR::createAndAddPaneCtrl(a, "MarioIcon", 3);
    MR::createAndAddPaneCtrl(a, "MiiIcon", 3);
    MR::hidePane(a, "MiiIcon");
    MR::startPaneAnim(a, "MarioIcon", "Character", 1);
    MR::startPaneAnim(a, "MarioIcon", "Hide", 2);
    MR::setPaneAnimFrameAndStop(a, "MarioIcon", 0.0f, 2);
    SetCharacter(a, MP_LOOK_MARIO);
}

// uinative2: MiiIcon's Hide animation (layer 2, stopped on frame 0 but applied on every calcAnim) is a material colour animation:
// it puts PicMario's TEV colours back to the layout's own each frame, so a tint set once never showed in the game (Cosmic Mario
// and Cosmic Luigi looked like Mario and Luigi, the ghost and the party strip's washes did not show). The tint again, after the
// animation, every frame the head is shown: TEV colours 0 and 1 only (the stages SetCharacter chose stay).
void TintHead(LayoutActor* a, int look, bool ghost, bool dark) {
    if (look < 0 || look >= MpPanel::kCharacters) look = MP_LOOK_MARIO;
    const MpPanel::CharTint& tint = MpPanel::kCharTint[look];
    GXColorS10 c0 = {0, 0, 0, 0}, c1 = {255, 255, 255, 255};
    if (tint.on) {
        c0 = {tint.c0[0], tint.c0[1], tint.c0[2], tint.c0[3]};
        c1 = {tint.c1[0], tint.c1[1], tint.c1[2], tint.c1[3]};
    }
    if (ghost) {
        const MpPanel::GhostTint& g = MpPanel::kGhostTint;
        c0 = {g.c0[0], g.c0[1], g.c0[2], g.c0[3]};
        c1 = {g.c1[0], g.c1[1], g.c1[2], g.c1[3]};
    }
    if (dark) c0 = {0, 0, 0, 0}, c1 = {110, 114, 130, 255};
    if (!tint.on && !ghost && !dark) return;   // (a plain head: the layout's own colours are right)
    if (nw4r::lyt::Pane* p = MR::getPane(a, "PicMario"))
        if (nw4r::lyt::Material* m = p->GetMaterial()) {
            m->SetTevColor(0, c0);
            m->SetTevColor(1, c1);
        }
}

class MpHeadLayout : public LayoutActor {
public:
    MpHeadLayout() : LayoutActor("MpHead", true) {}
    virtual void init(const JMapInfoIter&) {
        InitCharacterLayout(this);
        MR::showPane(this, "MarioIcon");
        kill();
    }
    void use(const MpPanel::LayoutUse& u) {
        mWanted = true;
        bool changed = false;
        if (MR::isDead(this)) {
            appear();
            changed = true;
        }
        if (u.state != mLook) {   // (the character, and the washes of the party strip: bubble = pale blue, down = dark)
            mLook = u.state;
            changed = true;
            SetCharacter(this, u.state & MpPanel::kCharLookMask, (u.state & MpPanel::kCharTaken) != 0, (u.state & MpPanel::kHeadDark) != 0);
        }
        if (!(u.state & (MpPanel::kCharTaken | MpPanel::kHeadDark))) CosmicShimmer(this, u.state & MpPanel::kCharLookMask);
        SetRoot(this, u.x, u.y);
        const float w = u.w > 0 ? u.w : 0.36f;
        if (nw4r::lyt::Pane* root = MR::getRootPane(this)) root->mScale.x = root->mScale.y = w;
        MR::setLayoutAlpha(this, Alpha(u.alpha));
        // MP3 lod2: the same head as the last frame (look, place, size, alpha): its animations are stopped frames, so another
        // movement / calcAnim / retint would give the same picture. (3 frames of grace after any change.)
        if (u.x != mX || u.y != mY || w != mW || u.alpha != mAlpha) changed = true;
        mX = u.x; mY = u.y; mW = w; mAlpha = u.alpha;
        mSteady = changed ? 0 : (mSteady < 255 ? mSteady + 1 : 255);
    }
    bool steady() const { return mSteady >= 3 && MpLod::tagsOn() && !MR::isDead(this); }   // MP3 lod2
    void endFrame() {
        if (!mWanted && !MR::isDead(this)) kill();
        mWanted = false;
    }
    void retint() { TintHead(this, mLook & MpPanel::kCharLookMask, (mLook & MpPanel::kCharTaken) != 0, (mLook & MpPanel::kHeadDark) != 0); }
    bool mWanted = false;
    uint8_t mLook = 0;
    uint8_t mSteady = 0;                 // MP3 lod2: frames since anything about this head changed
    float mX = 0, mY = 0, mW = 0, mAlpha = 0;
};

class MpCharLayout : public LayoutActor {
public:
    MpCharLayout() : LayoutActor("MpCharacter", true) {}
    virtual void init(const JMapInfoIter&) {
        InitCharacterLayout(this);
        mCtrl = new ButtonPaneController(this, "MarioIcon", "PicMario", 0, true);
        mCtrl->_22 = false;
        mCtrl->_23 = true;   // (pointed by the menu)
        kill();
    }
    virtual void control() { mCtrl->update(); }
    void use(const MpPanel::LayoutUse& u) {
        mWanted = true;
        if (MR::isDead(this) || mCtrl->isHidden() || mCtrl->isDisappearing()) {
            appear();
            mCtrl->appear();
        }
        const uint8_t look = u.state & (MpPanel::kCharLookMask | MpPanel::kCharTaken);
        if (look != mLook) {
            mLook = look;
            SetCharacter(this, look & MpPanel::kCharLookMask, (look & MpPanel::kCharTaken) != 0);
        }
        if (!(look & MpPanel::kCharTaken)) CosmicShimmer(this, look & MpPanel::kCharLookMask);
        SetRoot(this, u.x, u.y);
        if (nw4r::lyt::Pane* root = MR::getRootPane(this)) root->mScale.x = root->mScale.y = u.w > 0 ? u.w : 1.0f;
        MR::setLayoutAlpha(this, Alpha(u.alpha));   // (a ghost's low, pulsing alpha comes from the composer)
        if (u.state & MpPanel::kCharSelected) mCtrl->onPointing();
        mStill = (u.state & MpPanel::kCharStill) != 0;
    }
    void endFrame(bool draws3D) {
        const bool wanted = mWanted;
        mWanted = false;
        if (!wanted) {
            const bool wasStill = mStill;
            mStill = false;
            if (!MR::isDead(this)) {
                if (draws3D && wasStill) {
                    // Never drawn (3D head owns this slot) and non-interactive: retire at once so allDead()/onlyHudSteady() stay exact.
                    kill();
                } else {
                    if (!mCtrl->isHidden() && !mCtrl->isDisappearing()) mCtrl->disappear();
                    if (mCtrl->isHidden()) kill();
                }
            }
        }
    }
    void retint() { if (mLook != 0xFF) TintHead(this, mLook & MpPanel::kCharLookMask, (mLook & MpPanel::kCharTaken) != 0, false); }
    ButtonPaneController* mCtrl = nullptr;
    bool mWanted = false, mStill = false;
    uint8_t mLook = 0xFF;
};

// uinative: the Star Pointer's hand (DPDPointer's HandPointer, its finger pose: HandType frame 0, as StarPointerLayout's
// startAnimHandFinger), the fingertip at the root, turned so it points at the selected item. Only the hand: no star, no blur.
class MpPointerLayout : public LayoutActor {
public:
    MpPointerLayout() : LayoutActor("MpPointer", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("DPDPointer", 1);
        MR::createAndAddPaneCtrl(this, "HandPointer", 1);
        MR::hidePaneRecursive(this, "StarPointer");
        MR::hidePaneRecursive(this, "HandArrow");
        MR::hidePaneRecursive(this, "PicNozzle");
        MR::showPaneRecursive(this, "HandPointer");
        MR::hidePaneRecursive(this, "PicPlayer2");   // (uinative2: after the show, which brought back 2P's green star over 1P's blue)
        MR::startPaneAnim(this, "HandPointer", "HandType", 0);
        MR::setPaneAnimFrameAndStop(this, "HandPointer", 0.0f, 0);
        kill();
    }
    void use(const MpPanel::LayoutUse& u) {
        mWanted = true;
        if (MR::isDead(this)) {
            appear();
            MR::startPaneAnim(this, "HandPointer", "HandType", 0);
            MR::setPaneAnimFrameAndStop(this, "HandPointer", 0.0f, 0);
        }
        SetRoot(this, u.x, u.y);
        if (nw4r::lyt::Pane* root = MR::getRootPane(this)) {
            root->mScale.x = root->mScale.y = u.w > 0 ? u.w : 1.0f;
            root->mRotate.z = u.h;
        }
        MR::setLayoutAlpha(this, Alpha(u.alpha));
    }
    void endFrame() {
        if (!mWanted && !MR::isDead(this)) kill();
        mWanted = false;
    }
    bool mWanted = false;
};

// uinative: the Mii select's blue arrows (MiiSelect's Arrow pane: PicArrowL / PicArrowR, tips outward) either side of a value.
class MpArrowsLayout : public LayoutActor {
public:
    MpArrowsLayout() : LayoutActor("MpArrows", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("MiiSelect", 1);
        for (const char* n : {"BG", "MiiWindow", "Text", "MiiPosition"}) MR::hidePaneRecursive(this, n);
        if (nw4r::lyt::Pane* a = MR::getPane(this, "Arrow")) a->mTranslate.x = a->mTranslate.y = 0.0f;
        kill();
    }
    void use(const MpPanel::LayoutUse& u) {
        mWanted = true;
        if (MR::isDead(this)) appear();
        SetRoot(this, u.x, u.y);
        for (const char* n : {"Left", "Right"}) {
            if (nw4r::lyt::Pane* p = MR::getPane(this, n)) {
                p->mTranslate.x = n[0] == 'L' ? -u.w : u.w;
                p->mTranslate.y = 0.0f;
                p->mScale.x = p->mScale.y = u.h > 0 ? u.h : 0.4f;
            }
        }
        MR::setLayoutAlpha(this, Alpha(u.alpha));
    }
    void endFrame() {
        if (!mWanted && !MR::isDead(this)) kill();
        mWanted = false;
    }
    bool mWanted = false;
};

// uinative: the star list's page number (WinPage, TxtPage) between its round up / down buttons (AllStarList's Arrow pane), beside
// a list that has pages; a button at the first / last page stays, dimmed (there is nothing that way).
class MpScrollLayout : public LayoutActor {
public:
    MpScrollLayout() : LayoutActor("MpScroll", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("AllStarList", 1);
        for (const char* n : {"AllStarList", "Title", "PicButtonbase", "Capture", "Photo"}) MR::hidePaneRecursive(this, n);
        GrowText(this, "PageNumber");
        for (const char* n : {"NTitle", "Arrow"}) {
            if (nw4r::lyt::Pane* p = MR::getPane(this, n)) p->mAlpha = 255;
        }
        kill();
    }
    void use(const MpPanel::LayoutUse& u, const MpPanel::Scene& sc) {
        mWanted = true;
        if (MR::isDead(this)) {
            appear();
            mText[0] = 0xFFFF;
        }
        const nw4r::lyt::Pane* arrow = MR::getPane(this, "Arrow");
        const float ax = arrow ? arrow->mTranslate.x : 0.0f;
        struct { const char* name; float x, y; bool on; } place[3] = {
            {"ArrowUpButton", u.x - ax, u.y + u.h, (u.state & 1) != 0},
            {"ArrowDownButton", u.x - ax, u.y - u.h, (u.state & 2) != 0},
            {"PageNumber", u.x, u.y, true}};
        for (const auto& pl : place) {
            if (nw4r::lyt::Pane* p = MR::getPane(this, pl.name)) {
                p->mTranslate.x = pl.x, p->mTranslate.y = pl.y;
                p->mAlpha = pl.on ? 255 : 90;
            }
        }
        const char16_t* text = u.text >= 0 ? sc.texts[u.text] : u"";
        if (MpPanel::copyChanged(mText, 16, text)) {
            char16_t buf[32];
            MR::setTextBoxMessageRecursive(this, "PageNumber", BoxText(text, buf, 32));
        }
        MR::setLayoutAlpha(this, Alpha(u.alpha));
    }
    void endFrame() {
        if (!mWanted && !MR::isDead(this)) kill();
        mWanted = false;
    }
    bool mWanted = false;
    char16_t mText[16] = {0xFFFF, 0};
};

// flow look: the screen's one line in the game's own bottom message (the file select's InformationMessage: the InformationWindow
// layout, its upper window, Appear / Line / Wait / End as InformationMessage plays them). Not connected to the scene's decoration:
// drawn by the menu, over its buttons, like its other layouts.
class MpLineLayout : public LayoutActor {
public:
    MpLineLayout() : LayoutActor("MpLine", true) {}
    virtual void init(const JMapInfoIter&) {
        initLayoutManager("InformationWindow", 2);
        // menus5 (owner 12 Nov: "press to start should be centered"): the text is centred in the window. The game's text box is 480
        // wide from 256 left of the window's middle, left-aligned: centred, it is moved right so its middle is the window's middle.
        // The window's text is drawn by a PAIR of boxes: InfConfirmU and its shadow ShaConfirmU (plus the same pair for Japanese,
        // ...UJpJa): every one is centred and moved alike, or the unmoved one shows as a ghost at the old place (B6a).
        static const char* const kBoxes[] = {"InfConfirmU", "ShaConfirmU", "InfConfirmUJpJa", "ShaConfirmUJpJa"};
        for (const char* name : kBoxes) {
            MR::setTextBoxHorizontalPositionCenterRecursive(this, name);
            if (nw4r::lyt::Pane* box = MR::getPane(this, name)) box->mTranslate.x += MpPanel::kLineBoxShift;
        }
        kill();
    }
    void use(const MpPanel::LayoutUse& u, const MpPanel::Scene& sc) {
        mWanted = true;
        if (MR::isDead(this) || mEnding) {
            LayoutActor::appear();
            MR::showPaneRecursive(this, "InfoWindowUp");
            MR::hidePaneRecursive(this, "InfoWindowCenter");
            MR::startAnim(this, "Appear", 0);
            MR::startAnim(this, "Line", 1);
            mEnding = false;
            mWaiting = false;
            mText[0] = 0xFFFF;
        }
        const char16_t* text = u.text >= 0 ? sc.texts[u.text] : u"";
        SetRoot(this, u.x, u.y - 180.0f);   // (the upper window's top edge, 180 up in its layout, at the line's place)
        if (MpPanel::copyChanged(mText, 96, text)) {
            char16_t buf[160], wrapped[96];
            // the bottom message holds two lines (a third runs over the guide): a title and a hint that make three say the hint alone,
            // and a hint that is still too long is left out (menus2: Friend Requests in the first friends run)
            int lines = WrapForBox(this, "InfConfirmU", text, wrapped, 96);
            if (lines > 2) {
                int nl = -1;
                for (int i = 0; text[i] && i < 95; i++)
                    if (text[i] == u'\n') { nl = i; break; }
                if (nl > 0) {
                    lines = WrapForBox(this, "InfConfirmU", text + nl + 1, wrapped, 96);
                    if (lines > 2) {
                        char16_t head[96];
                        for (int i = 0; i < nl; i++) head[i] = text[i];
                        head[nl] = 0;
                        WrapForBox(this, "InfConfirmU", head, wrapped, 96);
                    }
                }
            }
            MR::setTextBoxMessageRecursive(this, nullptr, BoxText(wrapped, buf, 160));
            MR::setAnimFrameAndStopAdjustTextHeight(this, "InfConfirmU", 1);
        }
        MR::setLayoutAlpha(this, Alpha(u.alpha));
    }
    virtual void control() {
        if (mEnding) {
            if (MR::isAnimStopped(this, 0)) kill();
        } else if (!mWaiting && MR::isAnimStopped(this, 0)) {
            MR::startAnim(this, "Wait", 0);
            mWaiting = true;
        }
    }
    void endFrame() {
        if (!mWanted && !MR::isDead(this) && !mEnding) {
            MR::startAnim(this, "End", 0);
            mEnding = true;
        }
        mWanted = false;
    }
    bool mWanted = false, mEnding = false, mWaiting = false;
    char16_t mText[96] = {0xFFFF, 0};
};

MpLineLayout* sLine = nullptr;
MpFrameLayout* sFrame = nullptr;
MpButtonLayout* sButtons[MpPanel::kButtonBanks][MpPanel::kMaxButtons];
int sBank = 0;                 // uinative: the bank the current screen's buttons use
uint16_t sScreenKey = 0xFFFF;
// uinative2: the next screen waits while the screen before's big buttons leave (the chosen one's decide, the others' end), as the
// game's own menus do; on the real screen both at once overlapped (a list window over "Host a Room" flashing). At most kHoldMax frames.
bool sHold = false;
int sHoldFrames = 0;
constexpr int kHoldMax = 45;
MpPointerLayout* sPointer = nullptr;
MpArrowsLayout* sArrows = nullptr;
MpScrollLayout* sScroll = nullptr;
MpWindowLayout* sWindows[MpPanel::kMaxWindows];
MpHeadLayout* sHeads[MpPanel::kMaxHeads];
MpCharLayout* sChars[MpPanel::kMaxChars];

void Run(LayoutActor* a) {
    if (!a || MR::isDead(a)) return;
    a->movement();
    a->calcAnim();
}
void Draw(LayoutActor* a) {
    if (a && !MR::isDead(a)) a->draw();
}

}  // namespace

namespace {
// memfix: the layouts are created per scene (MpUiLayouts.hpp), in two groups. "panel": the frame, the big buttons (two banks), the
// pointer, arrows and scroll furniture, the windows and the heads (the strip's, the toasts', the lobby's): the menus, the session
// menu, the toasts, the party strip. "characters": the character select's big heads (the file select's menu scene only).
// A screen of another branch with layouts of its own adds a group here (registerLayouts) and null-checks its pointers the way the
// characters' are (a group may be absent in a scene that does not want it).
void createPanel() {
    sFrame = new MpFrameLayout();
    sFrame->initWithoutIter();
    for (auto& bank : sButtons)
        for (auto& b : bank) (b = new MpButtonLayout())->initWithoutIter();
    (sPointer = new MpPointerLayout())->initWithoutIter();
    (sArrows = new MpArrowsLayout())->initWithoutIter();
    (sScroll = new MpScrollLayout())->initWithoutIter();
    (sLine = new MpLineLayout())->initWithoutIter();
    for (auto& w : sWindows) (w = new MpWindowLayout())->initWithoutIter();
    for (int i = 2; i < MpPanel::kMaxWindows; i++) sWindows[i]->mToast = true;   // MP3 ingame
    for (auto& h : sHeads) (h = new MpHeadLayout())->initWithoutIter();
}
// settingsnative: the lean set of the native Settings opened from the pause menu of a single-player level (KIND_SETTINGS): the frame, three big buttons
// in each bank (the leave question), the arrows, the page column, the band's line and the windows (dialogs and toasts). No pointer, no heads, no characters:
// the Settings screens draw none of them (every use of those is null-checked below).
void createSettingsPanel() {
    sFrame = new MpFrameLayout();
    sFrame->initWithoutIter();
    for (auto& bank : sButtons)
        for (int i = 0; i < 3; i++) (bank[i] = new MpButtonLayout())->initWithoutIter();
    (sArrows = new MpArrowsLayout())->initWithoutIter();
    (sScroll = new MpScrollLayout())->initWithoutIter();
    (sLine = new MpLineLayout())->initWithoutIter();
    for (auto& w : sWindows) (w = new MpWindowLayout())->initWithoutIter();
    for (int i = 2; i < MpPanel::kMaxWindows; i++) sWindows[i]->mToast = true;
}
// settingsnative: the refusal's toast: the frame and the windows (the toasts' own), nothing else.
void createToastPanel() {
    sFrame = new MpFrameLayout();
    sFrame->initWithoutIter();
    for (auto& w : sWindows) (w = new MpWindowLayout())->initWithoutIter();
    for (int i = 2; i < MpPanel::kMaxWindows; i++) sWindows[i]->mToast = true;
}
void createCharacters() {
    for (auto& c : sChars) (c = new MpCharLayout())->initWithoutIter();
}
// The scene heap takes the memory back with the scene: only the pointers (and the state about them) go.
void forgetPanel() {
    sFrame = nullptr;
    for (auto& bank : sButtons)
        for (auto& b : bank) b = nullptr;
    sPointer = nullptr;
    sArrows = nullptr;
    sScroll = nullptr;
    sLine = nullptr;
    for (auto& w : sWindows) w = nullptr;
    for (auto& h : sHeads) h = nullptr;
    sBank = 0;
    sScreenKey = 0xFFFF;
    sHold = false;
    sHoldFrames = 0;
}
void forgetCharacters() {
    for (auto& c : sChars) c = nullptr;
}

// Budgets for the free-space check (bytes of the scene heap): measured costs with margin (the "ui: layouts" log lines).
constexpr uint32_t kPanelBudget = 1536 * 1024, kCharsBudget = 256 * 1024;
// integ: the LayoutData archives the groups' initLayoutManager calls name (MpFrame PauseMenu, MpButton RetryButton, MpWindow
// SysInfoWindowMini, heads and characters MiiIcon, MpPointer DPDPointer, MpArrows MiiSelect, MpScroll AllStarList, MpLine
// InformationWindow). A session scene makes them on its init thread: they must be mounted by the archive phase (MpUi::addArchives).
const char* const kPanelArchives[] = {"PauseMenu", "RetryButton", "SysInfoWindowMini", "MiiIcon", "DPDPointer", "MiiSelect", "AllStarList", "InformationWindow"};
const char* const kCharsArchives[] = {"MiiIcon"};
const MpUiLayouts::Group kPanelGroup = {"panel", MpUiLayouts::kMenu | MpUiLayouts::kSession, 1 + 10 + 4 + MpPanel::kMaxWindows + MpPanel::kMaxHeads,
                                        kPanelBudget, createPanel, forgetPanel, kPanelArchives, (uint8_t)(sizeof kPanelArchives / sizeof kPanelArchives[0])};
const char* const kSettingsArchives[] = {"PauseMenu", "RetryButton", "SysInfoWindowMini", "MiiSelect", "AllStarList", "InformationWindow"};   // (no MiiIcon, no DPDPointer)
const MpUiLayouts::Group kSettingsGroup = {"settings-panel", MpUiLayouts::kSettings, 1 + 6 + 3 + MpPanel::kMaxWindows, 640 * 1024, createSettingsPanel, forgetPanel,
                                           kSettingsArchives, (uint8_t)(sizeof kSettingsArchives / sizeof kSettingsArchives[0])};
const char* const kToastArchives[] = {"PauseMenu", "SysInfoWindowMini"};
const MpUiLayouts::Group kToastGroup = {"toast-panel", MpUiLayouts::kToast, 1 + MpPanel::kMaxWindows, 256 * 1024, createToastPanel, forgetPanel,
                                        kToastArchives, (uint8_t)(sizeof kToastArchives / sizeof kToastArchives[0])};
const MpUiLayouts::Group kCharsGroup = {"characters", MpUiLayouts::kMenu, MpPanel::kMaxChars, kCharsBudget, createCharacters, forgetCharacters,
                                        kCharsArchives, (uint8_t)(sizeof kCharsArchives / sizeof kCharsArchives[0])};

}  // namespace

void registerLayouts() {
    MpUiLayouts::add(&kPanelGroup);
    MpUiLayouts::add(&kCharsGroup);
    MpUiLayouts::add(&kSettingsGroup);
    MpUiLayouts::add(&kToastGroup);
}

// MP3 heads3d: a character select head to the 3D heads as well (where the layout put it, in square units); the layout's 2D head
// still runs (it is what the star pointer points) but is drawn only where no 3D head is
static void WantHead3D(const MpPanel::LayoutUse& u) {
    const MpPanel::Screen s = screen();
    MpHead3D::Spec sp;
    sp.look = u.state & MpPanel::kCharLookMask;
    sp.x = MpPanel::layoutToSquareX(s, u.x), sp.y = MpPanel::layoutToSquareY(s, u.y3d);
    sp.size = (u.w > 0 ? u.w : 1.0f) * 100.0f;
    sp.selected = (u.state & MpPanel::kCharSelected) != 0;
    sp.owner = u.owner, sp.mine = u.mine;
    sp.restFacing = MpPanel::headRestsFacing(u.state);   // uxfix B3: the gathering's heads face the viewer
    sp.arriving = (u.state & MpPanel::kCharArriving) != 0;   // flow: a player's head that just appeared on the gathering screen pops in
    sp.alpha = u.alpha3d;   // (the screen's fade; the 2D layout's ghost breathing for a taken head is in u.alpha, not wanted here)
    MpHead3D::show(u.slot, sp);
}

void update(const MpPanel::Scene& menu, const MpPanel::Scene& toasts, const MpPanel::Scene& strip) {
    if (!sFrame) return;
    MpHead3D::beginFrame();
    ++sShimmerFrame;
    // uinative: a new screen: its buttons come in the other bank, the old ones end (the chosen one with the game's decide)
    if (menu.screenKey != sScreenKey) {
        if (menu.decided >= 0 && menu.decided < MpPanel::kMaxButtons && sButtons[sBank][menu.decided]) sButtons[sBank][menu.decided]->decideOut();
        sBank ^= 1;
        sScreenKey = menu.screenKey;
        sHoldFrames = 0;
    }
    sHold = false;
    if (sHoldFrames < kHoldMax) {
        for (auto* b : sButtons[sBank ^ 1])
            if (b && !MR::isDead(b)) sHold = true;
        sHoldFrames = sHold ? sHoldFrames + 1 : kHoldMax;
    }
    // MP3 heads: on the character select the game's own Star Pointer (the file select's cursor: it follows the stick, the d-pad or the finger, and
    // points the head under it) is the one cursor; the plan's own hand (a second hand resting on the selected head) shows only where no real
    // pointer is on the screen
    bool charScreen = false;
    for (int i = 0; i < menu.useCount; i++)
        if (menu.uses[i].kind == MpPanel::USE_CHAR) charScreen = true;
    const bool realPointer = charScreen && MR::isStarPointerInScreen(0);
    for (const MpPanel::Scene* sc : {&menu, &toasts, &strip}) {
        for (int i = 0; i < sc->useCount; i++) {
            const MpPanel::LayoutUse& u = sc->uses[i];
            if (sHold && sc == &menu && u.kind != MpPanel::USE_FRAME) continue;   // (uinative2: the next screen waits)
            switch (u.kind) {
            case MpPanel::USE_FRAME: sFrame->use(u, *sc, sHold && sc == &menu); break;
            case MpPanel::USE_BUTTON: if (u.slot < MpPanel::kMaxButtons && sButtons[sBank][u.slot]) sButtons[sBank][u.slot]->use(u, *sc); break;
            case MpPanel::USE_WINDOW: if (u.slot < MpPanel::kMaxWindows && sWindows[u.slot]) sWindows[u.slot]->use(u); break;
            case MpPanel::USE_HEAD: if (u.slot < MpPanel::kMaxHeads && sHeads[u.slot]) sHeads[u.slot]->use(u); break;
            case MpPanel::USE_CHAR:
                if (u.slot < MpPanel::kMaxChars && sChars[u.slot]) {   // memfix: (the characters' group is the menu scene's only)
                    const bool skipDeadStill = MpHead3D::drawsSlot(u.slot) && (u.state & MpPanel::kCharStill) != 0 && MR::isDead(sChars[u.slot]);
                    if (!skipDeadStill) sChars[u.slot]->use(u);
                    WantHead3D(u);
                }
                break;
            case MpPanel::USE_POINTER: if (sPointer && !(realPointer && sc == &menu)) sPointer->use(u); break;
            case MpPanel::USE_ARROWS: if (sArrows) sArrows->use(u); break;
            case MpPanel::USE_SCROLL: if (sScroll) sScroll->use(u, *sc); break;
            case MpPanel::USE_LINE: if (sLine) sLine->use(u, *sc); break;
            default: break;
            }
        }
    }
    MpHead3D::run();
    sFrame->endFrame();
    for (auto& bank : sButtons)
        for (auto* b : bank) if (b) b->endFrame();
    if (sPointer) sPointer->endFrame();
    if (sArrows) sArrows->endFrame();
    if (sScroll) sScroll->endFrame();
    if (sLine) sLine->endFrame();
    for (auto* w : sWindows) if (w) w->endFrame();
    for (auto* h : sHeads) if (h) h->endFrame();
    for (int i = 0; i < MpPanel::kMaxChars; i++)
        if (sChars[i]) sChars[i]->endFrame(MpHead3D::drawsSlot(i));
    Run(sFrame);
    for (auto* w : sWindows) Run(w);
    for (auto& bank : sButtons)
        for (auto* b : bank) Run(b);
    for (auto* h : sHeads) {
        if (!h) continue;
        if (h->steady()) {   // MP3 lod2: nothing changed since a few frames: its last movement / calcAnim / retint still stand
            MpLod::count(MpLod::C_HEAD_STEADY);
        } else {
            if (!MR::isDead(h)) MpLod::count(MpLod::C_HEAD_ACTIVE);
            Run(h);
        }
    }
    for (int i = 0; i < MpPanel::kMaxChars; i++) {
        MpCharLayout* c = sChars[i];
        if (c && !(MpHead3D::drawsSlot(i) && c->mStill && !c->mCtrl->isDisappearing())) Run(c);
    }
    for (auto* h : sHeads) if (h && !MR::isDead(h) && !h->steady()) h->retint();   // uinative2: (after the Hide animation's colours, TintHead)
    for (int i = 0; i < MpPanel::kMaxChars; i++) {
        MpCharLayout* c = sChars[i];
        if (c && !MR::isDead(c) && !MpHead3D::drawsSlot(i)) c->retint();
    }
    Run(sPointer);
    Run(sArrows);
    Run(sScroll);
    Run(sLine);
}

void draw(const MpPanel::PlanBase& menuPlan, const MpPanel::PlanBase& toastPlan, const MpPanel::PlanBase& stripPlan) {
    if (!sFrame) return;
    for (int i = MpPanel::kStripHeadBase; i < MpPanel::kMaxHeads; i++) Draw(sHeads[i]);   // the party strip: heads, then its Power Stars
    DrawPlan(stripPlan);
    Draw(sFrame);
    Draw(sWindows[0]);
    Draw(sWindows[1]);
    for (auto* b : sButtons[sBank ^ 1]) Draw(b);   // (the screen before's, ending, under the new ones)
    for (auto* b : sButtons[sBank]) Draw(b);
    MpHead3D::draw();   // MP3 heads3d: the character select's heads in 3D where the file select's models are at hand
    for (int i = 0; i < MpPanel::kMaxChars; i++)
        if (!MpHead3D::drawsSlot(i)) Draw(sChars[i]);
    if (!sHold) DrawPlan(menuPlan);
    Draw(sArrows);
    Draw(sScroll);
    Draw(sLine);
    for (int i = 0; i < MpPanel::kToastHeadBase; i++) Draw(sHeads[i]);
    Draw(sPointer);
    for (int i = 2; i < MpPanel::kMaxWindows; i++) Draw(sWindows[i]);
    DrawPlan(toastPlan);
    for (int i = MpPanel::kToastHeadBase; i < MpPanel::kStripHeadBase; i++) Draw(sHeads[i]);
}

bool allDead() {
    if (!sFrame) return true;
    if (MpHead3D::anyShown()) return false;   // MP3 heads3d: (shrinking away)
    if (!MR::isDead(sFrame)) return false;
    for (auto& bank : sButtons)
        for (auto* b : bank) if (b && !MR::isDead(b)) return false;
    for (LayoutActor* a : {static_cast<LayoutActor*>(sPointer), static_cast<LayoutActor*>(sArrows), static_cast<LayoutActor*>(sScroll), static_cast<LayoutActor*>(sLine)})
        if (a && !MR::isDead(a)) return false;
    for (auto* w : sWindows) if (w && !MR::isDead(w)) return false;
    for (auto* h : sHeads) if (h && !MR::isDead(h)) return false;
    for (auto* c : sChars) if (c && !MR::isDead(c)) return false;
    return true;
}

void tickSkipped() {
    if (sFrame) ++sShimmerFrame;
}

bool onlyHudSteady() {
    if (!sFrame) return true;
    if (sHold || MpHead3D::anyShown()) return false;
    if (!MR::isDead(sFrame)) return false;
    for (auto& bank : sButtons)
        for (auto* b : bank) if (b && !MR::isDead(b)) return false;
    for (LayoutActor* a : {static_cast<LayoutActor*>(sPointer), static_cast<LayoutActor*>(sArrows), static_cast<LayoutActor*>(sScroll), static_cast<LayoutActor*>(sLine)})
        if (a && !MR::isDead(a)) return false;
    for (auto* w : sWindows) if (w && !MR::isDead(w)) return false;
    for (auto* c : sChars) if (c && !MR::isDead(c)) return false;
    for (auto* h : sHeads) {
        if (!h || MR::isDead(h)) continue;
        if (!h->steady()) return false;
    }
    return true;
}

int pointedButton() {
    for (int i = 0; i < MpPanel::kMaxChars; i++) {
        MpCharLayout* c = sChars[i];
        if (c && !MR::isDead(c) && !c->mStill && !c->mCtrl->isHidden() && c->mCtrl->isPointingPaneDirect()) return i;
    }
    for (int i = 0; i < MpPanel::kMaxButtons; i++) {
        MpButtonLayout* b = sButtons[sBank][i];
        if (b && !MR::isDead(b) && !b->mCtrl->isHidden() && b->mCtrl->isPointingPaneDirect()) return i;
    }
    return -1;
}

}  // namespace MpPanelDraw
