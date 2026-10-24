// Host side of tools/settings_panel_preview/render.py: builds the settings panel's plan (platform/src/port/settings_panel_plan.cpp,
// the same code the Vita runs) for one state and prints it as text. Font metrics come from a file render.py writes from the
// game's own fonts (local data, never part of the tree).
//   preview <metrics-file> <zoom tenths> <rear 0/1> <knob> <pulse> <alpha> <compact 0/1> <language prefix, e.g. UsEnglish>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "../../platform/src/port/settings_panel_plan.h"

namespace {
struct FontFile {
    std::map<int, float> advance;
};
FontFile gFonts[2];
float Advance(void* c, uint16_t code) {
    auto* f = static_cast<FontFile*>(c);
    auto it = f->advance.find(code);
    return it == f->advance.end() ? 0.0f : it->second;
}
bool Has(void* c, uint16_t code) { return static_cast<FontFile*>(c)->advance.count(code) != 0; }
}  // namespace

int main(int argc, char** argv) {
    if (argc < 9) return 2;
    settings_panel::FontMetrics metrics[2];
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
    settings_panel::State st;
    st.zoomTenths = atoi(argv[2]);
    st.rearAim = atoi(argv[3]) != 0;
    st.knob = static_cast<float>(atof(argv[4]));
    st.pulse = static_cast<float>(atof(argv[5]));
    st.alpha = static_cast<float>(atof(argv[6]));
    st.compact = atoi(argv[7]) != 0;
    st.language = settings_panel::LanguageIndex(argv[8]);
    static settings_panel::Plan plan;
    settings_panel::Build(st, metrics, &plan);
    printf("P %f %f %f %f %f %f %d %d %s\n", st.width, st.height, plan.x0, plan.y0, plan.x1, plan.y1, plan.vertexCount, plan.overflow ? 1 : 0,
           settings_panel::LanguageName(st.language));
    for (int i = 0; i < plan.vertexCount; i++) {
        const auto& v = plan.vertices[i];
        printf("V %.3f %.3f %d %d %d %d\n", v.x, v.y, v.c.r, v.c.g, v.c.b, v.c.a);
    }
    for (int i = 0; i < plan.runCount; i++) {
        const auto& r = plan.runs[i];
        printf("R %d %.3f %.3f %.5f %.5f %d %d %d %d", r.font, r.x, r.y, r.scaleX, r.scaleY, r.color.r, r.color.g, r.color.b, r.color.a);
        for (int k = 0; k < r.length; k++) printf(" %d", r.text[k]);
        printf("\n");
    }
    return 0;
}
