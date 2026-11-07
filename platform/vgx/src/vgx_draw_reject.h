// Included only in the backend's render-owner namespace when explicitly compiled on.
// These are Draw entry/return observations, not proof of native draw success or actor identity.
enum DrawRejectReason : unsigned { RejectSkip, RejectProgram, RejectStatic, RejectStream, RejectSanitize, RejectState, RejectCount };
struct DrawRejectExample {
    uint64_t frame = 0;
    uint32_t vs = 0, fs = 0, vbuf = 0, vchunk = 0, indices = 0;
};
struct DrawRejectCounters {
    uint64_t nonempty = 0, rejected = 0, submitted = 0;
    uint64_t reason[RejectCount]{}, programState[7]{};
    DrawRejectExample first[RejectCount]{};
    bool overflow = false;
};
DrawRejectCounters g_drawReject;
inline void DrawRejectAdd(uint64_t& n) {
    if (n == UINT64_MAX) g_drawReject.overflow = true;
    else ++n;
}
inline void DrawRejectNote(DrawRejectReason reason, const CmdBuf& buf, const DrawRec& r) {
    if (!r.idxCount) return;
    if (!g_drawReject.reason[reason]) {
        g_drawReject.first[reason] = {buf.frame, r.vsId, r.fsId, r.vbuf, r.vchunk, r.idxCount};
    }
    DrawRejectAdd(g_drawReject.reason[reason]);
    DrawRejectAdd(g_drawReject.rejected);
}
inline void DrawRejectProgramState(const DrawRec& r, const Program& p) {
    if (!r.idxCount) return;
    // Overlapping observed states, not mutually exclusive root-cause attribution.
    const bool allocation = GlAllocBackoff();
    const bool compile = (r.vsId < g_vsAttempts.size() && g_vsAttempts[r.vsId].failures) ||
                         (r.fsId < g_fsAttempts.size() && g_fsAttempts[r.fsId].failures);
    const bool link = p.failures || p.id;
    const bool missing = r.vsId >= g_vsSrc.size() || !g_vsSrc[r.vsId] || r.fsId >= g_fsSrc.size() || !g_fsSrc[r.fsId];
    const bool pending = (r.vsId < g_vsPending.size() && g_vsPending[r.vsId]) ||
                         (r.fsId < g_fsPending.size() && g_fsPending[r.fsId]);
    const bool exhausted = (r.vsId < g_vsAttempts.size() && g_vsAttempts[r.vsId].failures >= kGlMaxAttempts) ||
                           (r.fsId < g_fsAttempts.size() && g_fsAttempts[r.fsId].failures >= kGlMaxAttempts) ||
                           p.failures >= kGlMaxAttempts;
    const bool states[] = {allocation, compile, link, missing, pending, exhausted,
                           !(allocation || compile || link || missing || pending || exhausted)};
    for (unsigned i = 0; i < 7; ++i) {
        if (states[i]) DrawRejectAdd(g_drawReject.programState[i]);
    }
}
