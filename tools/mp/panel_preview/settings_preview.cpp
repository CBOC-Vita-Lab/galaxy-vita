// Host side of render.py for the native Settings mocks (words agent, 14 Nov): composes the Settings screens as the menu will build them (big buttons
// "Label: Value", the selected pill's sentence in the game's bottom message) with the real composer (MpPanelPlan.cpp) and the real words (MpMessages*.cpp),
// and prints the scene like preview.cpp. It needs no menu state machine, so it links on its own.
//   settings_preview <metrics-file> <screen> <language prefix>     screens: settings-mock, settings-mock-controls, settings-mock-more, leave-mock
//   PREVIEW_CURSOR=n picks the selected pill; PREVIEW_ASPECT=43 gives the 4:3 picture.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpPanelPlan.hpp"

namespace {
struct FontFile { std::map<int, float> advance; };
FontFile gFonts[2];
float Advance(void* c, uint16_t code) {
    auto* f = static_cast<FontFile*>(c);
    auto it = f->advance.find(code);
    return it == f->advance.end() ? 0.0f : it->second;
}
bool Has(void* c, uint16_t code) { return static_cast<FontFile*>(c)->advance.count(code) != 0; }
}  // namespace

void printScene(const char* tag, const MpPanel::Scene& sc, const MpPanel::PlanBase& plan) {
    printf("%s\n", tag);
    for (int i = 0; i < sc.textCount; i++) {
        printf("T %d", i);
        for (const char16_t* p = sc.texts[i]; *p; ++p) printf(" %d", *p);
        printf("\n");
    }
    for (int i = 0; i < sc.useCount; i++) {
        const MpPanel::LayoutUse& u = sc.uses[i];
        printf("U %d %d %d %d %d %.3f %.3f %.3f %.3f %.3f\n", u.kind, u.slot, u.state, u.text, u.text2, u.x, u.y, u.w, u.h, u.alpha);
    }
    for (int i = 0; i < plan.vertexCount; i++) {
        const auto& v = plan.vertices[i];
        printf("V %.3f %.3f %d %d %d %d\n", v.x, v.y, v.c.r, v.c.g, v.c.b, v.c.a);
    }
    for (int i = 0; i < plan.runCount; i++) {
        const auto& r = plan.runs[i];
        printf("R %d %.3f %.3f %.5f %.5f %d %d %d %d", r.font, r.x, r.y, r.scaleX, r.scaleY, r.color.r, r.color.g, r.color.b, r.color.a);
        for (int k = 0; k < r.length; k++) printf(" %d", plan.text[r.start + k]);
        printf("\n");
    }
    printf("E %d %d\n", plan.overflow ? 1 : 0, plan.textClipped ? 1 : 0);
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    MpPanel::FontMetrics metrics[MpPanel::FONT_COUNT];
    FILE* f = fopen(argv[1], "r");
    if (!f) return 3;
    char kind;
    int font;
    while (fscanf(f, " %c %d", &kind, &font) == 2) {
        if (kind == 'H') {
            float h, b;
            if (fscanf(f, "%f %f", &h, &b) != 2) return 4;
            metrics[font].height = h, metrics[font].baseline = b;
        } else {
            int code;
            float a;
            if (fscanf(f, "%d %f", &code, &a) != 2) return 4;
            gFonts[font].advance[code] = a;
        }
    }
    fclose(f);
    for (int i = 0; i < 2; i++) metrics[i].context = &gFonts[i], metrics[i].advance = Advance, metrics[i].has = Has;
    MpMsgText::setLanguage(MpMsgText::languageIndex(argv[3]));
    {
        // native Settings (v3): the screens as the menu will build them. Pill screens (KIND_BUTTONS): big buttons "Label: Value". Value rows (KIND_LIST,
        // the list window under the game's bottom message): the name and its value with arrows, three rows a page. The selected row's description is the
        // bottom message (GameLook). PREVIEW_CURSOR picks the selected row; PREVIEW_ASPECT=43 gives the 4:3 picture.
        using namespace MpMsgText;
        MpPanel::Screen screen;
        if (const char* a = getenv("PREVIEW_ASPECT"); a && strcmp(a, "43") == 0) screen.width = 608.0f;
        static MpPanel::Model model;
        static MpPanel::Plan plan;
        static MpPanel::Scene scene;
        MpPanel::clear(&model);
        model.look = MpPanel::LOOK_GAME;
        const std::string which = argv[2];
        struct Row { MpText label, value, desc; const char* num; bool arrows; };
        std::vector<Row> rows;
        bool pills = false;
        int pages = 0, page = 0;
        if (which == "settings-mock") {
            pills = true;
            rows = {{MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH, MP_T_SET_D_PICTURE_SMOOTH, nullptr, false}, {MP_T_SET_L_CAMERA, MP_T_NONE, MP_T_SET_D_CAMERA, nullptr, false},
                    {MP_T_SET_L_CONTROLS, MP_T_NONE, MP_T_SET_D_CONTROLS, nullptr, false}, {MP_T_SET_L_MORE, MP_T_NONE, MP_T_SET_D_MORE, nullptr, false}};
        } else if (which == "settings-mock-main" || which == "settings-mock-main-off") {   // setnat31: the list as the menu builds it now, with 60 Assist under Picture
            const bool off = which == "settings-mock-main-off";
            rows = {{MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH, MP_T_SET_D_PICTURE_SMOOTH, nullptr, true}, {MP_T_SET_L_ASSIST, off ? MP_T_OFF : MP_T_ON, off ? MP_T_SET_D_ASSIST_OFF : MP_T_SET_D_ASSIST_ON, nullptr, true},
                    {MP_T_SET_L_CAMERA, MP_T_NONE, MP_T_SET_D_CAMERA, nullptr, false}, {MP_T_SET_L_CONTROLS, MP_T_NONE, MP_T_SET_D_CONTROLS, nullptr, false}, {MP_T_SET_L_MORE, MP_T_NONE, MP_T_SET_D_MORE, nullptr, false}};
        } else if (which == "settings-mock-main-pause") {   // setnat31: from the pause menu, 60 Assist changed: the band says it starts the next time
            rows = {{MP_T_SET_L_PICTURE, MP_T_SET_V_SMOOTH, MP_T_SET_D_PICTURE_SMOOTH, nullptr, true}, {MP_T_SET_L_ASSIST, MP_T_OFF, MP_T_SET_D_RESTART_NEXT, nullptr, true},
                    {MP_T_SET_L_CAMERA, MP_T_NONE, MP_T_SET_D_CAMERA, nullptr, false}, {MP_T_SET_L_CONTROLS, MP_T_NONE, MP_T_SET_D_CONTROLS, nullptr, false}, {MP_T_SET_L_MORE, MP_T_NONE, MP_T_SET_D_MORE, nullptr, false}};
        } else if (which == "leave-mock" || which == "leave-mock-restart") {
            pills = true;
            rows = {{which == "leave-mock" ? MP_T_SET_SAVE : MP_T_SET_SAVE_RESTART, MP_T_NONE, MP_T_SET_LEAVE_Q, nullptr, false}, {MP_T_SET_DONT_SAVE, MP_T_NONE, MP_T_SET_LEAVE_Q, nullptr, false},
                    {MP_T_SET_KEEP_EDITING, MP_T_NONE, MP_T_SET_LEAVE_Q, nullptr, false}};
        } else if (which == "settings-mock-camera") {
            rows = {{MP_T_SET_L_ZOOM, MP_T_NONE, MP_T_SET_D_ZOOM, "1.5", true}, {MP_T_SET_L_RSTICK, MP_T_SET_V_CAMERA, MP_T_SET_D_RSTICK_CAMERA, nullptr, true},
                    {MP_T_SET_L_INVERT, MP_T_OFF, MP_T_SET_D_INVERT_OFF, nullptr, true}};
        } else if (which == "settings-mock-controls") {
            pages = 2;
            rows = {{MP_T_SET_L_TILT, MP_T_SET_V_LEFT_STICK, MP_T_SET_D_TILT_STICK, nullptr, true}, {MP_T_SET_L_TILT_POWER, MP_T_NONE, MP_T_SET_D_TILT_POWER, "10 (Normal)", true},
                    {MP_T_SET_L_POINTER, MP_T_SET_V_QUICK, MP_T_SET_D_POINTER_QUICK, nullptr, true}};
        } else if (which == "settings-mock-controls2") {
            pages = 2, page = 1;
            rows = {{MP_T_SET_L_REAR, MP_T_OFF, MP_T_SET_D_REAR_OFF, nullptr, true}, {MP_T_SET_L_TAPJUMP, MP_T_ON, MP_T_SET_D_TAPJUMP_ON, nullptr, true},
                    {MP_T_SET_L_BUTTONS, MP_T_NONE, MP_T_SET_D_BUTTONS, nullptr, false}};
        } else if (which == "settings-mock-more") {
            pages = 2;
            rows = {{MP_T_SET_L_LANGUAGE, MP_T_SET_V_SYSTEM_LANG, MP_T_SET_D_LANG_OWN, nullptr, true}, {MP_T_SET_L_MOTION, MP_T_ON, MP_T_SET_D_MOTION_ON, nullptr, true},
                    {MP_T_SET_L_REACH, MP_T_NONE, MP_T_SET_D_REACH, "1", true}};
        } else if (which == "settings-mock-more2") {
            pages = 2, page = 1;
            rows = {{MP_T_SET_L_RESET, MP_T_NONE, MP_T_SET_D_RESET, nullptr, false}};
        } else if (which == "settings-mock-buttons" || which == "settings-mock-buttons2") {
            pages = 4, page = which == "settings-mock-buttons" ? 0 : 1;
            if (page == 0) rows = {{MP_T_SET_M_JUMP, MP_T_NONE, MP_T_SET_MD_JUMP, "", false}, {MP_T_SET_M_STAR_BITS, MP_T_NONE, MP_T_SET_MD_STAR_BITS, "R2", false},
                                   {MP_T_SET_M_SPIN, MP_T_NONE, MP_T_SET_MD_SPIN, "", false}};
            else rows = {{MP_T_SET_M_SPIN2, MP_T_SET_V_NONE, MP_T_SET_MD_SPIN2, nullptr, false}, {MP_T_SET_M_CAMERA, MP_T_NONE, MP_T_SET_MD_CAMERA, "", false},
                         {MP_T_SET_M_CAM_RIGHT, MP_T_NONE, MP_T_SET_MD_CAM_RIGHT, "D-pad Right", false}};
        } else {
            return 6;
        }
        model.kind = pills ? MpPanel::KIND_BUTTONS : MpPanel::KIND_LIST;
        model.rowCount = static_cast<int>(rows.size());
        for (int i = 0; i < model.rowCount; i++) {
            MpPanel::Row& r = model.rows[i];
            memset(&r, 0, sizeof r);
            MpMsgText::format(r.label, MpPanel::kLabel, rows[i].label, Args());
            if (rows[i].num) MpMsgText::utf8To16(r.value, MpPanel::kValue, rows[i].num);
            else if (rows[i].value != MP_T_NONE) MpMsgText::format(r.value, MpPanel::kValue, rows[i].value, Args());
            r.ping = -1, r.owner = -1;
            if (rows[i].arrows) r.marks |= MpPanel::MARK_ARROWS;
            else if (!pills && rows[i].value == MP_T_NONE && !rows[i].num && which.rfind("settings-mock-main", 0) == 0) r.marks |= MpPanel::MARK_NAV;   // (a page: Camera, Controls, More)
        }
        int cur = 0;
        if (const char* c = getenv("PREVIEW_CURSOR")) cur = atoi(c);
        if (cur < 0 || cur >= model.rowCount) cur = 0;
        model.cursor = cur;
        model.page = page, model.pages = pages;
        { Args a; a.str = rows[cur].desc == MP_T_SET_D_LANG_OWN ? "Fran\xc3\xa7" "ais" : (rows[cur].desc == MP_T_SET_D_CAPTURE ? "Jump" : nullptr); MpMsgText::format(model.line, 96, rows[cur].desc, a); }
        model.buttonCount = 2;
        model.buttons[0].pic = MpMsgText::PIC_CROSS;
        MpMsgText::format(model.buttons[0].label, 20, which.rfind("settings-mock-buttons", 0) == 0 ? MP_T_SET_L_BUTTONS : MP_T_SELECT, Args());
        model.buttons[1].pic = MpMsgText::PIC_CIRCLE;
        MpMsgText::format(model.buttons[1].label, 20, MP_T_BACK, Args());
        printf("S %f %f %s \n", screen.width, screen.height, argv[2]);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        MpPanel::compose(model, screen, metrics, 0.0f, &scene, &plan);
        printScene("MENU", scene, plan);
        MpPanel::beginScene(&scene);
        MpPanel::begin(&plan);
        printScene("TOAST", scene, plan);
        printScene("STRIP", scene, plan);
        return 0;
    }
}
