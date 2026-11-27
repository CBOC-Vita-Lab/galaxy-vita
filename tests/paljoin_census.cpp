// paljoin31 (plan-gpu31 A-T1): host test of platform/vgx/src/vgx_paljoin.h, the decision of GXV_PALJOIN_CENSUS, against a synthetic J3D model.
//
// The model is built the way J3D draws a skinned character: a model data (one array base) with shapes; a shape has matrix groups; a group loads
// up to four matrices into slots 0, 3, 6, 9 and draws; the next group loads others. Every case states the answer by hand and the census must
// agree. tools/test_paljoin_census.sh builds this file once as it is (everything must pass) and once per PALJOIN_MUTANT=n (one rule broken:
// the test must fail), so each rule of the join is held by a case here.
#include "../platform/vgx/src/vgx_paljoin.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace paljoin;

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL line %d: %s  ", __LINE__, #c); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// ---- the synthetic model
struct Group { std::vector<int> joints; };           // the joints loaded into slots 0, 3, 6, ... (matrix ids)
struct Shape { uint32_t material = 1; std::vector<Group> groups; };
struct Model { uint64_t base = 0x80400000u; std::vector<Shape> shapes; };

// one matrix's content: what hashing the 12 + 9 words of the palette's rows gives; the same joint is the same content in any slot
static uint64_t MatHash(int joint) { uint32_t w[2] = {0xA5A50000u + static_cast<uint32_t>(joint), 7u}; return HashWords(w, 2); }

struct Driver {
    Census c;
    uint32_t jobSeq = 0;
    std::vector<Verdict> verdicts;
    // One group of a shape as a palette draw: loads its joints (the registers other than matrix memory do not change), then a static run.
    Verdict Draw1(const Model& m, const Shape& s, const Group& g, bool lit = true, bool regs = false, uint32_t serial = 100, uint32_t fs = 5, uint64_t vs = 77) {
        Draw d;
        d.model = m.base << 32 | (m.base + 0x1000);
        d.serial = serial; d.useFs = fs; d.vsNorm = vs ^ s.material; d.jobSeq = jobSeq; d.regsWritten = regs; d.lit = lit; d.palClass = 2;
        d.vertices = 90; d.indices = 150;
        uint64_t image = 1;
        for (size_t i = 0; i < g.joints.size() && i < static_cast<size_t>(kMaxSlots); ++i) {
            const uint64_t h = MatHash(g.joints[i]);
            bool have = false;
            for (int j = 0; j < d.nMat; ++j) have |= d.mat[j] == h;
            if (!have) d.mat[d.nMat++] = h;
            image = HashWords(reinterpret_cast<const uint32_t*>(&h), 2, image * (i + 3));
        }
        d.image = image;
        const Verdict v = c.Note(d);
        c.AddNs(v, 1000); // (1 us a draw)
        ++jobSeq;         // the run's job
        c.Sent(jobSeq);
        verdicts.push_back(v);
        return v;
    }
    void Other() { ++jobSeq; } // a draw that is not a palette run (rigid, dynamic, a copy): a job in between
};
static Group G(std::initializer_list<int> j) { Group g; g.joints = j; return g; }

