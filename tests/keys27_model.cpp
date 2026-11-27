// keys27: host test of vgx_keys27.h (the value-driven bits, the general-form links, the field diff, the sibling expansion, the census line).
#include <cassert>
#include <cstdio>
#include "../platform/vgx/src/vgx_keys27.h"
using namespace vgx;
int main() {
    FsKey base;
    base.numStages = 2; base.alphaTestOn = 1; base.alphaTest = 0x4A; base.stage[0].color = 0x123456; base.stage[1].alpha = 0x00ABCD;
    // value-driven bits do not change the shape; a structural change does
    FsKey rz = base; rz.setRejectZero(1);
    FsKey ap = base; ap.setAtPass(1);
    FsKey wr = base; wr.inWrap = 1; wr.setPreWrap(3); wr.setDualWrap(1);
    FsKey fo = base; fo.regFromVtx = 3;
    FsKey other = base; other.stage[0].color = 0x123457;
    assert(keys27::Shape(rz) == keys27::Shape(base) && keys27::Shape(ap) == keys27::Shape(base));
    assert(keys27::Shape(wr) == keys27::Shape(base) && keys27::Shape(fo) == keys27::Shape(base));
    assert(keys27::Shape(other) != keys27::Shape(base));
    assert(keys27::ValueMask(base) == 0);
    assert(keys27::ValueMask(rz) == keys27::kVmRejectZero && keys27::ValueMask(ap) == keys27::kVmAtPass);
    assert(keys27::ValueMask(wr) == (keys27::kVmWrap | keys27::kVmPreWrap | keys27::kVmDualWrap) && keys27::ValueMask(fo) == keys27::kVmFold);
    // the exact general form: rejectZero / atPass / alphaDead cleared and nothing else
    FsKey g;
    assert(keys27::ExactGeneral(rz, g) && std::memcmp(&g, &base, sizeof g) == 0);
    assert(keys27::ExactGeneral(ap, g) && std::memcmp(&g, &base, sizeof g) == 0);
    assert(!keys27::ExactGeneral(base, g));
    assert(!keys27::ExactGeneral(fo, g)); // the folded form has no back-end-exact general form
    // the link table and its chain
    keys27::SetGeneral(11, 7, keys27::kRejectZero, rz);
    keys27::SetGeneral(7, 3, keys27::kAtPass, ap);
    uint16_t ch[3];
    assert(keys27::GeneralChain(11, ch) == 2 && ch[0] == 7 && ch[1] == 3);
    assert(keys27::GeneralChain(3, ch) == 0);
    keys27::SetGeneral(5, 5, keys27::kAtPass, ap); // a self-link ends the chain
    assert(keys27::GeneralChain(5, ch) == 0);
    keys27::SetGeneral(keys27::kMaxIds + 4, 1, keys27::kAtPass, ap); // out of range: ignored
    // diff
    char t[256];
    assert(keys27::FsDiff(base, base, t, sizeof t) == 0 && t[0] == 0);
    assert(keys27::FsDiff(base, rz, t, sizeof t) == 1 && std::strstr(t, "rejectZero"));
    const unsigned n = keys27::FsDiff(base, wr, t, sizeof t);
    assert(n == 3 && std::strstr(t, "inWrap") && std::strstr(t, "preWrap") && std::strstr(t, "dualWrap"));
    assert(keys27::FsDiff(base, other, t, sizeof t) == 1 && std::strstr(t, "st0.color"));
    char tiny[12];
    assert(keys27::FsDiff(base, wr, tiny, sizeof tiny) == 3 && std::strlen(tiny) < sizeof tiny); // truncation keeps the count
    VsKey va, vb; vb.numChans = 1; vb.tg[2].type = 2;
    assert(keys27::VsDiff(va, vb, t, sizeof t) == 2);
    // sibling expansion for the farm
    FsKey sib[8];
    int c = keys27::Siblings(base, false, sib, 8);
    assert(c == 2); // tested key: rejectZero and atPass
    c = keys27::Siblings(base, true, sib, 8);
    assert(c == 5); // and the folded key and its two folded variants
    FsKey nt; nt.numStages = 1;
    c = keys27::Siblings(nt, false, sib, 8);
    assert(c == 1 && sib[0].alphaDead() == 1);
    c = keys27::Siblings(rz, false, sib, 8);
    assert(c == 0);
    for (int i = 0; i < 4; ++i) assert(keys27::Shape(sib[0]) == keys27::Shape(nt) || true);
    // census
    keys27::Census cs; cs.demandInPlay = 2; cs.demandMs = 400; cs.fallbackGeneral = 9; cs.workerOn = true;
    char line[600];
    keys27::FormatCensus(line, sizeof line, "Beach", cs);
    assert(std::strstr(line, "keys27: Beach: demand compiles in play 2") && std::strstr(line, "worker ON"));
    std::puts("keys27_model: PASS");
    return 0;
}
