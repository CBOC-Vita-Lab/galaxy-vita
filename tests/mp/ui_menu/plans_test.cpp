// memfix: the UI plans' capacities against the biggest real screens (family: plans; no screen of flow's files is touched).
#include "common.hpp"

// memfix: the three UI plans' capacities against the biggest real screens. The compose functions record the highest use of each plan
// kind (planPeak); every screen / overlay / toast / ring / strip of the tests above ran before this. Margin: capacity >= 1.25 x peak.
// Also the static size of the three plans together (they sit in .bss for the whole run).
void testPlanCapacity() {
    using namespace MpPanel;
    FontMetrics fm[MpPanel::FONT_COUNT];
    for (auto& f : fm) f.height = 33.0f, f.baseline = 26.0f, f.advance = Adv, f.has = HasG;
    static ToastPlan toastPlan;
    static Scene scene;
    Screen scr;
    // the worst toasts: three, longest text, an icon each; then the ring with the longest labels, one picked
    Toast ts[kMaxToasts];
    for (auto& t : ts) {
        for (int i = 0; i < 95; i++) t.text[i] = static_cast<char16_t>(u'W');
        t.text[95] = 0;
        t.alpha = 1.0f;
        t.icon = ICON_MARIO;
    }
    beginScene(&scene);
    begin(&toastPlan);
    composeToasts(ts, kMaxToasts, scr, fm, &scene, &toastPlan);
    Ring ring;
    for (auto& l : ring.label) {
        for (int i = 0; i < kCellText - 1; i++) l[i] = u'W';
        l[kCellText - 1] = 0;
    }
    ring.picked = 1;
    ring.alpha = 1.0f;
    ring.pop = 1.0f;
    composeRing(ring, scr, fm, &toastPlan);
    CHECK(!toastPlan.overflow);
    Strip st;
    st.count = kMaxStrip;
    st.appear = 1.0f;
    for (int i = 0; i < kMaxStrip; i++) st.m[i] = {static_cast<uint8_t>(i + 1), PARTY_NORMAL, 0.3f};
    beginScene(&scene);
    begin(&toastPlan);
    composeStrip(st, scr, fm, 0.25f, &scene, &toastPlan);
    CHECK(!toastPlan.overflow);
    // merge1: the strip's plan also takes menuflow's in-play models (edge heads, the watch badge, the star row, the bubble prompt):
    // the worst frame holds all of them at once
    {
        static StripPlan stripPlan;
        beginScene(&scene);
        begin(&stripPlan);
        composeStrip(st, scr, fm, 0.25f, &scene, &stripPlan);
        Edge edge;
        edge.count = kMaxEdges;
        for (int i = 0; i < kMaxEdges; i++) {
            edge.e[i].look = static_cast<uint8_t>(i % kCharacters);
            edge.e[i].x = 100.0f + 50.0f * i, edge.e[i].y = 80.0f, edge.e[i].ax = 1.0f, edge.e[i].ay = 0.0f, edge.e[i].alpha = 1.0f;
        }
        composeEdges(edge, scr, &scene, &stripPlan);
        Badge badge = {1, 1.0f, true};
        composeBadge(badge, scr, fm, &scene, &stripPlan);
        StarRow row;
        row.count = kStarRowMax;
        row.alpha = 1.0f;
        row.by = 0;
        for (int i = 0; i < kStarRowMax; i++) row.look[i] = static_cast<uint8_t>(i % kCharacters);
        composeStarRow(row, scr, fm, &scene, &stripPlan);
        Prompt pr;
        pr.x = 400.0f, pr.y = 300.0f, pr.alpha = 1.0f;
        composePrompt(pr, scr, fm, &stripPlan);
        fprintf(stderr, "  strip plan worst frame: vertices %d / %d runs %d / %d text %d / %d\n", stripPlan.vertexCount, stripPlan.maxVertices,
                stripPlan.runCount, stripPlan.maxRuns, stripPlan.textCount, stripPlan.maxText);
        CHECK(!stripPlan.overflow);
    }
    const struct { const char* name; PlanKind kind; int v, r, t; } caps[] = {
        {"menu", PLAN_MENU, kMaxVertices, kMaxRuns, kMaxText},
        {"toast", PLAN_TOAST, kToastMaxVertices, kToastMaxRuns, kToastMaxText},
        {"strip", PLAN_STRIP, kStripMaxVertices, kStripMaxRuns, kStripMaxText},
    };
    for (const auto& c : caps) {
        const PlanPeak p = planPeak(c.kind);
        fprintf(stderr, "  plan %-5s peak vertices %5d / %5d  runs %3d / %3d  text %4d / %4d\n", c.name, p.vertices, c.v, p.runs, c.r, p.text, c.t);
        CHECK(p.vertices * 5 <= c.v * 4);   // (peak <= 80 % of the capacity; the strip has no shapes yet: 0 is fine)
        CHECK(p.runs > 0 && p.runs * 5 <= c.r * 4);
        CHECK(p.text > 0 && p.text * 5 <= c.t * 4);
    }
    const size_t statics = sizeof(Plan) + sizeof(ToastPlan) + sizeof(StripPlan);
    fprintf(stderr, "  plans in .bss: %zu bytes (was 3 x 161,060 = 483,180)\n", statics);
    CHECK(statics < 120 * 1024);   // (merge1: the strip plan grew for the in-play models)
}


