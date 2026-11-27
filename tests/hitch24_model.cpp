// hitch24b: the hitch classifier, the job journal and the stage summary (platform/include/port/hitch24.h), host only.
#include "port/hitch24.h"
#include <cassert>
#include <cstdio>
using namespace port::hitch24;
int main() {
    // 1. the 152 ms stage-start picture: one target made (62 ms), uploads 41, link 28
    Picture p; p.intervalMs = 152; p.beMs = 150; p.linkMs = 28; p.uploadMs = 41; p.otherMs = 62; p.targetsMade = 1;
    Verdict v = Classify(p);
    assert(v.cause == kTarget && v.second == kUpload);
    // 2. the same without a target made: back end other
    p.targetsMade = 0; v = Classify(p); assert(v.cause == kBackOther);
    // 3. the back end idle, the game thread's full segment waits are the cause (title screen)
    Picture t; t.intervalMs = 58; t.beMs = 9; t.segWaitMs = 36; t.gameMs = 40; t.frontMs = 30;
    v = Classify(t); assert(v.cause == kGame || v.cause == kSegWait);
    t.gameMs = 20; v = Classify(t); assert(v.cause == kSegWait);
    // 4. a journaled job that is most of the frame start is named
    Picture j; j.intervalMs = 80; j.beMs = 78; j.frameStartMs = 72; j.jobMs = 70; j.jobName = "stage queue build";
    v = Classify(j); assert(v.cause == kJob && v.second == kFrameStart);
    // 5. nothing explains it
    Picture u; u.intervalMs = 60; u.beMs = 10; v = Classify(u); assert(v.cause == kUnknown);
    // C186 retail-profile Honeyhive replay: the long present is a measured
    // host phase, not proof of a late GPU or a blocked game thread.
    Picture h; h.intervalMs = 126; h.beMs = 110; h.presentMs = 109; h.otherMs = 1; h.turnGapMs = 20;
    v = Classify(h); assert(!std::strcmp(CauseName(v.cause), "present path") && v.ms == 109 && v.second == kUpstream);
    h.intervalMs = 130; h.beMs = 105; h.presentMs = 104; h.otherMs = 0; h.turnGapMs = 21;
    v = Classify(h); assert(!std::strcmp(CauseName(v.cause), "present path") && v.ms == 104 && v.secondMs == 25);
    h.gpuWaitMs = 30; // This clock can overlap present; it does not make a 134 ms phase.
    v = Classify(h); assert(v.ms == 104 && v.second == kGpuWait);
    Picture paced; paced.intervalMs = 60; paced.beMs = 34; paced.presentMs = 33.37;
    v = Classify(paced); assert(v.cause == kUnknown);
    paced.turnGapMs = 17; v = Classify(paced); assert(v.cause == kUpstream && v.ms == 26);
    paced.beMs = 17; paced.presentMs = 16.68; paced.turnGapMs = 45;
    v = Classify(paced); assert(v.cause == kGame);
    char queue[96];
    FormatDisplayQueue(queue, sizeof queue, 0, 0.0); assert(std::strstr(queue, "n/a"));
    FormatDisplayQueue(queue, sizeof queue, 1, 0.0); assert(std::strstr(queue, "0.00 ms") && !std::strstr(queue, "n/a"));
    FormatDisplayQueue(queue, sizeof queue, 2, 95.5); assert(std::strstr(queue, "95.50 ms") && std::strstr(queue, "2 calls"));
    // 6. the journal
    const int64_t n = 1000000000; // deterministic origin for the synthetic job intervals
    JobNote("memory census", n, n + 60000000);
    const char* name = nullptr; int64_t ns = 0;
    assert(JobLongest(n + 10, n + 20, 5000000, &name, &ns) && ns == 60000000 && !std::strcmp(name, "memory census"));
    assert(!JobLongest(n + 90000000, n + 91000000, 5000000, &name, &ns));
    for (unsigned i = 0; i < 100; ++i) JobNote("x", n + 1000, n + 1001);  // the ring wraps
    assert(!JobLongest(n + 10, n + 20, 5000000, &name, &ns));
    // 7. the summary
    Summary s; s.Add(kTarget, 152, ""); s.Add(kTarget, 60, ""); s.Add(kLink, 45, "");
    char buf[400]; s.Format(buf, sizeof buf);
    assert(s.n == 3 && s.worstMs == 152 && std::strstr(buf, "render targets made 2 (212 ms)"));
    char tiny[20]; s.Format(tiny, sizeof tiny);
    std::puts(buf); std::puts("hitch24_model: ok");
    return 0;
}
