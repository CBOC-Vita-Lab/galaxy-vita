// Actual DVD APIs, worker, and IRQ path. Only host scheduling boundaries are controlled.
#include <array>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unistd.h>

static ssize_t proof_pread(int, void*, size_t, off_t);
#ifndef DVD_SOURCE
#define DVD_SOURCE "../platform/src/dvd/dvd.cpp"
#endif
#define pread proof_pread
#include DVD_SOURCE
#undef pread

#if DVD_HEAP_FIXTURE
void test_set_game_thread(bool);
#endif

namespace {
constexpr unsigned kCapacity = 1100;
struct WorkerFinished {};
struct Request {
    DVDFileInfo file{};
    uint64_t before = 0x0123456789abcdefULL;
    std::array<unsigned char, 32> bytes{};
    uint64_t after = 0xfedcba9876543210ULL;
    unsigned calls = 0;
};
std::array<Request, kCapacity> requests;
std::array<unsigned, kCapacity> retired{}, delivered{};
unsigned retiredCount, deliveredCount, checks, irqGoal, irqCount, cancelCalls;
bool syncPump, enqueueInCallback;
unsigned enqueueFrom, enqueueId;
std::mutex gateMutex;
std::condition_variable gateCv;
void* gateAddress;
bool gateReached, gateReleased;
unsigned gateHits;
std::array<const void*, kCapacity> pendingAddresses{};
unsigned pendingAddressCount, hostNodeFrees;

void require(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
unsigned idOf(const DVDCommandBlock* block) { return static_cast<unsigned>(reinterpret_cast<uintptr_t>(block->userData)) - 1; }
unsigned char pattern(unsigned offset) { return static_cast<unsigned char>((offset * 37u + 11u) & 255u); }
void checkBytes(unsigned id) {
    const Request& r = requests[id];
    require(r.before == 0x0123456789abcdefULL && r.after == 0xfedcba9876543210ULL, "destination guard bytes remain intact");
    for (unsigned i = 0; i < r.bytes.size(); ++i)
        require(r.bytes[i] == pattern((id % 8) * 32 + i), "exact destination payload is unchanged");
}
void submitRequest(unsigned id, int priority);
void completed(s32 result, DVDFileInfo* file) {
    const unsigned id = idOf(&file->cb);
    require(id < requests.size() && file == &requests[id].file, "callback retains original file-info lifetime and identity");
    require(result == 32 && file->cb.state == kStateEnd && file->cb.transferredSize == 32, "callback result and terminal state are unchanged");
    require(++requests[id].calls == 1, "read callback is delivered exactly once");
    checkBytes(id);
#if DVD_HEAP_FIXTURE
    require(port_is_game_thread() && port_use_game_heap(), "callback remains on the guest allocation context");
    auto* p = new unsigned char[64];
    require(port_is_game_heap_ptr(p), "guest callback still allocates from the original game heap");
    delete[] p;
#endif
    delivered[deliveredCount++] = id;
    if (enqueueInCallback && id == enqueueFrom) {
        enqueueInCallback = false;
        submitRequest(enqueueId, 0);
    }
}
void prepare(unsigned id) {
    Request& r = requests[id];
    r = Request{};
    r.bytes.fill(0xcc);
    r.file.startAddr = 0;
    r.file.length = 256;
    r.file.cb.userData = reinterpret_cast<void*>(static_cast<uintptr_t>(id + 1));
}
void submitRequest(unsigned id, int priority) {
    prepare(id);
    Request& r = requests[id];
    require(DVDReadAsyncPrio(&r.file, r.bytes.data(), 32, (id % 8) * 32, completed, priority), "actual asynchronous API accepts request");
    require(r.file.cb.state == kStateBusy && r.file.cb.transferredSize == 0, "submission retains original busy state");
#if DVD_HEAP_FIXTURE
    require(port_use_game_heap(), "submit restores guest allocation scope");
    for (const ReadRequest& pending : sPending)
        require(!port_is_game_heap_ptr(&pending), "pending queue nodes belong to host heap");
#endif
}
std::thread startWorker(unsigned count) {
    irqCount = 0;
    irqGoal = count;
    PortHostAllocScope scope;
    return std::thread([] {
#if DVD_HEAP_FIXTURE
        test_set_game_thread(false);
#endif
        try { workerMain(); } catch (const WorkerFinished&) {}
    });
}
void drain(unsigned count) {
    auto worker = startWorker(count);
    worker.join();
    dvdIrq();
    require(irqCount == count && !sActive && sDone.empty(), "actual worker and IRQ retire the exact requested count");
}
void expectOrder(std::initializer_list<unsigned> want) {
    require(retiredCount == want.size(), "expected completion count");
    unsigned i = 0, callbackIndex = 0;
    for (unsigned id : want) {
        if (retired[i] != id) {
            std::fprintf(stderr, "order[%u]: expected %u, actual %u\n", i, id, retired[i]);
            require(false, "actual worker obeys priority and FIFO within priority");
        }
        if (requests[id].file.cb.callback != nullptr)
            require(callbackIndex < deliveredCount && delivered[callbackIndex++] == id, "callback order matches actual prioritized completion order");
        ++i;
    }
    require(callbackIndex == deliveredCount, "there are no extra callbacks outside expected order");
}
void canceled(s32 result, DVDCommandBlock* block) {
    require(result == 0 && block->state == kStateCanceled, "queued async cancellation keeps original result and state");
    ++cancelCalls;
}
void reset() {
    require(sPending.empty() && sDone.empty() && !sActive, "no requests outlive their test case");
    retiredCount = deliveredCount = irqCount = cancelCalls = 0;
    syncPump = enqueueInCallback = false;
    gateAddress = nullptr;
    gateReached = gateReleased = false;
    gateHits = 0;
    pendingAddressCount = hostNodeFrees = 0;
}
void baseline() {
    reset();
    for (unsigned i = 0; i < 4; ++i) submitRequest(i, 2);
    drain(4);
    expectOrder({0, 1, 2, 3});
    require(deliveredCount == 4, "equal-priority callbacks exactly once");
}
void order() {
    reset();
    const int priorities[] = {3, 2, 1, 0, 2, 0, 3, 1};
    for (unsigned i = 0; i < 8; ++i) submitRequest(i, priorities[i]);
    drain(8);
    expectOrder({3, 5, 2, 7, 1, 4, 0, 6});
    require(deliveredCount == 8, "all priority levels complete exactly once");
}
void cancellation() {
    reset();
    submitRequest(0, 2); submitRequest(1, 1); submitRequest(2, 1); submitRequest(3, 1); submitRequest(4, 0);
    require(DVDCancelAsync(&requests[2].file.cb, canceled), "queued asynchronous cancellation accepts command");
    require(DVDCancel(&requests[4].file.cb) == 0, "queued synchronous cancellation accepts command");
    drain(3);
    expectOrder({1, 3, 0});
    require(cancelCalls == 1 && deliveredCount == 3 && requests[2].calls == 0 && requests[4].calls == 0,
            "canceled requests never read or deliver read callbacks");
    for (unsigned id : {2u, 4u})
        for (unsigned char byte : requests[id].bytes) require(byte == 0xcc, "canceled destination remains untouched");
}
void synchronous() {
    for (int priority : {0, 1, 2, 3, -1, 4, INT_MIN, INT_MAX}) {
        reset();
        submitRequest(0, 3); submitRequest(1, 0);
        prepare(2);
        syncPump = true;
        Request& r = requests[2];
        const s32 result = DVDReadPrio(&r.file, r.bytes.data(), 32, 64, priority);
        syncPump = false;
        require(result == 32 && r.file.cb.state == kStateEnd && r.file.cb.transferredSize == 32, "actual synchronous API returns correct completed bytes");
        checkBytes(2);
        require(r.calls == 0, "synchronous request has no asynchronous callback");
        if (!sPending.empty()) drain(static_cast<unsigned>(sPending.size()));
        if (priority == 3) expectOrder({1, 0, 2}); else expectOrder({1, 2, 0});
        require(deliveredCount == 2, "only asynchronous requests invoke callbacks");
    }
}
void active() {
    reset();
    submitRequest(0, 3);
    gateAddress = requests[0].bytes.data();
    auto worker = startWorker(3);
    {
        std::unique_lock<std::mutex> lock(gateMutex);
        require(gateCv.wait_for(lock, std::chrono::seconds(3), [] { return gateReached; }), "actual pread reaches deterministic active gate");
    }
    {
        std::lock_guard<std::mutex> lock(sQueueMutex);
        require(sActive == &requests[0].file.cb && sPending.empty(), "first request really left pending queue and owns active I/O");
    }
    submitRequest(1, 2); submitRequest(2, 0);
    {
        std::lock_guard<std::mutex> lock(gateMutex);
        gateReleased = true;
    }
    gateCv.notify_all();
    worker.join();
    dvdIrq();
    expectOrder({0, 2, 1});
    require(gateHits == 1, "interposer gated exactly the intended active pread");
    require(deliveredCount == 3, "active request is not duplicated or preempted");
}
void callbackEnqueue() {
    reset();
    submitRequest(0, 2); submitRequest(1, 2);
    enqueueInCallback = true; enqueueFrom = 0; enqueueId = 2;
    drain(1);
    require(sPending.size() == 2, "real callback queues new high-priority request beside waiting low-priority work");
    drain(2);
    expectOrder({0, 2, 1});
}
void invalid() {
    reset();
    const int priorities[] = {INT_MIN, 3, -1, 0, 4, 1, INT_MAX, 2};
    for (unsigned i = 0; i < 8; ++i) submitRequest(i, priorities[i]);
    drain(8);
    // Invalid priorities use ordinary priority 2; this is port policy, not an SDK guarantee.
    expectOrder({3, 5, 0, 2, 4, 6, 7, 1});
}
void pressure() {
    reset();
    for (unsigned i = 0; i < 1024; ++i) submitRequest(i, (3 - i % 4));
    for (const ReadRequest& r : sPending) pendingAddresses[pendingAddressCount++] = &r;
    drain(1024);
    for (unsigned i = 0; i < 1024; ++i) {
        const unsigned want = (3 - i / 256) + 4 * (i % 256);
        require(retired[i] == want, "priority and same-priority FIFO survive deque allocation pressure");
    }
    require(deliveredCount == 1024, "all pressure callbacks complete exactly once");
#if DVD_HEAP_FIXTURE
    require(hostNodeFrees > 0, "real host worker frees host-owned pending deque nodes");
#endif
}
void runCase(const char* name) {
    if (!std::strcmp(name, "baseline")) baseline();
    else if (!std::strcmp(name, "order")) order();
    else if (!std::strcmp(name, "cancel")) cancellation();
    else if (!std::strcmp(name, "sync")) synchronous();
    else if (!std::strcmp(name, "active")) active();
    else if (!std::strcmp(name, "callback")) callbackEnqueue();
    else if (!std::strcmp(name, "invalid")) invalid();
    else if (!std::strcmp(name, "pressure")) pressure();
    else require(false, "known test case");
    require(sPending.empty() && sDone.empty() && !sActive, "all test-owned command and destination lifetimes end after retirement");
    std::printf("PASS: %s; %u assertions; actual DVD APIs/worker/IRQ; host proof only\n", name, checks);
}
int run(const char* which) {
    char directory[] = "/tmp/galaxy-priority-XXXXXX";
    require(mkdtemp(directory), "private host fixture directory");
    char path[256];
    std::snprintf(path, sizeof(path), "%s/payload", directory);
    FILE* f = std::fopen(path, "wb");
    require(f != nullptr, "synthetic source opens");
    for (unsigned i = 0; i < 256; ++i) { const unsigned char byte = pattern(i); require(std::fwrite(&byte, 1, 1, f) == 1, "synthetic payload writes"); }
    require(std::fclose(f) == 0, "synthetic source closes before worker starts");
    {
        PortHostAllocScope scope;
        sFst.resize(1);
        sFst[0].hostPath = path;
        sFst[0].size = 256;
    }
    setenv("PETARI_READAHEAD", "0", 1);
    if (std::strcmp(which, "all")) runCase(which);
    else for (const char* name : {"baseline", "order", "cancel", "sync", "active", "callback", "invalid", "pressure"}) runCase(name);
    for (auto& slot : sOpenFiles) if (slot.fd >= 0) { require(close(slot.fd) == 0, "worker descriptor closes after all completions"); slot.fd = -1; }
    require(unlink(path) == 0 && rmdir(directory) == 0, "host fixture removed");
    return 0;
}
}

static ssize_t proof_pread(int fd, void* dst, size_t length, off_t offset) {
    if (dst == gateAddress) {
        std::unique_lock<std::mutex> lock(gateMutex);
        gateReached = true;
        ++gateHits;
        gateCv.notify_all();
        require(gateCv.wait_for(lock, std::chrono::seconds(3), [] { return gateReleased; }), "active I/O gate releases without holding DVD queue mutex");
    }
    return ::pread(fd, dst, length, offset);
}
extern "C" void OSWakeupThread(OSThreadQueue*) {}
#if !DVD_HEAP_FIXTURE
extern "C" void port_gpu_memory_written(void*, uint32_t) {}
#endif
extern "C" int64_t port_gap_age_ns(void) { return 0; }
extern "C" int64_t port_gap_threshold_ns(void) { return 0; }
extern "C" void port_irq_poll(void) {}
extern "C" void port_irq_raise(int irq) {
    require(irq == PORT_IRQ_DVD, "worker raises only DVD completion IRQ");
    {
        std::lock_guard<std::mutex> lock(sQueueMutex);
        require(!sDone.empty() && sActive == nullptr, "completion publishes before interrupt");
        retired[retiredCount++] = idOf(sDone.back());
#if DVD_HEAP_FIXTURE
        for (auto* const& block : sDone) require(!port_is_game_heap_ptr(&block), "completion queue remains host allocated");
#endif
    }
    if (++irqCount == irqGoal) throw WorkerFinished{};
}
extern "C" void OSSleepThread(OSThreadQueue* queue) {
    require(syncPump && queue == &sSyncQueue, "synchronous wait uses actual DVD completion queue");
    drain(1);
}
#if DVD_HEAP_FIXTURE
extern "C" void port_vgx_notify_write(const void*, size_t) {}
extern "C" void port_irq_poll_at(void*) {}
void test_note_host_free(const void* pointer) {
    if (!port_is_game_thread())
        for (unsigned i = 0; i < pendingAddressCount; ++i) if (pointer == pendingAddresses[i]) { ++hostNodeFrees; break; }
}
extern "C" int64_t port_host_time_ns(void) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
int test_dvd_heap_routing() { return run("all"); }
#else
extern "C" void port_host_alloc_scope_enter(void) {}
extern "C" void port_host_alloc_scope_leave(void) {}
extern "C" BOOL OSDisableInterrupts(void) { return TRUE; }
extern "C" BOOL OSRestoreInterrupts(BOOL value) { return value; }
int main(int argc, char** argv) { require(argc == 2, "one test case argument required"); return run(argv[1]); }
#endif
