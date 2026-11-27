// across26 (12 Nov 2026): the occlusion-query glue of GXV_SILH_OCC (logic: vgx_silhocc.h). Included by vgx_backend_gl.cpp inside namespace vgx after the GL
// declarations. The GL calls exist on the Vita (vitaGL: glGenQueries / glBeginQuery(GL_SAMPLES_PASSED) / glEndQuery / glGetQueryObjectuiv) and in the host
// test (mock functions, VGX_SILHOCC_MOCKGL); on any other build the glue is inert (Active() false, nothing is called).
//
// Use. Before every command, Step(buf, cmd): a draw flagged DrawRec::occ opens a query (once per run); the first command that is not such a draw and not a
// resource-only command closes it. Present(): the query still open is closed, then every pending query is polled WITHOUT waiting (RESULT_AVAILABLE first,
// then RESULT: vitaGL's wait path, which marks the scene dirty, is never taken) and the count goes to the history. Six slots; a picture whose run found no
// free slot is simply not measured (the front end then fails closed after `lagMax` pictures).
#pragma once

#include <cstdint>
#include "vgx_cmd.h"
#include "vgx_silhocc.h"

#if defined(__VGX_VITA__) || defined(VGX_SILHOCC_MOCKGL)
#define VGX_SILHOCC_HAVE_GL 1
#else
#define VGX_SILHOCC_HAVE_GL 0
#endif

#if defined(__VGX_VITA__) && !defined(VGX_SILHOCC_MOCKGL)
// vitaGL's occlusion queries (buffers.c; the GLES2 headers do not declare them). Weak: a library without them leaves the glue inert.
extern "C" {
void glGenQueries(int n, unsigned int* ids) __attribute__((weak));
void glBeginQuery(unsigned int target, unsigned int id) __attribute__((weak));
void glEndQuery(unsigned int target) __attribute__((weak));
void glGetQueryObjectuiv(unsigned int id, unsigned int pname, unsigned int* params) __attribute__((weak));
}
#endif
#if VGX_SILHOCC_HAVE_GL
#ifndef GL_SAMPLES_PASSED
#define GL_SAMPLES_PASSED 0x8914
#endif
#ifndef GL_QUERY_RESULT
#define GL_QUERY_RESULT 0x8866
#endif
#ifndef GL_QUERY_RESULT_AVAILABLE
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#endif
#endif

namespace vgx {
namespace silhocc {

struct Glue {
    static constexpr int kSlots = 6;
    struct Slot { unsigned q = 0; uint64_t frame = 0; bool pending = false; };
    Slot slot[kSlots];
    int open = -1;
    bool made = false, on = false;
    uint32_t minPx = 1;
    uint64_t began = 0, ended = 0, polled = 0, noSlot = 0, forced = 0;

    bool Active() const { return on && VGX_SILHOCC_HAVE_GL; }
    void Enable(bool v, uint32_t px) {
        if (!v && open >= 0) End();
        on = v;
        minPx = px ? px : 1u;
    }

    static bool Neutral(CmdType t) {
        return t == CMD_SHADER || t == CMD_PROGRAM || t == CMD_VARIANT || t == CMD_TEX_UPLOAD || t == CMD_BUF_UPLOAD || t == CMD_TEX_FREE || t == CMD_BUF_FREE;
    }
#if VGX_SILHOCC_HAVE_GL
#if defined(VGX_SILHOCC_MOCKGL)
    static bool HaveGl() { return true; }
#else
    static bool HaveGl() { return &glGenQueries && &glBeginQuery && &glEndQuery && &glGetQueryObjectuiv; }
#endif
    void Make() {
        if (made) return;
        made = true;
        if (!HaveGl()) return; // (no queries in this vitaGL: no slot gets a query, nothing is counted)
        for (Slot& s : slot) glGenQueries(1, &s.q);
    }
    void Begin(uint64_t frame) {
        Make();
        for (int i = 0; i < kSlots; ++i)
            if (!slot[i].pending && slot[i].q) {
                glBeginQuery(GL_SAMPLES_PASSED, slot[i].q);
                slot[i].frame = frame;
                open = i;
                ++began;
                return;
            }
        ++noSlot;
    }
    void End() {
        if (open < 0) return;
        glEndQuery(GL_SAMPLES_PASSED);
        slot[open].pending = true;
        open = -1;
        ++ended;
    }
    void Poll(History& h) {
        for (Slot& s : slot) {
            if (!s.pending) continue;
            unsigned av = 0;
            glGetQueryObjectuiv(s.q, GL_QUERY_RESULT_AVAILABLE, &av);
            if (!av) continue;
            unsigned n = 0;
            glGetQueryObjectuiv(s.q, GL_QUERY_RESULT, &n);
            s.pending = false;
            ++polled;
            h.OnResult(s.frame, n, minPx);
        }
    }
#else
    void Begin(uint64_t) {}
    void End() {}
    void Poll(History&) {}
#endif
    // Before command `c` of `buf` is executed.
    void Step(const CmdBuf& buf, const Cmd& c) {
        if (!on) return;
        const bool flagged = c.type == CMD_DRAW && buf.draws[c.arg].occ && buf.draws[c.arg].idxCount;
        if (flagged) {
            if (open < 0) Begin(buf.frame);
        } else if (open >= 0 && !Neutral(c.type)) {
            End();
        }
    }
    // At the picture's present.
    void Present(History& h) {
        if (!on) return;
        if (open >= 0) { ++forced; End(); }
        Poll(h);
        h.glueBegan.store(began, std::memory_order_relaxed); h.gluePolled.store(polled, std::memory_order_relaxed);
        h.glueNoSlot.store(noSlot, std::memory_order_relaxed); h.glueForced.store(forced, std::memory_order_relaxed);
    }
};

}  // namespace silhocc
}  // namespace vgx
