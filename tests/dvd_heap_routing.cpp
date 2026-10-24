#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>

#include "../platform/src/dvd/dvd.cpp"

namespace {
constexpr unsigned requestCount = 1024;
constexpr unsigned payloadBytes = 32;
constexpr unsigned fixtureFiles = 128;
unsigned checks;
unsigned callbacks;
unsigned failures;
u32 expectedFailure = UINT32_MAX;
unsigned expectedInterrupts = requestCount;
std::atomic<unsigned> interrupts;
std::array<const void*, requestCount> pendingAddresses{};
unsigned workerNodeFrees;
struct WorkerFinished {};
void require(bool value, const char* message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "DVD heap routing FAILED: %s\n", message); std::exit(1); }
}
void completed(s32 result, DVDFileInfo* file) {
    require(port_is_game_thread() && port_use_game_heap(), "completion callback retains the guest allocation context");
    auto* bytes = static_cast<unsigned char*>(file->cb.addr);
    if (file->startAddr == expectedFailure) {
        require(result == -1 && file->cb.state == kStateFatal && !file->cb.transferredSize,
                "real file failure delivers one fatal guest callback without a false transfer");
        for (unsigned i = 0; i < payloadBytes; ++i) require(bytes[i] == 0, "failed read leaves its destination untouched");
        ++failures;
    } else {
        require(result == payloadBytes && file->cb.state == kStateEnd, "real worker read completes through the DVD IRQ");
        for (unsigned i = 0; i < payloadBytes; ++i)
            require(bytes[i] == static_cast<unsigned char>(i ^ 0xA5 ^ file->startAddr), "read payload is unchanged across file eviction and revisits");
    }
    auto* guest = new unsigned char[64];
    require(port_is_game_heap_ptr(guest), "game callback allocations still use the original heap");
    delete[] guest;
    ++callbacks;
}
}

void test_set_game_thread(bool);
void test_note_host_free(const void* address) {
    if (!port_is_game_thread()) {
        for (const void* pending : pendingAddresses) {
            if (address == pending) { ++workerNodeFrees; break; }
        }
    }
}
extern "C" void OSInitThreadQueue(OSThreadQueue* queue) { std::memset(queue, 0, sizeof(*queue)); }
extern "C" void OSWakeupThread(OSThreadQueue*) {}
extern "C" void OSSleepThread(OSThreadQueue*) { port_fatal("unexpected synchronous DVD wait in asynchronous test"); }
extern "C" void port_irq_set_handler(int, void (*)(void)) {}
extern "C" void port_irq_raise(int irq) {
    require(irq == PORT_IRQ_DVD && !port_is_game_thread(), "host DVD worker raises only the DVD interrupt");
    // End the real infinite worker at a completed IRQ boundary, only in this fixture.
    if (interrupts.fetch_add(1) + 1 == expectedInterrupts) throw WorkerFinished{};
}

enum class Batch { Heap, Pressure, OpenFailure, OpenRetry, Lru, CloseFailure, CloseRetry };
static void readBatch(Batch batch) {
    const unsigned count = batch == Batch::Heap || batch == Batch::Pressure ? requestCount :
        batch == Batch::Lru ? 34 : batch == Batch::OpenFailure ? 2 : 1;
    expectedInterrupts = count;
    interrupts.store(0);
    callbacks = 0;
    failures = 0;
    pendingAddresses.fill(nullptr);
    workerNodeFrees = 0;
    std::array<DVDFileInfo, requestCount> files{};
    std::array<std::array<unsigned char, payloadBytes>, requestCount> buffers{};
    for (unsigned i = 0; i < count; ++i) {
        const unsigned entry = batch == Batch::Pressure && (i & 1) ? (i / 2) % fixtureFiles :
            (batch == Batch::OpenFailure && i == 0) || batch == Batch::OpenRetry ? fixtureFiles :
            batch == Batch::Lru ? (i < 32 ? i : i == 32 ? 0 : 32) :
            batch == Batch::CloseFailure || batch == Batch::CloseRetry ? 33 : 0;
        require(DVDFastOpen(entry, &files[i]), "original file API opens the synthetic entry");
        require(DVDReadAsyncPrio(&files[i], buffers[i].data(), payloadBytes, 0, completed, 2), "real asynchronous API submits a read");
    }
    require(sPending.size() == count, "pending queue contains the complete actual read batch");
    unsigned index = 0;
    for (const ReadRequest& request : sPending) {
        require(!port_is_game_heap_ptr(&request), "DVD pending queue storage must belong to the host, not the submitting game thread");
        pendingAddresses[index++] = &request;
    }
    std::thread worker;
    {
        PortHostAllocScope scope;
        worker = std::thread([] {
            test_set_game_thread(false);
            try { workerMain(); }
            catch (const WorkerFinished&) {}
        });
    }
    worker.join();
    if (count == requestCount)
        require(workerNodeFrees > 0, "host worker actually frees pending deque nodes, not merely their elements");
    require(interrupts.load() == count && sPending.empty() && sDone.size() == count,
            "real host worker retires all pending nodes without touching a game mutex");
    for (auto* const& block : sDone) {
        (void)block;
        require(!port_is_game_heap_ptr(&block), "DVD completion queue storage stays host owned");
    }
    dvdIrq();
    require(callbacks == count && sDone.empty(), "all original callbacks are delivered by the guest IRQ path");
    require(failures == unsigned(expectedFailure != UINT32_MAX), "only the requested file failure reaches a fatal callback");
    require(DVDGetDriveStatus() == 0 && port_use_game_heap(), "drive is idle and the guest allocation context is restored");
    std::fprintf(stderr, "DVD batch %u: %u actual worker reads and guest callbacks, %u failures, %u worker-side node frees\n",
                 unsigned(batch), count, failures, workerNodeFrees);
}

