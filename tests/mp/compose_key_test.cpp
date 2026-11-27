// optfix item 3: the in-play panels' composition key (MpComposeKey.hpp). Soundness: whenever two frames have the same key, composing them
// gives the same Scene and Plan (so keeping the last composition is exact); any model change gives another key; the strip's bob phase counts
// only while a head uses it (so a still strip is not recomposed every frame).   bash tests/mp/compose_key_build.sh
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Game/MP/MpComposeKey.hpp"
#include "Game/MP/MpPanelPlan.hpp"

static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace MpPanel;

static float adv(void*, uint16_t) { return 10.0f; }
static bool has(void*, uint16_t) { return true; }

struct Frame {
    Strip strip; Edge edge; Badge badge; StarRow row; Prompt prompt;
    bool stripOn = false, edgeOn = false, badgeOn = false, rowOn = false, promptOn = false;
    uint32_t frame = 0;
    Screen screen;
};

static uint64_t keyOf(const Frame& f) {
    MpComposeKey::Inputs in;
    in.stripOn = f.stripOn; in.edgeOn = f.edgeOn; in.badgeOn = f.badgeOn; in.starRowOn = f.rowOn; in.promptOn = f.promptOn;
    in.strip = &f.strip; in.edge = &f.edge; in.badge = &f.badge; in.starRow = &f.row; in.prompt = &f.prompt;
    in.screen = f.screen; in.frame = f.frame;
    return MpComposeKey::key(in);
}

struct Out {
    Scene* scene; StripPlan* plan;
    Out() : scene(new Scene()), plan(new StripPlan()) {}
    ~Out() { delete scene; delete plan; }
};
static void compose(const Frame& f, Out* o) {
    beginScene(o->scene);
    begin(o->plan);
    FontMetrics fonts[FONT_COUNT];
    for (auto& m : fonts) { m.height = 20; m.baseline = 16; m.advance = adv; m.has = has; }
    const float bob = (f.frame % 180) / 180.0f;
    if (f.stripOn) composeStrip(f.strip, f.screen, fonts, bob, o->scene, o->plan);
    if (f.edgeOn) composeEdges(f.edge, f.screen, o->scene, o->plan);
    if (f.badgeOn) composeBadge(f.badge, f.screen, fonts, o->scene, o->plan);
    if (f.rowOn) composeStarRow(f.row, f.screen, fonts, o->scene, o->plan);
    if (f.promptOn) composePrompt(f.prompt, f.screen, fonts, o->plan);
}
static bool same(const Out& a, const Out& b) {
    if (a.scene->useCount != b.scene->useCount || a.scene->textCount != b.scene->textCount || a.scene->headCount != b.scene->headCount) return false;
    if (memcmp(a.scene->uses, b.scene->uses, sizeof(LayoutUse) * a.scene->useCount) != 0) {
        for (int i = 0; i < a.scene->useCount; i++) {   // (field-wise: LayoutUse has padding)
            const LayoutUse &x = a.scene->uses[i], &y = b.scene->uses[i];
            if (x.kind != y.kind || x.slot != y.slot || x.state != y.state || x.text != y.text || x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h ||
                x.alpha != y.alpha || x.owner != y.owner || x.mine != y.mine || x.alpha3d != y.alpha3d || x.y3d != y.y3d) return false;
        }
    }
    if (a.plan->vertexCount != b.plan->vertexCount || a.plan->runCount != b.plan->runCount) return false;
    for (int i = 0; i < a.plan->vertexCount; i++) {
        const Vertex &x = a.plan->vertices[i], &y = b.plan->vertices[i];
        if (x.x != y.x || x.y != y.y) return false;
    }
    return true;
}

