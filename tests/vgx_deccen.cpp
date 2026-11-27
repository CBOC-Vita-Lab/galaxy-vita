// streamc31: the stream-decode census (platform/vgx/src/vgx_deccen.h, GXV_DYN_CENSUS bit 2) on crafted draws with known answers:
// what is matched to the picture before, which attribute is identical (stream slice, indices, array range, its stride / byte order /
// address), whole draws, "only the position changed", the content key (seen before / repeated in the picture), the cap, the times with the
// clock's cost taken off, and the lines. tools/test_vgx_deccen.sh runs it (with ASan / UBSan) and its mutants (each must make it fail).
#include "vgx_deccen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace vgx::deccen;

namespace {
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// A clock that moves 7 ns a read: the calibration finds 7, every timed scope of the census itself then measures 0.
uint64_t g_t = 1000;
uint64_t FakeNow() { g_t += 7; return g_t; }

// A draw: stream of `count` vertices of `ss` bytes; attributes as given.
struct Draw {
    std::vector<uint8_t> stream;
    DrawIn d;
};
DrawIn Quad(const std::vector<uint8_t>& stream, uint64_t sig, uint8_t origin = kTop) {
    // 4 vertices, POS f32 x3 + CLR0 rgba8, all direct: 16 bytes a vertex
    DrawIn d;
    d.sig = sig; d.origin = origin; d.allDirect = true; d.count = 4; d.streamSize = 16; d.stream = stream.data();
    d.nAttr = 2;
    d.a[0].attr = 9; d.a[0].srcOff = 0; d.a[0].srcSize = 12;
    d.a[1].attr = 11; d.a[1].srcOff = 12; d.a[1].srcSize = 4;
    d.decodeNs = 100; d.decodeScopes = 0; d.emitNs = 400; d.emitScopes = 0;
    return d;
}
// An indexed mesh: `count` vertices, PNMTXIDX direct (1 byte), POS / NRM / TEX0 through 16-bit indices: 7 bytes a vertex.
DrawIn Mesh(const std::vector<uint8_t>& stream, uint32_t count, uint64_t sig, const uint8_t* pos, uint32_t posLen, const uint8_t* nrm, uint32_t nrmLen,
            const uint8_t* tex, uint32_t texLen) {
    DrawIn d;
    d.sig = sig; d.origin = kList; d.allDirect = false; d.count = count; d.streamSize = 7; d.stream = stream.data();
    d.nAttr = 4;
    d.a[0].attr = 0; d.a[0].srcOff = 0; d.a[0].srcSize = 1;
    d.a[1].attr = 9; d.a[1].indexed = true; d.a[1].srcOff = 1; d.a[1].srcSize = 2; d.a[1].arr = pos; d.a[1].arrLen = posLen; d.a[1].stride = 12;
    d.a[2].attr = 10; d.a[2].indexed = true; d.a[2].srcOff = 3; d.a[2].srcSize = 2; d.a[2].arr = nrm; d.a[2].arrLen = nrmLen; d.a[2].stride = 12;
    d.a[3].attr = 13; d.a[3].indexed = true; d.a[3].srcOff = 5; d.a[3].srcSize = 2; d.a[3].arr = tex; d.a[3].arrLen = texLen; d.a[3].stride = 8;
    d.decodeNs = 1000; d.decodeScopes = 0; d.emitNs = 3000; d.emitScopes = 0;
    return d;
}
std::vector<uint8_t> Bytes(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(seed * 31u + i * 7u + (i >> 3));
    return v;
}
std::vector<uint8_t> MeshStream(uint32_t count, uint8_t seed) {
    std::vector<uint8_t> s(count * 7u);
    for (uint32_t v = 0; v < count; ++v) {
        s[v * 7] = static_cast<uint8_t>(3 * (seed & 1));
        for (int a = 0; a < 3; ++a) { s[v * 7 + 1 + a * 2] = 0; s[v * 7 + 2 + a * 2] = static_cast<uint8_t>((v + seed) % count); }
    }
    return s;
}
} // namespace

