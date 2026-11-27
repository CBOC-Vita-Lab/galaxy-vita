// GXV_DIRECT_J3D (vgx_direct.h): a retained shape call's emission with its key template (included into vgx_dlcache.cpp, namespace vgx, after
// the retained-shape records). The template (RtSlot::dj, in the record's last cache line, which the hit has just read) only says which
// shader ids the keys hash to; dj::Resolve checks it against the registers of every run, so a template of another shape, another material
// or another picture can only make that run resolve the normal way. (A record made again for another shape keeps the template it had, which
// then only makes its runs resolve the normal way until the first call replaces it.)
namespace {
// The calls whose runs EmitStaticRun resolves: a static buffer, not a learned copy (Emit's other branches copy into the arena).
inline bool DjEligible(const Entry& e) { return e.bufId != 0 && !(e.copyable && g_learnMax != 0) && !e.runs.empty(); }

void DjEmit(RtSlot& s, const uint8_t* list, Entry& e, uint32_t nbytes) {
    if (dj::Mode() == 0 || !DjEligible(e) || !dj::Usable()) {
        // (fe27 census: why this call is not armed, the first reason; four counters, read by the "direct J3D not armed" line)
        dj::CountNotArmed(e.bufId == 0 && !e.copyable ? 0u : e.copyable && g_learnMax != 0 ? 1u : e.bufId != 0 && e.runs.empty() ? 2u : 3u);
        Emit(e, nbytes);
        return;
    }
    const uint32_t before = dj::Resolves();
    const uint64_t other = dj::OtherRuns();
    const bool had = s.dj.valid;
    if (had) {
        dj::g_armed = &s.dj;
        if (dj::Mode() == 2) { dj::g_armedHandle = s.handle; dj::g_armedList = list; } // (the verify log's names)
        dj::CountArmed();
        Emit(e, nbytes);
        dj::g_armed = nullptr;
    } else {
        Emit(e, nbytes);
    }
    // (the keys the call's last run resolved to: the template of the next call, unless it is the one it has: every run took it, or had the
    // keys of the run before, which have its key hash then)
    if ((!had || dj::OtherRuns() != other) && dj::Capture(s.dj, before)) dj::CountCapture();
}
} // namespace
