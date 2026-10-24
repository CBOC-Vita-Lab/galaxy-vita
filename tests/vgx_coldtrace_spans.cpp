#include "../platform/vgx/src/vgx_coldtrace.h"
#include <cstdio>
#include <cstdlib>

#if VGX_COLDTRACE_DETAIL_VERSION >= 3
#include <optional>
#include <thread>

namespace {
using namespace vgx::coldtrace;
uint64_t now = 0;
thread_local uint32_t threadId = 1;
uint64_t Clock() noexcept { return now; }
uint32_t Thread() noexcept { return threadId; }
void Require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
size_t Id(Phase phase) { return static_cast<size_t>(phase); }
void Partition(const Span& span) {
    uint64_t phases = span.selfUs;
    for (uint64_t value : span.exclusiveUs) phases += value;
    Require(phases == span.rootUs && span.rootUs + span.unclassifiedUs == span.totalUs,
            "root phases, root self and unclassified gaps partition the complete span");
    uint64_t origins = 0;
    for (uint64_t value : span.originUs) origins += value;
    Require(origins == span.rootUs, "origins partition the roots without double-counting nested requests");
}
void CompleteSlice() {
    Recorder recorder;
    Require(recorder.Arm(Clock, Thread, 1), "arm complete-span fixture");
    {
        SpanScope execute(recorder, SpanKind::Execute, 77);
        now = 1000;
        { Scope stream(recorder, Key{77, 0x10002}, Origin::Stream); now = 2000; }
        now = 5000;
        {
            SpanScope warm(recorder, SpanKind::WarmSlice);
            now = 6000;
            {
                Scope program(recorder, Key{77, 2, 4, 5}, Origin::Queued);
                { Scope nested(recorder, Key{77, 1, 99}, Origin::DirectShader); }
                now = 7000;
                {
                    Scope compile(recorder, Phase::Compile);
                    now = 8000;
                    { Scope read(recorder, Phase::Read, IoClass::Read); read.Result(64, 64); now = 58000; }
                    { Scope shark(recorder, Phase::Shark); now = 158000; }
                    now = 168000;
                }
                now = 180000;
            }
            {
                Scope variant(recorder, Key{77, 3, 4, 5, 6, 7}, Origin::Queued);
                { Scope draw(recorder, Phase::Draw);
                  { Scope patch(recorder, Phase::PatchFragment); now = 207000; }
                  now = 208000; }
                now = 210000;
            }
            now = 213000;
        }
        {
            Scope upload(recorder, Key{77, 4, 123}, Origin::Texture);
            now = 214000;
            { Scope wait(recorder, Phase::TransferFinish); wait.Result(0); now = 218000; }
            now = 219000;
        }
        now = 220000;
    }
    // Evict every root of that slice from the separate bounded root set.
    for (unsigned i = 0; i < kWorstItems; ++i) {
        Scope root(recorder, Key{99, 5, i}, Origin::DirectProgram); now += 300000;
    }
    const uint64_t idleStart = now;
    { SpanScope idle(recorder, SpanKind::WarmSlice);
      { Scope shader(recorder, Key{77, 1, 88}, Origin::Queued); now += 10; } }
    recorder.RequestFreeze();
    const auto view = recorder.Frozen();
    Require(view.data && view.data->executeCount == 1 && view.data->sliceCount == 2,
            "complete execution and both warm slices retained independently");
    const auto& execute = view.data->worstExecutions[0];
    const auto& warm = view.data->worstSlices[0];
    Partition(execute); Partition(warm); Partition(view.data->worstSlices[1]);
    Require(warm.totalUs == 208000 && warm.rootUs == 204000 && warm.unclassifiedUs == 4000,
            "complete 208ms slice includes all roots and its gaps");
    Require(warm.serial == 2 && warm.parentSerial == execute.serial && warm.frame == 77,
            "slice parent is the explicit execution serial, not a stale frame counter");
    Require(warm.roots == 2 && warm.maxItem.totalUs == 174000 && warm.maxItem.key.a == 4,
            "slowest root detail survives even after the separate worst-root set evicts it");
    Require(warm.exclusiveUs[Id(Phase::Compile)] == 11000 && warm.exclusiveUs[Id(Phase::Read)] == 50000 &&
            warm.exclusiveUs[Id(Phase::Shark)] == 100000 && warm.exclusiveUs[Id(Phase::PatchFragment)] == 27000 &&
            warm.exclusiveUs[Id(Phase::Draw)] == 1000 && warm.selfUs == 15000,
            "slice sums every completed root's non-overlapping phases, including nested draw patching");
    Require(execute.totalUs == 220000 && execute.rootUs == 211000 && execute.unclassifiedUs == 9000 &&
            execute.exclusiveUs[Id(Phase::TransferFinish)] == 4000,
            "execution distinguishes texture synchronization from shader work and gaps");
    Require(execute.originUs[static_cast<size_t>(Origin::Texture)] == 6000 &&
            execute.originUs[static_cast<size_t>(Origin::Stream)] == 1000,
            "upload and stream work have independent complete-frame totals");
    const auto& idle = view.data->worstSlices[1];
    Require(idle.startUs == idleStart && idle.parentSerial == 0 && idle.frame == 0 && idle.totalUs == 10,
            "WarmIdle outside Execute has no invented frame or execution parent");
    for (unsigned i = 0; i < view.data->worstCount; ++i)
        Require(view.data->worst[i].totalUs == 300000, "fixture really evicted all original roots");
    std::puts("PASS: complete208ms slice, explicit parentage, gaps, upload wait and independent worst retention");
}
void OutsideCallsAndFreeze() {
    Recorder recorder; now = 0;
    Require(recorder.Arm(Clock, Thread, 1), "arm outside-root fixture");
    std::optional<SpanScope> execute;
    execute.emplace(recorder, SpanKind::Execute, 9);
    now = 2;
    { Scope wait(recorder, Phase::TransferFinish); wait.Result(-17); now = 9; }
    recorder.RequestFreeze();
    Require(!recorder.Frozen().data && recorder.PendingPermits() == 1,
            "a span permit prevents publishing between completed roots");
    { SpanScope rejected(recorder, SpanKind::WarmSlice); }
    now = 12; execute.reset();
    const auto view = recorder.Frozen();
    Require(view.data && view.data->phasesOutsideItems == 1 && view.data->worstCount == 1,
            "owner SDK calls outside explicit roots are timed, not silently discarded");
    const auto& item = view.data->worst[0];
    Require(item.totalUs == 7 && item.exclusiveUs[Id(Phase::TransferFinish)] == 7 &&
            item.origin == Origin::Sdk && item.firstError[Id(Phase::TransferFinish)] == -17,
            "standalone transfer wait retains wall time, origin and SDK error");
    const auto& span = view.data->worstExecutions[0];
    Partition(span);
    Require(span.flags & kFrozenMidItem, "freeze during a span marks incomplete phase admission");
    Require(span.totalUs == 12 && span.rootUs == 7 && span.unclassifiedUs == 5,
            "standalone calls contribute once to the surrounding span");
    std::puts("PASS: unrooted SDK timing and freeze publication across root-free gaps");
}
void RetentionAndMisuse() {
    Recorder recorder; now = 0;
    Require(recorder.Arm(Clock, Thread, 1), "arm span retention");
    for (unsigned i = 0; i < kWorstSpans + 7; ++i) {
        SpanScope execute(recorder, SpanKind::Execute, i);
        now += i == 0 ? 208000 : 10 + i;
    }
    recorder.RequestFreeze(); auto view = recorder.Frozen();
    Require(view.data->executionsSeen == kWorstSpans + 7 && view.data->executeCount == kWorstSpans,
            "execution retention stays bounded while observed count remains complete");
    bool found = false;
    for (const auto& span : view.data->worstExecutions) { Partition(span); found |= span.frame == 0 && span.totalUs == 208000; }
    Require(found, "worst execution survives later short spans");
    now = 0; Require(recorder.Arm(Clock, Thread, 1), "rearm span misuse fixture");
    {
        SpanScope execute(recorder, SpanKind::Execute, 1);
        { SpanScope duplicate(recorder, SpanKind::Execute, 2); now = 1; }
        { Scope root(recorder, Key{}, Origin::Texture);
          { SpanScope invalid(recorder, SpanKind::WarmSlice); now = 2; } }
        threadId = 2; { SpanScope foreign(recorder, SpanKind::WarmSlice); } threadId = 1;
        now = 3;
    }
    recorder.RequestFreeze(); view = recorder.Frozen();
    Require(view.data->spanErrors == 2 && view.foreignDropped == 1 && view.data->sliceCount == 0,
            "invalid nesting and foreign spans are explicit without fabricating timing records");
    Require(view.data->worstExecutions[0].flags & kMalformedScopes,
            "a nesting error prevents trusting the enclosing partition as complete attribution");
    std::puts("PASS: bounded span retention and malformed nesting coverage");
}
}
int main() { CompleteSlice(); OutsideCallsAndFreeze(); RetentionAndMisuse(); }
#else
int main() { std::fputs("FAIL: recorder cannot retain complete warm slices, execution gaps or transfer waits\n", stderr); return 1; }
#endif