int main() {
    Census c(&FakeNow, 1u << 20);
    // ---- picture 1
    const auto qa = Bytes(64, 1), qd = Bytes(64, 9);
    auto pos = Bytes(12 * 24, 2), nrm = Bytes(12 * 24, 3), tex = Bytes(8 * 24, 4);
    auto pos2 = Bytes(12 * 24, 12), nrm2 = Bytes(12 * 24, 13), tex2 = Bytes(8 * 24, 14);
    const auto ms = MeshStream(24, 0), ms2 = MeshStream(24, 0);
    c.Note(Quad(qa, 0xA));                                                                               // A
    c.Note(Mesh(ms, 24, 0xB, pos.data(), 288, nrm.data(), 288, tex.data(), 192));                        // B (deformed next picture)
    c.Note(Mesh(ms2, 24, 0xB, pos2.data(), 288, nrm2.data(), 288, tex2.data(), 192));                    // C (same signature, its indices change)
    c.Note(Quad(qd, 0xD, kFx));                                                                          // D twice, identical
    c.Note(Quad(qd, 0xD, kFx));
    CHECK(c.ClockNs() == 7.0);
    {
        const Census::Cls t = c.Total();
        CHECK(t.draws == 5);
        CHECK(t.matched == 0);
        CHECK(t.repeat == 1);           // (D's second)
        CHECK(t.repeatDecNs == 100);
        CHECK(t.seenPrev == 0);
        CHECK(t.verts == 4 + 24 + 24 + 4 + 4);
        CHECK(t.decNs == 100 + 1000 + 1000 + 100 + 100);
        CHECK(t.emitNs == 400 + 3000 + 3000 + 400 + 400);
        CHECK(c.Window().c[kList * kShapes + kIndexedSmall].draws == 2);
        CHECK(c.Window().c[kFx * kShapes + kQuadDirect].draws == 2);
        CHECK(c.Window().c[kTop * kShapes + kQuadDirect].draws == 1);
        CHECK(c.Window().c[kList * kShapes + kIndexedSmall].indexed == 2 && c.Window().c[kList * kShapes + kIndexedSmall].inRange == 2);
    }
    c.EndPicture();
    char lines[6][1000];
    CHECK(c.Lines(lines, 6) >= 4); // (resets the window)

    // ---- picture 2: A the same; B's positions deformed (the other arrays, the indices the same); C's indices changed; D the same twice;
    // E new. The texture array of B is copied to another address with the same bytes.
    pos[5] ^= 0x40;
    std::vector<uint8_t> texMoved = tex;
    const auto ms2b = MeshStream(24, 1);
    c.Note(Quad(qa, 0xA));
    c.Note(Mesh(ms, 24, 0xB, pos.data(), 288, nrm.data(), 288, texMoved.data(), 192));
    c.Note(Mesh(ms2b, 24, 0xB, pos2.data(), 288, nrm2.data(), 288, tex2.data(), 192));
    c.Note(Quad(qd, 0xD, kFx));
    c.Note(Quad(qd, 0xD, kFx));
    const auto qe = Bytes(64, 5);
    c.Note(Quad(qe, 0xE));
    {
        const Census::Win& w = c.Window();
        const Census::Cls t = c.Total();
        CHECK(t.draws == 6);
        CHECK(t.matched == 5);          // (E is new)
        CHECK(t.whole == 3);            // A, D, D
        CHECK(t.wholeDecNs == 300);
        CHECK(t.stream == 4);           // A, D, D, B (B's stream bytes are the same, its position array is not)
        CHECK(t.nonPos == 1);           // B
        CHECK(t.nonPosDecEst == 1000 * 3 / 4);
        CHECK(t.seenPrev == 3);         // A, D, D (key B)
        CHECK(t.repeat == 1);
        CHECK(w.c[kList * kShapes + kIndexedSmall].matched == 2);
        CHECK(w.c[kList * kShapes + kIndexedSmall].whole == 0);
        CHECK(w.c[kList * kShapes + kIndexedSmall].nonPos == 1);
        // per attribute of the matched draws: pos A, D, D same (3) + B different + C different (indices) = 3 of 5
        CHECK(w.kindCmp[kPos] == 5 && w.kindSame[kPos] == 3);
        CHECK(w.kindCmp[kClr] == 3 && w.kindSame[kClr] == 3);
        CHECK(w.kindCmp[kNrm] == 2 && w.kindSame[kNrm] == 1);    // (C's indices changed)
        CHECK(w.arrCmp[kNrm] == 2 && w.arrSame[kNrm] == 2);      // (both normal ranges the same bytes)
        CHECK(w.arrCmp[kPos] == 2 && w.arrSame[kPos] == 1);
        CHECK(w.arrCmp[kTex] == 2 && w.arrSame[kTex] == 2 && w.arrMoved[kTex] == 1);
        CHECK(w.kindCmp[kMtxIdx] == 2 && w.kindSame[kMtxIdx] == 1); // (C's matrix index bytes changed with its seed)
        CHECK(w.cmpNs == 0 && w.keepNs == 0 && w.copyNs == 0);    // (the fake clock's reads: all clock cost)
        CHECK(w.overCap == 0 && w.noKept == 0);
        CHECK(w.needBytes == 64 + (168 + 768) * 2 + 64 * 3);
    }
    c.EndPicture();
    CHECK(c.Lines(lines, 6) >= 4);

    // ---- picture 3: stride or byte order or length of an array changes with the same bytes: not identical; D's two draws swapped with
    // another quad of D's signature in between (the ordinal moves: key A finds other draws, key B still finds them)
    {
        DrawIn b = Mesh(ms, 24, 0xB, pos.data(), 288, nrm.data(), 288, texMoved.data(), 192);
        b.a[2].stride = 16;
        c.Note(b);
        DrawIn b2 = Mesh(ms2b, 24, 0xB, pos2.data(), 288, nrm2.data(), 288, tex2.data(), 192);
        b2.a[3].le = true;
        c.Note(b2);
        const auto qx = Bytes(64, 77);
        c.Note(Quad(qx, 0xD, kFx));
        c.Note(Quad(qd, 0xD, kFx));
        c.Note(Quad(qd, 0xD, kFx));
        const Census::Cls t = c.Total();
        CHECK(t.matched == 4);          // (the two B, the first two D; the third D has no third of the picture before)
        CHECK(t.whole == 1);            // (the second D now sits where the second D was)
        CHECK(t.seenPrev == 2);         // (the two qd)
        CHECK(t.nonPos == 0);
        CHECK(c.Window().arrSame[kNrm] == 1 && c.Window().arrSame[kTex] == 1);
    }
    c.EndPicture();
    c.Lines(lines, 6);

    // ---- the cap: a census of 4 KiB keeps nothing over 512 bytes a draw; such a draw is not compared next time
    {
        Census s(&FakeNow, 4096);
        const auto big = Bytes(600, 3);
        for (int pic = 0; pic < 2; ++pic) {
            DrawIn d = Quad(qa, 0xA);
            s.Note(d);
            DrawIn m = Mesh(ms, 24, 0xB, big.data(), 600, nrm.data(), 288, tex.data(), 192);
            s.Note(m);
            if (pic == 1) {
                CHECK(s.Window().overCap == 2);
                CHECK(s.Window().noKept == 1);
                CHECK(s.Total().matched == 2);
                CHECK(s.Total().whole == 1);
            }
            s.EndPicture();
        }
    }

    // ---- times: raw differences with scopes take the calibrated clock cost off, never below 0
    {
        Census s(&FakeNow);
        DrawIn d = Quad(qa, 0xA);
        d.decodeNs = 50; d.decodeScopes = 2; d.emitNs = 3; d.emitScopes = 1;
        s.Note(d);
        CHECK(s.Total().decNs == 36);
        CHECK(s.Total().emitNs == 0);
    }

    // ---- the lines: present, short of the log's limit, the figures per picture
    {
        Census s(&FakeNow);
        for (int pic = 0; pic < 4; ++pic) {
            s.Note(Quad(qa, 0xA));
            s.Note(Mesh(ms, 24, 0xB, pos.data(), 288, nrm.data(), 288, tex.data(), 192));
            s.EndPicture();
        }
        char l[6][1000];
        const int n = s.Lines(l, 6);
        CHECK(n == 4);
        bool found = false;
        for (int i = 0; i < n; ++i) {
            CHECK(std::strlen(l[i]) < 1000);
            if (std::strstr(l[i], "4 pictures; a picture: stream draws 2.0 (28 vertices)") && std::strstr(l[i], "whole draws 1.5")) found = true;
            std::printf("%s\n", l[i]);
        }
        CHECK(found);
        CHECK(s.Lines(l, 6) == 2); // (an empty window: the two total lines only)
    }
    if (g_fail) { std::printf("vgx_deccen: %d checks FAILED\n", g_fail); return 1; }
    std::printf("vgx_deccen: all checks passed\n");
    return 0;
}
