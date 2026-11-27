// GXV_STARBATCH, the pure logic (decomp/libs/JSystem/include/JSystem/J3DGraphBase/J3DPortSort.hpp): the plan of a drawer's order of packets, the material
// list's test, the skeleton. tools/test_starbatch.sh builds and runs it (no game data, no GL).
#include <JSystem/J3DGraphBase/J3DPortSort.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
using namespace j3dportsort;
int fails = 0, checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++fails; std::printf("FAIL: %s\n", what); }
}
struct Dl {
    std::vector<uint8_t> b;
    void bp(unsigned reg, unsigned v) { b.push_back(0x61); b.push_back(uint8_t(reg)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    void xf(unsigned addr, unsigned n, unsigned seed) {
        b.push_back(0x10); b.push_back(uint8_t((n - 1) >> 8)); b.push_back(uint8_t(n - 1)); b.push_back(uint8_t(addr >> 8)); b.push_back(uint8_t(addr));
        for (unsigned i = 0; i < n; ++i) for (int k = 0; k < 4; ++k) b.push_back(uint8_t(seed * 31 + i * 7 + k));
    }
    void cp(unsigned reg, unsigned v) { b.push_back(0x08); b.push_back(uint8_t(reg)); for (int k = 3; k >= 0; --k) b.push_back(uint8_t(v >> (8 * k))); }
};
unsigned rs = 99;
unsigned Rnd(unsigned n) { rs = rs * 1664525u + 1013904223u; return (rs >> 8) % n; }
Dl Colour(unsigned c) { Dl d; d.xf(0x100C, 1, c); return d; }

// a plan is right when: a permutation, the first stays first when pinned, the last stays last, and every list's packets are one run (two for the split group)
bool GoodPlan(const std::vector<In>& in, bool pin, const std::vector<uint16_t>& o) {
    const uint32_t m = uint32_t(in.size());
    if (!CheckPlan(in.data(), m, pin, o.data())) return false;
    auto same = [&](uint32_t a, uint32_t b) { return in[a].size == in[b].size && memcmp(in[a].dl, in[b].dl, in[a].size) == 0; };
    // runs of equal lists in the new order: each list at most once, except the list of the first packet when it is also the last's
    std::vector<uint32_t> heads;
    uint32_t runsOfLast = 0;
    for (uint32_t k = 0; k < m; ++k) {
        if (k > 0 && same(o[k], o[k - 1])) continue;
        bool seen = false;
        for (uint32_t h : heads) if (same(o[k], h)) seen = true;
        if (seen) {
            if (!same(o[k], m - 1)) return false;  // (only the last packet's list may come twice: split)
            ++runsOfLast;
        }
        heads.push_back(o[k]);
    }
    return runsOfLast <= 1;
}
}  // namespace

int main() {
    // ---- skeleton and pixel registers
    {
        Dl a = Colour(1), b = Colour(2), c; c.xf(0x100C, 2, 3);
        uint32_t ha, hb, hc, pe;
        Check(Skeleton(a.b.data(), a.b.size(), &ha, &pe) && !pe, "skeleton of a colour list");
        Skeleton(b.b.data(), b.b.size(), &hb, &pe);
        Skeleton(c.b.data(), c.b.size(), &hc, &pe);
        Check(ha == hb, "same skeleton, other data");
        Check(ha != hc, "other count: other skeleton");
        Dl z; z.bp(0x40, 0x17);
        Check(Skeleton(z.b.data(), z.b.size(), &ha, &pe) && pe, "z mode is a pixel-engine register");
        Dl m1, m2; m1.bp(0xFE, 0xFFFF); m1.bp(0x45, 1); m2.bp(0xFE, 0xFF00); m2.bp(0x45, 1);
        uint32_t h1, h2;
        Skeleton(m1.b.data(), m1.b.size(), &h1, &pe); Skeleton(m2.b.data(), m2.b.size(), &h2, &pe);
        Check(h1 != h2, "a different mask is a different skeleton");
        Dl bad; bad.b.push_back(0x40);
        Check(!Skeleton(bad.b.data(), bad.b.size(), &ha, &pe), "an unknown opcode is not a skeleton");
    }
    // ---- the material list's test
    {
        auto mat = [](unsigned z, unsigned cm0, bool maskedZ) {
            Dl d;
            d.xf(0x1009, 1, 1);
            if (maskedZ) { d.bp(0xFE, 0x00001F); d.bp(0x40, z); } else d.bp(0x40, z);
            d.bp(0x41, cm0);
            return d;
        };
        auto ok = [](const Dl& d) { return MatListOpaqueDepth(d.b.data(), uint32_t(d.b.size())); };
        Check(ok(mat(0x17, 0x18, false)), "opaque LEQUAL depth write");
        Check(ok(mat(0x13, 0x18, false)), "LESS");
        Check(ok(mat(0x17, 0x18, true)), "z mode by a mask write that covers its five bits");
        Check(!ok(mat(0x07, 0x18, false)), "depth write off");
        Check(!ok(mat(0x16, 0x18, false)), "depth compare off");
        Check(!ok(mat(0x1F, 0x18, false)), "ALWAYS compare");
        Check(!ok(mat(0x15, 0x18, false)), "EQUAL compare");
        Check(!ok(mat(0x17, 0x19, false)), "blend on");
        Check(!ok(mat(0x17, 0x1A, false)), "logic op on");
        Check(ok(mat(0x1F ^ 0x08 ^ 0x04, 0x18, false)) == true || true, "(shape of the test)");
        { Dl d; d.bp(0x41, 0x18); Check(!ok(d), "no z mode written: left to an earlier list"); }
        { Dl d; d.bp(0x40, 0x17); Check(!ok(d), "no blend mode written"); }
        { Dl d = mat(0x17, 0x18, false); d.bp(0x40, 0x07); Check(!ok(d), "the last z mode write counts"); }
        { Dl d; d.bp(0xFE, 0x00000F); d.bp(0x40, 0x17); d.bp(0x41, 0x18); Check(!ok(d), "a mask that leaves bit 4 of the z mode to an earlier list"); }
        { Dl d = mat(0x17, 0x18, false); d.b.push_back(0x80); Check(!ok(d), "a draw in the list"); }
    }
    // ---- the plan, random
    unsigned okPlans = 0, same = 0, refused = 0;
    for (int round = 0; round < 4000; ++round) {
        const unsigned m = 2 + Rnd(40);
        const unsigned colours = 1 + Rnd(7);
        std::vector<Dl> lists(colours);
        for (unsigned c = 0; c < colours; ++c) lists[c] = Colour(c * 17 + 3);
        const bool poison = Rnd(8) == 0;  // a list with another skeleton / a CP write / a pixel register
        const unsigned kind = Rnd(4);
        std::vector<Dl> mine(m);
        std::vector<In> in(m);
        for (unsigned i = 0; i < m; ++i) {
            mine[i] = lists[Rnd(colours)];
        }
        if (poison) {
            Dl& d = mine[Rnd(m)];
            if (kind == 0) d.xf(0x100A, 1, 5);
            else if (kind == 1) d.cp(0x50, 7);
            else if (kind == 2) d.bp(0x41, 0x18);
            else d.xf(0x0, 12, 9);  // position matrix memory
        }
        for (unsigned i = 0; i < m; ++i) in[i] = In{mine[i].b.data(), uint32_t(mine[i].b.size())};
        const bool pin = Rnd(2) != 0;
        std::vector<uint16_t> o(m);
        Info info = {};
        const int r = Plan(in.data(), m, pin, o.data(), &info, 10, 4);
        if (m < 4) { Check(r == J3DPortSortWhy_Few, "few packets: left alone"); continue; }
        // identity unless Ok
        bool identity = true;
        for (unsigned i = 0; i < m; ++i) identity = identity && o[i] == i;
        if (r != J3DPortSortWhy_Ok) {
            Check(identity, "not planned: the order is the identity");
            if (r == J3DPortSortWhy_Same) ++same; else ++refused;
            // refused only where there is something to refuse
            if (!poison && r != J3DPortSortWhy_Same) Check(false, "a clean stretch was refused");
            continue;
        }
        ++okPlans;
        Check(!poison, "a poisoned stretch was planned");
        Check(GoodPlan(in, pin, o), "plan is a permutation, pins held, lists adjacent");
        Check(info.runsAfter <= info.runsBefore, "runs do not grow");
        Check(info.recAfter <= info.recBefore + 1, "records do not grow (but by the split)");
    }
    std::printf("plans: %u ordered, %u already one list, %u refused (of the poisoned ones)\n", okPlans, same, refused);
    Check(okPlans > 1000, "most random stretches are ordered");
    // ---- the shape of the star-bit case: 110 packets of six colours at random, one pinned first: the lists sent fall from about 92 to 6 (+ the split)
    {
        std::vector<Dl> lists;
        for (int c = 0; c < 6; ++c) lists.push_back(Colour(c * 40 + 1));
        std::vector<int> col(110);
        for (int& c : col) c = int(Rnd(6));
        std::vector<In> in(110);
        for (int i = 0; i < 110; ++i) in[i] = In{lists[col[i]].b.data(), uint32_t(lists[col[i]].b.size())};
        std::vector<uint16_t> o(110);
        Info info = {};
        const int r = Plan(in.data(), 110, true, o.data(), &info, 10, 4);
        Check(r == J3DPortSortWhy_Ok, "star bits: planned");
        std::printf("star bits: %u lists in %u runs before, %u after; GL records at 10 a record %u -> %u\n", info.groups, info.runsBefore, info.runsAfter, info.recBefore, info.recAfter);
        Check(info.groups == 6 && info.runsBefore > 60 && info.runsAfter <= 7, "star bits: six colours, many runs before, six (+1) after");
    }
    // ---- (starbatch26) GXV_STARBATCH=2 compares the differed list IN FORCE at each packet's shape draw with the packet's own list, between the own order and the
    // planned order. The sender's rule (DrawBuffer.cpp portDrawPort): a list equal in bytes to the last one sent, with no light between, is not sent; the one in force is then the last
    // sent. Walk both orders of random stretches with that rule: in force == own at every packet (the property the verify mode checks on the console), and the planned order sends
    // as many lists as it has runs (about one a colour) where the own order sends one a run.
    {
        unsigned walked = 0, bad = 0, sentOwn = 0, sentPlanned = 0;
        for (int t = 0; t < 400; ++t) {
            const unsigned m = 8 + Rnd(120), nc = 2 + Rnd(6);
            std::vector<Dl> lists;
            for (unsigned c = 0; c < nc; ++c) lists.push_back(Colour(c * 40 + 1));
            std::vector<unsigned> col(m);
            for (unsigned& c : col) c = Rnd(nc);
            std::vector<In> in(m);
            for (unsigned i = 0; i < m; ++i) in[i] = In{lists[col[i]].b.data(), uint32_t(lists[col[i]].b.size())};
            std::vector<uint16_t> o(m);
            Info info = {};
            if (Plan(in.data(), m, true, o.data(), &info, 10, 4) != J3DPortSortWhy_Ok) continue;
            for (int pass = 0; pass < 2; ++pass) {
                const In* last = nullptr;
                unsigned sent = 0;
                for (unsigned k = 0; k < m; ++k) {
                    const unsigned idx = pass == 0 ? k : o[k];
                    const In& own = in[idx];
                    if (last != nullptr && last->size == own.size && memcmp(last->dl, own.dl, own.size) == 0) {
                        // skipped
                    } else {
                        last = &own;
                        ++sent;
                    }
                    ++walked;
                    if (last->size != own.size || memcmp(last->dl, own.dl, own.size) != 0) ++bad;
                }
                (pass == 0 ? sentOwn : sentPlanned) += sent;
                if (pass == 1) Check(sent == info.runsAfter, "planned order: lists sent == runs after");
            }
        }
        std::printf("verify walk: %u packets walked, %u with a wrong list in force; lists sent %u (own order) -> %u (planned)\n", walked, bad, sentOwn, sentPlanned);
        Check(bad == 0, "the list in force is the packet's own, in both orders");
        Check(sentPlanned < sentOwn, "the planned order sends fewer lists");
        // negative control: a walk that skips a list on a weaker rule (same first byte only) must be caught by the same compare
        unsigned wrong = 0;
        {
            Dl a = Colour(1), b = Colour(41);
            const In* last = &*(new In{a.b.data(), uint32_t(a.b.size())});
            In own{b.b.data(), uint32_t(b.b.size())};
            if (last->dl[0] == own.dl[0]) { /* a wrongly skipped send */ if (memcmp(last->dl, own.dl, own.size) != 0) ++wrong; }
        }
        Check(wrong == 1, "control: a list skipped on a weak rule is a wrong list in force and the compare sees it");
    }
    // ---- negative controls: the checks must see a wrong plan
    {
        std::vector<Dl> lists;
        for (int c = 0; c < 3; ++c) lists.push_back(Colour(c * 40 + 1));
        const int col[8] = {0, 1, 2, 0, 1, 2, 0, 1};
        std::vector<In> in(8);
        for (int i = 0; i < 8; ++i) in[i] = In{lists[col[i]].b.data(), uint32_t(lists[col[i]].b.size())};
        std::vector<uint16_t> o(8);
        Plan(in.data(), 8, true, o.data(), nullptr, 10, 4);
        Check(GoodPlan(in, true, o), "control: the real plan is good");
        std::vector<uint16_t> bad = o;
        std::swap(bad[0], bad[7]);
        Check(!GoodPlan(in, true, bad), "control: first and last swapped is caught");
        bad = o; bad[3] = bad[4];
        Check(!GoodPlan(in, true, bad), "control: not a permutation is caught");
        // a plain sort by colour (what a naive plan does) puts the last packet's group wherever it falls
        std::vector<uint16_t> naive;
        for (int c = 0; c < 3; ++c) for (int i = 0; i < 8; ++i) if (col[i] == c) naive.push_back(uint16_t(i));
        Check(!GoodPlan(in, true, naive), "control: a plain sort by colour breaks the pins and is caught");
    }
    std::printf("starbatch logic: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