int main() {
    // ---- 1. a character: one shape, five matrix groups of up to four joints; Mario-like
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1, 2, 3}), G({3, 4, 5}), G({5, 6, 7, 8}), G({8, 9}), G({9, 10, 11})};
        m.shapes.push_back(sh);
        Driver d;
        for (const Group& g : sh.groups) d.Draw1(m, sh, g);
        d.c.PictureEnd();
        // distinct joints 0..11 = 12 matrices: more than 10, so the shader's palette holds groups 1-4 (joints 0..9 = 10), the fifth starts a new draw
        CHECK(d.c.c.palDraws == 5, "palDraws %llu", (unsigned long long)d.c.c.palDraws);
        CHECK(d.c.c.removed[0] == 3, "cap 10: groups 2,3,4 join group 1 (joints 0-9), group 5 does not fit: removed %llu", (unsigned long long)d.c.c.removed[0]);
        CHECK(d.c.c.brk[BkCap] == 1, "the fifth group breaks on the palette size: %llu", (unsigned long long)d.c.c.brk[BkCap]);
        CHECK(d.c.c.removed[2] == 4, "no limit: 4 removed, got %llu", (unsigned long long)d.c.c.removed[2]);
        CHECK(d.c.c.removed[1] == 4, "21 matrices holds all 12: %llu", (unsigned long long)d.c.c.removed[1]);
        CHECK(d.c.c.removedReload[0] == 3 && d.c.c.removedSame[0] == 0, "all three have a different palette: reload %llu same %llu",
              (unsigned long long)d.c.c.removedReload[0], (unsigned long long)d.c.c.removedSame[0]);
        CHECK(d.c.c.removedNs[0] == 3000, "front end of the removed: %llu", (unsigned long long)d.c.c.removedNs[0]);
        CHECK(d.c.c.chains[0] == 2 && d.c.c.chainLen[0][2] == 1 && d.c.c.chainLen[0][0] == 1, "chains: a 4 and a 1");
        CHECK(d.c.c.chains[2] == 1 && d.c.c.chainLen[2][3] == 1, "one chain of 5 with no limit");
        CHECK(d.c.nModels == 1, "one model");
    }
    // ---- 2. the same shape, unlit: 21 matrices allowed
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1, 2, 3}), G({4, 5, 6, 7}), G({8, 9, 10, 11}), G({12, 13, 14, 15}), G({16, 17, 18, 19}), G({20, 21, 22})};
        Driver d;
        for (const Group& g : sh.groups) d.Draw1(m, sh, g, false);
        d.c.PictureEnd();
        // 23 distinct joints: the first five groups hold 20, the sixth would make 23 > 21
        CHECK(d.c.c.removed[0] == 4, "unlit: 4 removed, got %llu", (unsigned long long)d.c.c.removed[0]);
        Driver e;
        for (const Group& g : sh.groups) e.Draw1(m, sh, g, true);
        e.c.PictureEnd();
        CHECK(e.c.c.removed[0] == 3, "lit: (1 2) (3 4) (5 6): a third group would make 12 > 10: got %llu", (unsigned long long)e.c.c.removed[0]);
    }
    // ---- 3. state differs between groups: a material change splits the chain
    {
        Model m;
        Shape a, b;
        a.groups = {G({0, 1}), G({2, 3})};
        b.groups = {G({4, 5}), G({6, 7})};
        b.material = 2;
        Driver d;
        d.Draw1(m, a, a.groups[0]); d.Draw1(m, a, a.groups[1]);
        d.Draw1(m, b, b.groups[0], true, false, 101, 6, 77); d.Draw1(m, b, b.groups[1], true, false, 101, 6, 77);
        d.c.PictureEnd();
        CHECK(d.c.c.removed[0] == 2, "two shapes of two groups: 2 removed, got %llu", (unsigned long long)d.c.c.removed[0]);
        CHECK(d.c.c.brk[BkState] == 1, "the second shape's first group differs in state: %llu", (unsigned long long)d.c.c.brk[BkState]);
    }
    // ---- 4. a register written between the groups, a draw between them, another model
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1}), G({2, 3}), G({4, 5}), G({6, 7}), G({8, 9})};
        Driver d;
        d.Draw1(m, sh, sh.groups[0]);
        d.Draw1(m, sh, sh.groups[1], true, true);  // a register (a colour, a light, a texture) was written: a different state
        d.Draw1(m, sh, sh.groups[2]);              // joins the one before
        d.Other();                                 // a draw of something else between
        d.Draw1(m, sh, sh.groups[3]);
        Model m2;
        m2.base = 0x80500000u;
        d.Draw1(m2, sh, sh.groups[4]);            // another model, same state
        d.c.PictureEnd();
        CHECK(d.c.c.removed[0] == 1, "only group 3 joins group 2: removed %llu", (unsigned long long)d.c.c.removed[0]);
        CHECK(d.c.c.brk[BkRegs] == 1, "registers: %llu", (unsigned long long)d.c.c.brk[BkRegs]);
        CHECK(d.c.c.brk[BkSent] == 1, "a job between: %llu", (unsigned long long)d.c.c.brk[BkSent]);
        CHECK(d.c.c.brk[BkModel] == 1, "another model: %llu", (unsigned long long)d.c.c.brk[BkModel]);
        CHECK(d.c.nModels == 2, "two models: %d", d.c.nModels);
        CHECK(d.c.models[0].draws == 4 && d.c.models[1].draws == 1, "draws by model");
        CHECK(d.c.models[0].removed == 1 && d.c.models[0].removedNs == 1000, "joinable by model");
    }
    // ---- 5. the same palette twice: not a reload (the record stage may already merge it), counted apart
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1, 2}), G({0, 1, 2}), G({0, 1, 3})};
        Driver d;
        for (const Group& g : sh.groups) d.Draw1(m, sh, g);
        d.c.PictureEnd();
        CHECK(d.c.c.removed[0] == 2 && d.c.c.removedSame[0] == 1 && d.c.c.removedReload[0] == 1, "same %llu reload %llu", (unsigned long long)d.c.c.removedSame[0],
              (unsigned long long)d.c.c.removedReload[0]);
        CHECK(d.c.c.removedReloadNs[0] == 1000, "reload ns %llu", (unsigned long long)d.c.c.removedReloadNs[0]);
    }
    // ---- 6. exactly the palette: ten distinct matrices fit, eleven do not; matrices shared between groups are counted once
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1, 2, 3}), G({4, 5, 6, 7}), G({8, 9, 0, 1})};
        Driver d;
        for (const Group& g : sh.groups) d.Draw1(m, sh, g);
        d.c.PictureEnd();
        CHECK(d.c.c.removed[0] == 2 && d.c.c.brk[BkCap] == 0, "ten matrices fit: removed %llu", (unsigned long long)d.c.c.removed[0]);
        Shape s2;
        s2.groups = {G({0, 1, 2, 3}), G({4, 5, 6, 7}), G({8, 9, 10, 11})};
        Driver e;
        for (const Group& g : s2.groups) e.Draw1(m, s2, g);
        e.c.PictureEnd();
        CHECK(e.c.c.removed[0] == 1 && e.c.c.brk[BkCap] == 1, "twelve do not: removed %llu cap breaks %llu", (unsigned long long)e.c.c.removed[0], (unsigned long long)e.c.c.brk[BkCap]);
    }
    // ---- 6b. eleven distinct matrices do not fit a lit palette (10), twenty-one fit an unlit one and twenty-two do not
    {
        Model m;
        Shape s3;
        s3.groups = {G({0, 1, 2, 3}), G({4, 5, 6, 7}), G({8, 9, 10})};
        Driver d;
        for (const Group& g : s3.groups) d.Draw1(m, s3, g);
        d.c.PictureEnd();
        CHECK(d.c.c.removed[0] == 1 && d.c.c.brk[BkCap] == 1, "eleven lit: removed %llu", (unsigned long long)d.c.c.removed[0]);
        Shape s4;
        s4.groups = {G({0, 1, 2, 3}), G({4, 5, 6, 7}), G({8, 9, 10, 11}), G({12, 13, 14, 15}), G({16, 17, 18, 19}), G({20}), G({21})};
        Driver e;
        for (const Group& g : s4.groups) e.Draw1(m, s4, g, false);
        e.c.PictureEnd();
        CHECK(e.c.c.removed[0] == 5 && e.c.c.brk[BkCap] == 1, "unlit: 21 fit (groups 1-6), the 22nd does not: removed %llu", (unsigned long long)e.c.c.removed[0]);
    }
    // ---- 7. a draw that cannot join (a texture matrix from the palette, a row above 61, rows unknown) starts nothing and ends the chain
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1}), G({2, 3}), G({4, 5})};
        Driver d;
        d.Draw1(m, sh, sh.groups[0]);
        Draw bad;
        bad.model = m.base << 32; bad.jobSeq = d.jobSeq; bad.serial = 100; bad.useFs = 5; bad.vsNorm = 77 ^ 1; bad.ineligible = InTexgenPal; bad.nMat = 2; bad.mat[0] = 1; bad.mat[1] = 2;
        const Verdict v = d.c.Note(bad);
        d.c.AddNs(v, 1000);
        ++d.jobSeq; d.c.Sent(d.jobSeq);
        d.Draw1(m, sh, sh.groups[1]);
        d.Draw1(m, sh, sh.groups[2]);
        d.c.PictureEnd();
        CHECK(!v.joined[0] && v.brk == BkIneligible, "not joinable");
        CHECK(d.c.c.ineligible[InTexgenPal] == 1, "counted by reason");
        CHECK(d.c.c.removed[0] == 1, "only the last two join: removed %llu", (unsigned long long)d.c.c.removed[0]);
    }
    // ---- 8. pictures: no chain across the picture's end (the job counter restarts)
    {
        Model m;
        Shape sh;
        sh.groups = {G({0, 1}), G({2, 3})};
        Driver d;
        d.Draw1(m, sh, sh.groups[0]);
        d.c.PictureEnd();
        d.jobSeq = 0;
        d.Draw1(m, sh, sh.groups[1]);
        d.c.PictureEnd();
        CHECK(d.c.c.removed[0] == 0 && d.c.c.pictures == 2, "no join across pictures: %llu", (unsigned long long)d.c.c.removed[0]);
    }
    // ---- 9. a scene: 30 characters of 6 groups each, drawn one after the other, and 20 rigid draws between some of them
    {
        Driver d;
        Model models[30];
        uint64_t expectRemoved = 0;
        for (int i = 0; i < 30; ++i) {
            models[i].base = 0x81000000u + static_cast<uint32_t>(i) * 0x20000u;
            Shape sh;
            for (int g = 0; g < 6; ++g) sh.groups.push_back(G({g * 2, g * 2 + 1, g * 2 + 2}));
            // 13 distinct joints: 10 fit; groups 1-5 hold joints 0..10 = 11 > 10, so 1-4 hold 0..8 (9), the 5th adds 9, 10 -> 11: no
            for (const Group& g : sh.groups) d.Draw1(models[i], sh, g);
            if (i % 3 == 0) d.Other();
        }
        d.c.PictureEnd();
        // per model: groups 1..4 join (joints 0..8 = 9), the 5th needs 9, 10 = 11 > 10: it starts a new draw, the 6th (10..12) joins it: 3 + 1 = 4 removed
        expectRemoved = 30 * 4;
        CHECK(d.c.c.removed[0] == expectRemoved, "scene: removed %llu expected %llu", (unsigned long long)d.c.c.removed[0], (unsigned long long)expectRemoved);
        CHECK(d.c.nModels == 30, "30 models");
        CHECK(d.c.c.palDraws == 180, "180 palette draws");
    }
    // ---- 10. a model table that is full does not crash and the loss is counted
    {
        Driver d;
        Shape sh;
        sh.groups = {G({0})};
        for (int i = 0; i < 70; ++i) {
            Model m;
            m.base = 0x82000000u + static_cast<uint32_t>(i) * 0x20000u;
            d.Draw1(m, sh, sh.groups[0]);
        }
        d.c.PictureEnd();
        CHECK(d.c.nModels == Census::kModels && d.c.modelsLost == 6, "table of %d models, lost %llu", d.c.nModels, (unsigned long long)d.c.modelsLost);
        CHECK(d.c.c.palDraws == 70, "all counted");
    }
    std::printf("paljoin census: %d checks, %d failed (PALJOIN_MUTANT=%d)\n", g_checks, g_fail, PALJOIN_MUTANT);
    return g_fail == 0 ? 0 : 1;
}
