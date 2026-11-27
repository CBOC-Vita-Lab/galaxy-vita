// Host unit test for puppet player forms and animation ids with missing resources (puppetprep).
// Build: g++ -std=c++20 -fsanitize=address,undefined -g -Iplatform/include -Idecomp/include tests/mp/puppet_form_test.cpp -o /tmp/puppet_form_test
//
// What a puppet shows for every form id (0..255, valid and invalid) on all six characters, with the form's own resources missing
// (a puppet never loads any: the fallback IS the design), plus what a STATE with an out-of-range form / animation id does.
#include "Game/MP/MpPoseMath.hpp"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int gFail = 0, gChecks = 0;
#define CHECK(c)                                                                                                                       \
    do {                                                                                                                               \
        gChecks++;                                                                                                                     \
        if (!(c)) {                                                                                                                    \
            gFail++;                                                                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                                        \
        }                                                                                                                              \
    } while (0)

static void testFormTable() {
    for (int ch = 0; ch < MP_LOOK_COUNT; ch++) {
        const bool plain = mp_character_is_toad(ch) || mp_character_is_cosmic(ch);
        for (int f = 0; f < 256; f++) {
            MpFormLook l = mp_form_look((uint32_t)f, ch, 12345u);
            const int cf = f < MP_FORM_COUNT ? f : MP_FORM_NORMAL;
            CHECK(l.form == cf);           // unknown ids are the normal form
            CHECK(!l.ownModel);            // never needs a model / archive of its own: nothing to be missing
            if (plain) {
                // Cosmic / Toad: their own model and materials, plain; no btp
                CHECK(!l.tinted && l.btpFrame == -1 && l.r == 255 && l.g == 255 && l.b == 255 && l.alpha == 255);
            } else {
                CHECK(l.btpFrame >= 0 && l.btpFrame <= 2);
                CHECK(l.btpFrame == (cf == MP_FORM_FIRE ? 2 : (cf == MP_FORM_FLYING ? 1 : 0)));
                // the visible hint: every form but normal / fire / flying (btp-coloured) tints the material
                const bool shouldTint = cf == MP_FORM_ICE || cf == MP_FORM_BEE || cf == MP_FORM_BOO || cf == MP_FORM_SPRING ||
                                        cf == MP_FORM_BONE || cf == MP_FORM_RAINBOW;
                CHECK(l.tinted == shouldTint);
                CHECK(l.alpha == (cf == MP_FORM_BOO ? 0xA0 : 255));   // a Boo is the see-through puppet; every other form is solid
            }
        }
    }
    // rainbow cycles through six colours, never out of range, for any phase
    for (uint32_t t = 0; t < 4000000000u; t += 7919u * 7u) {
        MpFormLook l = mp_form_look(MP_FORM_RAINBOW, MP_LOOK_MARIO, t);
        CHECK(l.tinted);
    }
    // out-of-range character ids (clamped by the drop-in logic before, but the look function must not index anything)
    for (int ch = -3; ch < 300; ch += 7) (void)mp_form_look(2, ch, 0);
}

// The local player's body is "hidden" for the others only for real hides, never because he is a Boo (his own body is hidden by the
// game, the Boo model replaces it): an invisible Boo player was the first bug the form audit found.
static void testHidden() {
    for (int f = 0; f < 256; f++) {
        const bool boo = f == MP_FORM_BOO;
        for (int m = 0; m < 32; m++) {
            const bool h481 = m & 1, h482 = m & 2, h483 = m & 4, mode = m & 8, model = m & 16;
            const bool got = mp_pose_hidden(h481, h482, h483, mode, model, (uint32_t)f);
            const bool want = h481 || h482 || model || (!boo && (h483 || mode));
            CHECK(got == want);
            if (boo && !h481 && !h482 && !model) CHECK(!got);   // a Boo by himself is visible
        }
    }
    CHECK(!mp_pose_hidden(false, false, true, true, false, MP_FORM_BOO));    // Boo Mario: _483 and "player hidden" both set
    CHECK(mp_pose_hidden(false, true, false, false, false, MP_FORM_BOO));    // a Boo in a black hole / cannon is hidden like anybody
    CHECK(mp_pose_hidden(false, false, true, true, false, MP_FORM_NORMAL));  // (the same flags on a plain form: hidden, as before)
}

static void putPose(uint8_t* buf, size_t* n, uint8_t form, uint32_t anim, uint16_t flags) {
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.quat[3] = 1.0f;
    p.scale = 1.0f;
    p.anim = anim;
    p.form = 0;
    p.flags = flags;
    MpPose w;
    MpPoseMath::encodePose(p, &w);
    w.form = form;   // (encodePose clamps; write the raw id as a hostile sender would)
    MpWriter wr(buf, 128);
    mp_write_pose(wr, w);
    *n = wr.n;
}

