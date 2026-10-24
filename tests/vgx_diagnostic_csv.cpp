#include "../platform/vgx/src/vgx_diagnostic_csv.h"
#include <cassert>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using vgx::diagnosticcsv::Buffer;
using vgx::diagnosticcsv::Row;
static_assert(std::is_trivially_destructible_v<vgx::diagnosticcsv::Log>);
namespace {
unsigned opens = 0, writes = 0, closes = 0, removes = 0, allocations = 0;
bool failAllocation = false, failOpen = false, failClose = false, failWrite = false, zeroWrite = false, interruptWrite = false;
bool blockWrite = false;
std::atomic<bool> writeEntered{false}, releaseWrite{false};
size_t shortLimit = 0;
std::string output;
void ResetIo() {
    opens = writes = closes = removes = 0;
    failOpen = failClose = failWrite = zeroWrite = interruptWrite = false;
    shortLimit = 0;
    output.clear();
}
Row Line(unsigned n) { Row row; row.Add("%u,value\n", n); return row; }
void Require(bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
}
extern "C" void* __real_malloc(size_t);
extern "C" void* __wrap_malloc(size_t bytes) {
    ++allocations;
    return failAllocation ? nullptr : __real_malloc(bytes);
}
extern "C" int __wrap_open(const char*, int flags, ...) {
    ++opens;
    Require((flags & O_EXCL) && (flags & O_CREAT), "exports never truncate existing captures");
    if (failOpen) { errno = EEXIST; return -1; }
    return 123;
}
extern "C" ssize_t __wrap_write(int file, const void* data, size_t size) {
    Require(file == 123, "only explicit export reaches the writer");
    ++writes;
    if (blockWrite) {
        writeEntered.store(true, std::memory_order_release);
        while (!releaseWrite.load(std::memory_order_acquire)) std::this_thread::yield();
    }
    if (interruptWrite) { interruptWrite = false; errno = EINTR; return -1; }
    if (failWrite) { errno = EIO; return -1; }
    if (zeroWrite) return 0;
    if (shortLimit && size > shortLimit) size = shortLimit;
    output.append(static_cast<const char*>(data), size);
    return size;
}
extern "C" int __wrap_close(int file) {
    Require(file == 123, "export closes its own file");
    ++closes;
    if (failClose) { errno = EIO; return -1; }
    return 0;
}
extern "C" int __wrap_unlink(const char*) { ++removes; return 0; }

int main() {
    static Buffer normal, overflow, invalid, allocationFailure, existing, partial, zero, closeFailure, busy, empty;
    static Buffer badPaths[5];
    static Buffer concurrent[64];
    ResetIo();
    const std::string longPath(256,'x');
    const auto beforeInvalid=allocations;
    Require(!badPaths[0].Arm(nullptr) && !badPaths[1].Arm("") && !badPaths[2].Arm(longPath.c_str()) &&
            !badPaths[3].Arm("zero.csv",0) && !badPaths[4].Arm("large.csv",Buffer::kCapacity+1),"bad paths and capacities are rejected");
    Require(allocations==beforeInvalid,"rejected capture configuration does not allocate");
    Require(empty.Arm("empty.csv",16) && empty.Append(Row{}) && empty.Dropped()==0 && empty.Append(Line(0)),
            "empty formatting fragments are harmless no-ops");
    Require(normal.Export() == 0 && opens == 0, "empty pause does not consume capture");
    Require(normal.Arm("capture.csv"), "capture arms");
    std::string expected;
    expected.reserve(20000);
    const unsigned before = allocations;
    for (unsigned n = 0; n < 1000; ++n) {
        const auto row = Line(n);
        Require(normal.Append(row), "ordinary row is retained");
        expected.append(row.data, row.size);
    }
    Require(allocations == before && opens == 0 && writes == 0 && closes == 0,
            "1000 active frames do no allocation or filesystem I/O after arm");
    shortLimit = 7; interruptWrite = true;
    Require(normal.Export() == 0 && output == expected && opens == 1 && closes == 1,
            "paused export preserves exact bytes through EINTR and short writes");
    const auto writeCount = writes;
    Require(!normal.Append(Line(1000)) && normal.Export() == 0 && writes == writeCount && opens == 1,
            "resume/second pause never rearms or writes again");

    ResetIo();
    Require(overflow.Arm("overflow.csv", 16), "tiny capture arms");
    Require(overflow.Append(Line(0)) && overflow.Append(Line(1)), "whole rows fit the cap");
    Require(!overflow.Append(Line(2)), "overflow drops a whole row");
    Row smaller; smaller.Add("x\n");
    Require(!overflow.Append(smaller) && overflow.Dropped() == 2, "full capture latches a contiguous prefix");
    Require(overflow.Export() == 0 && output == "0,value\n1,value\n", "overflow export has no partial row");

    ResetIo();
    Require(invalid.Arm("invalid.csv", 16), "invalid-row capture arms");
    Row tooLarge; tooLarge.Add("%05000d\n", 1);
    Require(!invalid.Append(tooLarge) && !invalid.Append(Line(0)) && invalid.Committed() == 0 && invalid.Dropped() == 2,
            "format truncation never publishes a partial header or later rows");
    Require(invalid.Export() < 0 && invalid.Frozen() && opens == 0, "invalid header reports failure without creating an empty file");
    failAllocation = true;
    const auto tries = allocations;
    Require(!allocationFailure.Arm("oom.csv") && !allocationFailure.Arm("oom.csv"), "allocation failure is latched");
    failAllocation = false;
    Require(allocations == tries + 1 && !allocationFailure.Append(Line(0)) && allocationFailure.Export() == 0 && opens == 0,
            "allocation failure never falls back to file writes or retries per frame");

    for (unsigned kind = 0; kind < 4; ++kind) {
        ResetIo();
        Buffer* capture[] = {&existing, &partial, &zero, &closeFailure};
        Require(capture[kind]->Arm("fault.csv") && capture[kind]->Append(Line(0)), "fault fixture arms");
        failOpen = kind == 0; failWrite = kind == 1; zeroWrite = kind == 2; failClose = kind == 3;
        Require(capture[kind]->Export() < 0 && capture[kind]->Frozen(), "export errors freeze the capture and stay visible");
        Require(removes == (kind == 0 ? 0u : 1u), "only a newly created incomplete file is removed");
        capture[kind]->Export();
        Require(opens == 1, "failed export is not retried automatically");
    }

    ResetIo();
    Require(busy.Arm("busy.csv") && busy.Append(Line(0)), "busy fixture arms");
    blockWrite = true;
    std::thread exporter([&] { Require(busy.Export() == 0, "first consumer finishes its export"); });
    while (!writeEntered.load(std::memory_order_acquire)) std::this_thread::yield();
    Require(busy.Export() == -EBUSY, "second consumer cannot mistake an in-flight export for success");
    releaseWrite.store(true, std::memory_order_release); exporter.join(); blockWrite = false;
    Require(opens == 1 && closes == 1, "concurrent export callers still create exactly one file");

    for (unsigned round = 0; round < 64; ++round) {
        ResetIo();
        auto& capture = concurrent[round];
        Require(capture.Arm("threaded.csv", 65536), "concurrent fixture arms");
        std::atomic<bool> started{false}, done{false};
        std::thread producer([&] {
            for (unsigned n = 0; n < 4000; ++n) {
                capture.Append(Line(n));
                if (n == 0) started.store(true, std::memory_order_release);
            }
            unsigned n=4000;
            while (!capture.Frozen()) { capture.Append(Line(n++)); std::this_thread::yield(); }
            done.store(true,std::memory_order_release);
        });
        while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
        Require(!done.load(std::memory_order_acquire),"producer is still running when export begins");
        Require(capture.Export() == 0, "concurrent paused export succeeds without draining/spinning on producer");
        producer.join();
        Require(capture.ExportedBytes()==output.size(),"export metadata describes the exported prefix, not a late commit");
        size_t at = 0;
        unsigned number = 0;
        while (at < output.size()) {
            const auto expectedRow = Line(number++);
            Require(output.compare(at, expectedRow.size, expectedRow.data, expectedRow.size) == 0,
                    "concurrent export is a consecutive prefix of complete rows");
            at += expectedRow.size;
        }
        Require(number && at == output.size(), "concurrent report is nonempty and ends at a row boundary");
    }
    ResetIo();
    setenv("GXV_STATS_RAM","1",1);
    static vgx::diagnosticcsv::Log log;
    log.Close();
    log.Open("log.csv"); const auto afterOpen=allocations; log.Open("ignored.csv");
    Require(log.Enabled() && allocations==afterOpen,"Log opens only once");
    log.Append(Line(0)); log.FlushLive();
    Require(opens==0 && writes==0 && closes==0,"Log wrapper never leaks RAM rows into live stdio");
    Require(log.ExportPaused()==0 && !log.Enabled() && output=="0,value\n","Log exports a complete immutable prefix only on pause");
    log.Close(); log.Append(Line(1));
    Require(opens==1 && closes==1 && output=="0,value\n","Log close/late append never re-exports a paused capture");
    setenv("GXV_STATS_RAM","0",1);
#if defined(__vita__) || defined(__VGX_VITA__)
    Require(vgx::diagnosticcsv::RamEnabled(),"Vita ignores legacy settings that would restore blocking CSV output");
#else
    Require(!vgx::diagnosticcsv::RamEnabled(),"host workflows may explicitly retain live CSV output");
    unsetenv("GXV_STATS_RAM");
    Require(!vgx::diagnosticcsv::RamEnabled(),"ordinary desktop default stays compatible");
#endif
    std::puts("PASS: bounded CSV, 1000 I/O-free frames, overflow/OOM/errors, 64 concurrent prefix exports");
}