static uint32_t rnd(uint32_t* s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

int main() {
    Frame base;
    base.stripOn = base.edgeOn = base.badgeOn = base.rowOn = base.promptOn = true;
    base.strip.count = 2;
    base.strip.m[0] = StripMember{1, PARTY_NORMAL, -1.0f};
    base.strip.m[1] = StripMember{2, PARTY_QUIET, -1.0f};
    base.strip.appear = 1.0f; base.strip.counterX = -200.0f; base.strip.counterScale = 1.0f;
    base.edge.count = 1; base.edge.e[0] = EdgeHead{1, 100, 50, 0.5f, 0.5f, 1.0f};
    base.badge = Badge{2, 1.0f, true};
    base.row.count = 2; base.row.look[0] = 0; base.row.look[1] = 1; base.row.by = 1; base.row.alpha = 0.7f;
    base.prompt = Prompt{300, 200, 1.0f};
    const uint64_t k0 = keyOf(base);
    CHECK(keyOf(base) == k0);

    // the frame counter does not matter while no head uses the bob (a still strip is not recomposed every frame)
    Frame f = base;
    f.frame = 77;
    CHECK(keyOf(f) == k0);
    // ... and it does while a head floats or a link blinks
    Frame bub = base;
    bub.strip.m[1].state = PARTY_BUBBLE;
    const uint64_t kb = keyOf(bub);
    bub.frame = 1;
    CHECK(keyOf(bub) != kb);
    Frame lost = base;
    lost.strip.m[0].link = LINK_LOST;
    const uint64_t kl = keyOf(lost);
    lost.frame = 5;
    CHECK(keyOf(lost) != kl);

    // every field the composers read changes the key
    #define CHANGES(expr) do { Frame g = base; expr; CHECK(keyOf(g) != k0); } while (0)
    CHANGES(g.strip.count = 3);
    CHANGES(g.strip.m[0].look = 0);
    CHANGES(g.strip.m[0].state = PARTY_DOWN);
    CHANGES(g.strip.m[1].flash = 0.3f);
    CHANGES(g.strip.m[1].link = LINK_WEAK);
    CHANGES(g.strip.m[0].away = true);
    CHANGES(g.strip.m[0].enter = 0.5f);
    CHANGES(g.strip.m[0].pos = 1.5f);
    CHANGES(g.strip.appear = 0.5f);
    CHANGES(g.strip.counterX = -190.0f);
    CHANGES(g.strip.counterY = 3.0f);
    CHANGES(g.strip.counterScale = 1.1f);
    CHANGES(g.strip.localLook = 3);
    CHANGES(g.edge.count = 0);
    CHANGES(g.edge.e[0].x = 101);
    CHANGES(g.edge.e[0].ay = 0.4f);
    CHANGES(g.edge.e[0].alpha = 0.9f);
    CHANGES(g.edge.e[0].look = 2);
    CHANGES(g.badge.look = 3);
    CHANGES(g.badge.alpha = 0.5f);
    CHANGES(g.badge.canSwitch = false);
    CHANGES(g.row.count = 3);
    CHANGES(g.row.look[1] = 3);
    CHANGES(g.row.by = 0);
    CHANGES(g.row.alpha = 0.6f);
    CHANGES(g.prompt.x = 301);
    CHANGES(g.prompt.alpha = 0.5f);
    CHANGES(g.screen.width = 608.0f);
    CHANGES(g.stripOn = false);
    CHANGES(g.promptOn = false);
    // data beyond the count is not part of the key
    { Frame g = base; g.strip.m[2].look = 3; g.edge.e[2].x = 9; g.row.look[3] = 2; CHECK(keyOf(g) == k0); }

    // soundness: random frame pairs, same key => same composition
    uint32_t seed = 12345;
    int sameKeys = 0, total = 0;
    Frame prev = base;
    Out a, b;
    for (int i = 0; i < 4000; i++) {
        Frame g = prev;
        const uint32_t r = rnd(&seed);
        switch (r % 14) {
        case 0: g.strip.m[0].state = (uint8_t)(rnd(&seed) % 4); break;
        case 1: g.strip.m[1].link = (uint8_t)(rnd(&seed) % 3); break;
        case 2: g.strip.count = (int)(rnd(&seed) % 4); break;
        case 3: g.strip.appear = (rnd(&seed) % 3) * 0.5f; break;
        case 4: g.edge.count = (int)(rnd(&seed) % 4); g.edge.e[0].x = (float)(rnd(&seed) % 3) * 50; break;
        case 5: g.badgeOn = rnd(&seed) & 1; break;
        case 6: g.row.count = (int)(rnd(&seed) % 5); g.row.by = (int8_t)((int)(rnd(&seed) % 5) - 1); break;
        case 7: g.promptOn = rnd(&seed) & 1; g.prompt.x = (float)(rnd(&seed) % 3) * 10; break;
        case 8: g.frame = rnd(&seed) % 400; break;
        case 9: g.stripOn = rnd(&seed) & 1; break;
        case 10: g.strip.m[0].flash = (rnd(&seed) % 4) == 0 ? 0.4f : -1.0f; break;
        case 11: g.strip.m[1].away = rnd(&seed) & 1; break;
        case 12: g.edgeOn = rnd(&seed) & 1; break;
        default: break;   // no change at all: the common case
        }
        if (g.strip.count > kMaxStrip) g.strip.count = kMaxStrip;
        if (g.edge.count > kMaxEdges) g.edge.count = kMaxEdges;
        if (g.row.count > kStarRowMax) g.row.count = kStarRowMax;
        if (g.row.by >= g.row.count) g.row.by = -1;
        total++;
        if (keyOf(g) == keyOf(prev)) {
            sameKeys++;
            compose(prev, &a);
            compose(g, &b);
            CHECK(same(a, b));
        }
        prev = g;
    }
    CHECK(sameKeys > 400);   // (the test really exercised the equal-key path)
    printf("compose_key_test: %d checks, %d failed (%d of %d frame pairs kept)\n", gChecks, gFails, sameKeys, total);
    return gFails ? 1 : 0;
}