static void testWireForms() {
    uint8_t buf[128];
    size_t n;
    for (int f = 0; f < 256; f++) {
        putPose(buf, &n, (uint8_t)f, 0x1234u, MP_POSE_VISIBLE);
        MpPoseF d;
        const bool ok = MpPoseMath::readState(buf, n, &d);
        CHECK(ok);                                  // the pose is kept: the player does not vanish or freeze on a new form id
        CHECK(!ok || d.form < MP_FORM_COUNT);
        CHECK(!ok || d.form == (f < MP_FORM_COUNT ? f : 0));
        MpReader r(buf, n);
        MpPose raw;
        CHECK(mp_read_pose(r, &raw) && raw.form < MP_FORM_COUNT);
    }
    // encodePose never writes an invalid form either
    MpPoseF p;
    memset(&p, 0, sizeof p);
    p.quat[3] = 1.0f; p.scale = 1.0f;
    p.form = 200;
    MpPose w;
    MpPoseMath::encodePose(p, &w);
    CHECK(w.form == MP_FORM_NORMAL);
}

// An animation id the receiver does not know (its group table / name map has no such hash): the puppet keeps its previous
// animation (MpRemotePlayer::applyAnimation: ok == false); this checks the name map side of that decision.
static void testUnknownAnimIds() {
    static const char* const kNames[] = {"Wait", "Walk", "Run", "Jump", "BeeFly", "TeresaFloat", "FireShoot", "IceSkate", "SpringJump", "Swim"};
    MpAnimNameMap* m = new MpAnimNameMap();
    m->clear();
    uint32_t known[10];
    for (int i = 0; i < 10; i++) { known[i] = MpPoseMath::animHash(kNames[i]); m->add(known[i], kNames[i]); }
    m->finish();
    for (int i = 0; i < 10; i++) CHECK(m->find(known[i]) != nullptr);
    uint32_t seed = 99;
    int missed = 0;
    for (int i = 0; i < 200000; i++) {
        seed = seed * 1664525u + 1013904223u;
        const uint32_t h = seed;
        const char* nm = m->find(h);
        bool isKnown = false;
        for (int k = 0; k < 10; k++) isKnown = isKnown || known[k] == h;
        CHECK((nm != nullptr) == isKnown);
        if (nm == nullptr) missed++;
    }
    CHECK(missed > 199000);
    CHECK(m->find(0) == nullptr);
    // an empty / unsorted map never answers
    MpAnimNameMap* e = new MpAnimNameMap();
    e->clear();
    CHECK(e->find(0x1234) == nullptr);
    e->add(1, "x");
    CHECK(e->find(1) == nullptr);   // not finished: no answer (never a read of unsorted data)
    delete m;
    delete e;

    // the pose decoder with extreme animation fields: frames, rates, weights, hashes of every kind never produce NaN / inf
    uint8_t buf[128];
    for (int i = 0; i < 100000; i++) {
        size_t n;
        seed = seed * 1664525u + 1013904223u;
        putPose(buf, &n, (uint8_t)(seed >> 3), seed * 2654435761u, (uint16_t)(seed >> 8) & 0x1FFF);
        for (size_t k = 0; k < 6; k++) { seed = seed * 1664525u + 1013904223u; buf[34 + k] = (uint8_t)(seed >> 11); }   // scale/anim/frame bytes
        MpPoseF d;
        if (!MpPoseMath::readState(buf, n, &d)) continue;
        CHECK(MpPoseMath::finitef(d.frame) && d.frame >= 0.0f && MpPoseMath::finitef(d.rate) && fabsf(d.rate) <= 4.0f);
        CHECK(d.scale > 0.0f && d.scale <= 4.0f);
        for (int w = 0; w < 4; w++) CHECK(d.weights[w] >= 0.0f && d.weights[w] <= 1.0f);
        CHECK(d.form < MP_FORM_COUNT);
    }
}

// review2 #2 + warm frustum fix: the warm pose places the puppet cleanly inside the frustum at cam + fwd * (nearZ + 120)
// with identity rotation and tiny 0.001 scale (so both solid and ghost warm draws pass StaticRunCulled behind the opening wipe).
static void testWarmPose() {
    const float cams[][3] = {{0, 0, 0}, {120.5f, -3.0f, 9000.0f}, {-1e5f, 1e5f, 7.0f}};
    const float fwd[3] = {0.0f, 0.0f, -1.0f};
    const float nearZ = 100.0f;
    const float dist = nearZ + 120.0f;
    for (const auto& c : cams) {
        MpWarmPose w = mp_warm_pose(c[0], c[1], c[2], fwd[0], fwd[1], fwd[2], nearZ);
        float base[3][4];
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) base[r][k] = w.rot[r][k];   // calcAndSetBaseMtx: rotation from mDrawRotMtx
            base[r][3] = w.pos[r];                                   // translation from mDrawPos
        }
        CHECK(fabsf(base[0][3] - (c[0] + fwd[0] * dist)) < 1e-3f &&
              fabsf(base[1][3] - (c[1] + fwd[1] * dist)) < 1e-3f &&
              fabsf(base[2][3] - (c[2] + fwd[2] * dist)) < 1e-3f);
        for (int r = 0; r < 3; r++) for (int k = 0; k < 3; k++) CHECK(base[r][k] == (r == k ? 1.0f : 0.0f));
        CHECK(fabsf(w.scale - 0.001f) < 1e-6f);
    }
}

int main() {
    testWarmPose();
    testFormTable();
    testHidden();
    testWireForms();
    testUnknownAnimIds();
    printf("puppet_form_test: %d checks, %d failed\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
