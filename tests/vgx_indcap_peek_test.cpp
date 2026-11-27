// GXV_INDCAP_PEEK (peek25): the pure rule on hand-made buffers (vgx_indcap_peek.h). tools/test_vgx_indcap_peek.sh.
#include "vgx_indcap_peek.h"
#include <cstdio>
using namespace vgx;
namespace {
unsigned failures = 0;
void Check(bool ok, const char* name) { std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }
void Cap(CmdBuf& b, uint32_t tex, bool transient = true) {
    CopyTexCmd c; c.texId = tex; c.transient = transient; b.copyTex.push_back(c);
    b.cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)});
}
void Draw(CmdBuf& b, uint32_t tex) {
    DrawRec d; d.texMask = 1; d.tex[0].texId = tex; b.draws.push_back(d);
    b.cmds.push_back(Cmd{CMD_DRAW, static_cast<uint32_t>(b.draws.size() - 1)});
}
void Atlas(CmdBuf& b, uint32_t src, uint32_t dst) { CopyTexCmd c; c.srcTex = src; c.texId = dst; b.copyTex.push_back(c); b.cmds.push_back(Cmd{CMD_COPY_TEX, static_cast<uint32_t>(b.copyTex.size() - 1)}); }
unsigned Run(CmdBuf& cur, const CmdBuf* next, bool apply, indcappeek::Counters& k) { indcappeek::Shadow s; return indcappeek::Decide(cur, next, apply, k, s); }
}
int main() {
    { CmdBuf a, b; Cap(a, 7); Draw(a, 9); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 1 && a.copyTex[0].skip && k.viaNext == 1, "no reader here, the next buffer overwrites first: left out"); }
    { CmdBuf a, b; Cap(a, 7); Draw(b, 7); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && !a.copyTex[0].skip && k.nextReads == 1, "the next buffer reads before it writes: kept"); }
    { CmdBuf a, b; Cap(a, 7); Draw(a, 7); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.readerHere == 1, "a reader after it in its own picture: kept"); }
    { CmdBuf a; Cap(a, 7); indcappeek::Counters k;
      Check(Run(a, nullptr, true, k) == 0 && k.noNext == 1, "no next buffer queued: kept"); }
    { CmdBuf a, b; Cap(a, 7); Draw(b, 9); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.nextNoWrite == 1, "the next buffer never writes the texture: kept"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 8); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 1, "a write of another texture first does not matter"); }
    { CmdBuf a, b; Cap(a, 7); Atlas(b, 7, 8); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.nextReads == 1, "an atlas copy out of the texture is a reader"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); b.copyTex[0].skip = true; indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.nextNoWrite == 1, "the next copy is itself left out: not an overwrite"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); b.copyTex[0].nzOn = 1; indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0, "a copy of part of the texture is not a whole overwrite"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); b.copyTex[0].dstTexW = 8; indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0, "an atlas cell write is not a whole overwrite"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); b.hrwId = 3; indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.noNext == 1, "the next buffer opens an HRW pair: kept"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); b.hrwReplayId = 3; indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0, "the next buffer is a replay ask: kept"); }
    { CmdBuf a, b; Cap(a, 7); Cap(a, 7); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 2 && k.deadOwn == 1 && k.viaNext == 1, "two captures in a row: the first is dead in its own picture, the second via the next"); }
    { CmdBuf a, b; Cap(a, 7); a.copyTex[0].fromPass = true; Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.refusedShape == 1, "a copy that ends a pass is not the plain capture: kept"); }
    { CmdBuf a, b; Cap(a, 7, false); Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 0 && k.captures == 0, "a copy the game did not mark is not touched"); }
    { CmdBuf a, b; Cap(a, 7); a.copyTex[0].swapOk = true; Cap(b, 7); indcappeek::Counters k;
      Check(Run(a, &b, true, k) == 1 && !a.copyTex[0].swapOk, "a planned EFBSWAP2 hand-over is cancelled with the copy"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); indcappeek::Counters k; indcappeek::Shadow s;
      Check(indcappeek::Decide(a, &b, false, k, s) == 1 && !a.copyTex[0].skip, "count only: nothing is changed");
      Check(indcappeek::Verify(b, s, k) && k.shadowChecks == 1 && k.shadowBad == 0, "shadow check: the next buffer's first use is a whole write"); }
    { CmdBuf a, b; Cap(a, 7); Cap(b, 7); indcappeek::Counters k; indcappeek::Shadow s;
      indcappeek::Decide(a, &b, false, k, s);
      b.cmds.insert(b.cmds.begin(), Cmd{CMD_DRAW, 0}); DrawRec d; d.texMask = 1; d.tex[0].texId = 7; b.draws.push_back(d); // (the buffer changed after it was looked at)
      Check(!indcappeek::Verify(b, s, k) && k.shadowBad == 1, "shadow check: a read that appeared first is caught"); }
    std::printf(failures ? "FAILED\n" : "all passed\n");
    return failures ? 1 : 0;
}