static unsigned closeReadDescriptors() {
    unsigned retained = 0;
    for (auto& slot : sOpenFiles) {
        if (slot.fd < 0) continue;
        ++retained;
        require(close(slot.fd) == 0, "production read descriptor closes after the fixture");
        slot.fd = -1;
    }
    return retained;
}

int test_dvd_heap_routing() {
    char directory[] = "/tmp/galaxy-dvd-XXXXXX";
    require(mkdtemp(directory) != nullptr, "private synthetic fixture directory opens");
    {
        PortHostAllocScope scope;
        sFst.resize(fixtureFiles + 1);
        sFst[fixtureFiles].hostPath = std::string(directory) + "/missing-file";
        sFst[fixtureFiles].isDir = false;
        sFst[fixtureFiles].size = payloadBytes;
        for (unsigned entry = 0; entry < fixtureFiles; ++entry) {
            char path[256];
            std::snprintf(path, sizeof(path), "%s/file-%u", directory, entry);
            sFst[entry].hostPath = path;
            sFst[entry].isDir = false;
            FILE* source = fopen(path, "wb");
            require(source != nullptr, "synthetic named file opens");
            unsigned char payload[payloadBytes];
            for (unsigned i = 0; i < payloadBytes; ++i) payload[i] = static_cast<unsigned char>(i ^ 0xA5 ^ entry);
            require(fwrite(payload, 1, sizeof(payload), source) == sizeof(payload), "synthetic bytes reach the file");
            require(fclose(source) == 0, "fixture writer closes before read pressure");
        }
    }
    readBatch(Batch::Heap);
    struct rlimit saved{};
    require(getrlimit(RLIMIT_NOFILE, &saved) == 0, "descriptor limit is readable");
    struct rlimit pressure = saved;
    pressure.rlim_cur = 64;
    require(setrlimit(RLIMIT_NOFILE, &pressure) == 0, "actual kernel descriptor limit is reduced to 64");
    readBatch(Batch::Pressure);
    expectedFailure = fixtureFiles;
    readBatch(Batch::OpenFailure);
    FILE* repaired = fopen(sFst[fixtureFiles].hostPath.c_str(), "wb");
    require(repaired != nullptr, "previously missing file becomes available without cache reinitialization");
    unsigned char payload[payloadBytes];
    for (unsigned i = 0; i < payloadBytes; ++i) payload[i] = static_cast<unsigned char>(i ^ 0xA5 ^ fixtureFiles);
    require(fwrite(payload, 1, sizeof(payload), repaired) == sizeof(payload) && fclose(repaired) == 0,
            "retry fixture owns a complete closed source");
    expectedFailure = UINT32_MAX;
    readBatch(Batch::OpenRetry);
    require(closeReadDescriptors() == 32, "descriptor cache stays bounded after reading all 128 files and recovering");
    readBatch(Batch::Lru);
    auto cached = [](u32 entry) {
        return std::any_of(sOpenFiles.begin(), sOpenFiles.end(), [entry](const auto& slot) { return slot.fd >= 0 && slot.entry == entry; });
    };
    require(cached(0) && !cached(1) && cached(32), "cache hit promotes entry zero and evicts the true least-recently-used entry one");
    for (const auto& slot : sOpenFiles) {
        if (slot.fd >= 0 && slot.entry == 2) require(close(slot.fd) == 0, "kernel fault fixture invalidates only the next LRU victim");
    }
    expectedFailure = 33;
    readBatch(Batch::CloseFailure);
    expectedFailure = UINT32_MAX;
    readBatch(Batch::CloseRetry);
    require(setrlimit(RLIMIT_NOFILE, &saved) == 0, "original descriptor limit is restored");
    require(closeReadDescriptors() == 32, "close failure and retry preserve bounded descriptor ownership");
    for (auto& entry : sFst) {
        require(unlink(entry.hostPath.c_str()) == 0, "synthetic file is removed");
    }
    require(rmdir(directory) == 0, "synthetic fixture directory is removed");
    std::fprintf(stderr, "DVD heap routing and descriptor pressure: %u checks passed, %u actual worker reads and guest callbacks; host fixture, not device proof\n",
                 checks, requestCount * 2 + 39);
    return 0;
}
