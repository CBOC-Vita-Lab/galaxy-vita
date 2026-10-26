#include "JSystem/JAudio2/JAIStream.hpp"
#include "JSystem/JAudio2/JAIStreamDataMgr.hpp"
#include "JSystem/JAudio2/JAIStreamMgr.hpp"
#include "JSystem/JAudio2/JAISoundHandles.hpp"
#include "JSystem/JAudio2/JASDriverIF.hpp"
#include "JSystem/JAudio2/JASChannel.hpp"
#include "JSystem/JKernel/JKRAram.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

extern bool UNK_BOOL_A;
extern bool UNK_BOOL_B;

namespace {
unsigned checks, reads, errors, reports, copies, registrations;
bool rejectOpen;
std::string streamPath;
struct Task { JASThreadCallback callback; std::vector<unsigned char> data; void* pointer; };
std::deque<Task> tasks;
void require(bool ok, const char* why) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void drain() {
    unsigned count = 0;
    while (!tasks.empty()) {
        require(++count < 32, "synthetic preparation task graph remains bounded");
        Task task = std::move(tasks.front());
        tasks.pop_front();
        task.callback(task.data.empty() ? task.pointer : task.data.data());
    }
}
struct Aram final : JAIStreamAramMgr {
    void* newStreamAram(u32* size) override {
        *size = 6 * JASAramStream::sBlockSize;
        return reinterpret_cast<void*>(0x10000);
    }
    bool deleteStreamAram(u32) override { return true; }
};
void writeSyntheticStream(const std::string& path) {
    constexpr unsigned block = 0x2000;
    JASAramStream::Header header{};
    header.tag = 0x5354524d;
    header.format = 1;
    header.channels = 2;
    header.loop = 1;
    header._10 = 32000;
    header.loop_end = 16384;
    header.block_size = block;
    header._28 = 127;
    std::vector<unsigned char> bytes(sizeof(header) + 4 * (sizeof(JASAramStream::BlockHeader) + block * 2));
    std::memcpy(bytes.data(), &header, sizeof(header));
    for (unsigned i = 0; i < 4; ++i) {
        JASAramStream::BlockHeader b{};
        b._4 = block;
        std::memcpy(bytes.data() + sizeof(header) + i * (sizeof(b) + block * 2), &b, sizeof(b));
    }
    FILE* file = std::fopen(path.c_str(), "wb");
    require(file != nullptr, "create synthetic stream only in fixture temporary directory");
    require(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(), "write synthetic stream");
    require(std::fclose(file) == 0, "close synthetic stream");
}
void reset(bool retainError) {
    require(tasks.empty(), "previous scenario left no asynchronous work");
    reads = errors = reports = copies = registrations = 0;
    if (!retainError) UNK_BOOL_A = UNK_BOOL_B = false;
    rejectOpen = false;
}
void scenario(const char* name, bool missing, bool invalidEntry, bool retainError = false) {
    reset(retainError);
    rejectOpen = invalidEntry;
    Aram aram;
    JAIStreamMgr manager(false);
    manager.setStreamAramMgr(&aram);
    JAIStream stream(&manager, nullptr);
    JAISoundHandle handle;
    stream.JAIStreamMgr_startID_(JAISoundID(0x02000001), 7, nullptr, nullptr, 0);
    stream.attachHandle(&handle);
    stream.lockWhenPrepared();
    require(!handle->isPrepared(), "title sound starts unprepared");
    stream.prepare_();
    require(stream.mPrepareState == 1, "actual JAIStream allocated ARAM and installed callback");
    stream.prepare_();
    if (invalidEntry) {
        require(stream.mPrepareState == 1 && tasks.empty() && registrations == 0,
                "invalid entry is a distinct state-1 retry, before DVD tasks");
    } else {
        require(stream.mPrepareState == 2 && tasks.size() == 1 && registrations == 1,
                "actual JAS prepare accepted FST entry and queued header read");
        drain();
    }
    for (unsigned i = 0; i < 4096; ++i) stream.prepare_();
    if (missing) {
        if (retainError) {
            require(reads == 0 && errors == 0 && UNK_BOOL_B,
                    "new sound with restored file is blocked by previous global error before another DVD read");
        } else {
            require(reads == 1 && errors == 1 && UNK_BOOL_B, "missing host file reaches actual header-read error latch once");
        }
        require(stream.mPrepareState == 2 && stream.mPrepareCount == 4096 && reports == 16,
                "actual JAIStream remains pending after 4096 updates, with periodic prepare reports");
        require(handle.isSoundAttached() && !handle->isPrepared() && !stream.mIsStreamStopped,
                "attached title sound never receives prepared callback; the BGM predicate remains false");
        require(copies == 0 && !stream.inner_.aramStream._0AC, "failure never transfers audio or prepares channels");
    } else if (invalidEntry) {
        require(stream.mPrepareState == 1 && !handle->isPrepared() && reads == 0 && !UNK_BOOL_B,
                "invalid entry cannot be misreported as a missing host-file read");
    } else {
        require(reads == 3 && errors == 0 && !UNK_BOOL_B && copies == 4,
                "valid synthetic header and two blocks complete actual preparation work");
        require(stream.mPrepareState == 3 && handle->isPrepared() &&
                    stream.mStatus.getState() == JAISoundStatus_::State_LOCK_READY,
                "actual prepare-finish callback advances locked sound to ready without starting audio");
    }
    std::printf("%s: state=%d attached=%d prepared=%d header_error=%d reads=%u read_errors=%u prepare_steps=%d reports=%u\n",
                name, stream.mPrepareState, handle.isSoundAttached(), handle->isPrepared(), UNK_BOOL_B,
                reads, errors, stream.mPrepareCount, reports);
    handle.releaseSound();
}
}

// Host seams replace scheduling, disc access and hardware only. The three
// sound classes, preparation state machine, callbacks and status flags are real.
extern "C" BOOL OSDisableInterrupts() { return TRUE; }
extern "C" BOOL OSRestoreInterrupts(BOOL enabled) { return enabled; }
extern "C" void DCInvalidateRange(void*, u32) {}
extern "C" void OSInitMessageQueue(OSMessageQueue* q, OSMessage* messages, s32 capacity) {
    std::memset(q, 0, sizeof(*q)); q->msgArray = messages; q->msgCount = capacity;
}
extern "C" BOOL OSSendMessage(OSMessageQueue* q, OSMessage message, s32) {
    require(q->usedCount < q->msgCount, "fixture message queue has room");
    q->msgArray[(q->firstIndex + q->usedCount++) % q->msgCount] = message;
    return TRUE;
}
extern "C" BOOL OSReceiveMessage(OSMessageQueue* q, OSMessage* message, s32) {
    if (!q->usedCount) return FALSE;
    *message = q->msgArray[q->firstIndex];
    q->firstIndex = (q->firstIndex + 1) % q->msgCount; --q->usedCount;
    return TRUE;
}
extern "C" BOOL DVDFastOpen(s32 entry, DVDFileInfo* info) {
    require(entry == 7, "synthetic FST entry is stable even when host file is missing");
    if (rejectOpen) return FALSE;
    std::memset(info, 0, sizeof(*info));
    struct stat st;
    info->length = stat(streamPath.c_str(), &st) == 0 ? static_cast<u32>(st.st_size) : 0;
    return TRUE;
}
extern "C" s32 DVDReadPrio(DVDFileInfo*, void* destination, s32 size, s32 offset, s32) {
    ++reads;
    int fd = open(streamPath.c_str(), O_RDONLY);
    if (fd < 0) { require(errno == ENOENT, "controlled DVD failure is ENOENT"); ++errors; return -1; }
    ssize_t got = pread(fd, destination, size, offset);
    require(close(fd) == 0, "close synthetic stream read");
    require(got == size, "valid fixture never relies on short-read padding");
    return static_cast<s32>(got);
}
void JASReport(const char*, ...) { ++reports; }
namespace JASDriver {
bool registerSubFrameCallback(DriverCallback, void*) { ++registrations; return true; }
int rejectCallback(DriverCallback, void*) { return 1; }
}
JKRDisposer::JKRDisposer() : mHeap(nullptr), mLink(this) {}
JKRDisposer::~JKRDisposer() = default;
JKRThread::JKRThread(u32, int, int) : mThreadListLink(this) {}
JKRThread::~JKRThread() = default;
JASTaskThread::JASTaskThread(int priority, int count, u32 stack) : JKRThread(stack, count, priority) {}
JASTaskThread::~JASTaskThread() = default;
void* JASTaskThread::run() { std::abort(); }
int JASTaskThread::sendCmdMsg(JASThreadCallback callback, const void* data, u32 size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    tasks.push_back({callback, {bytes, bytes + size}, nullptr}); return 1;
}
int JASTaskThread::sendCmdMsg(JASThreadCallback callback, void* pointer) {
    tasks.push_back({callback, {}, pointer}); return 1;
}
JKRAramBlock* JKRAram::mainRamToAram(u8*, u32, u32, JKRExpandSwitch, u32, JKRHeap*, int, u32*) {
    ++copies; return reinterpret_cast<JKRAramBlock*>(0x10000);
}
JASGenericMemPool::JASGenericMemPool() : _0(nullptr), mFreeMemCount(0), mTotalMemCount(0), mUsedMemCount(0) {}
JASGenericMemPool::~JASGenericMemPool() = default;
void* JASGenericMemPool::alloc(u32) { std::abort(); }
void JASGenericMemPool::free(void*, u32) { std::abort(); }
void JASChannel::release(u16) { std::abort(); }
extern "C" void __wrap__ZN13JASAramStream12channelStartEv(JASAramStream*) { std::abort(); }
extern "C" void __wrap__ZN13JASAramStream11channelStopEt(JASAramStream*, u16) { std::abort(); }

int main() {
    char directory[] = "/tmp/galaxy-stream-prepare-XXXXXX";
    require(mkdtemp(directory) != nullptr, "create isolated fixture directory");
    const std::string valid = std::string(directory) + "/synthetic.ast";
    writeSyntheticStream(valid);
    JASTaskThread worker(0, 8, 0);
    std::vector<u8> readBuffer(0x5000);
    JASAramStream::sLoadThread = &worker;
    JASAramStream::sReadBuffer = readBuffer.data();
    JASAramStream::sBlockSize = 0x2000;
    JASAramStream::sChannelMax = 2;
    streamPath = valid;
    scenario("baseline", false, false);
    streamPath = std::string(directory) + "/missing-title.ast";
    scenario("missing-host-file", true, false);
    streamPath = valid;
    scenario("new-sound-after-file-restored", true, false, true);
    scenario("invalid-entry-control", false, true);
    streamPath = valid;
    scenario("fresh-boot-recovery", false, false);
    require(unlink(valid.c_str()) == 0 && rmdir(directory) == 0, "remove only owned synthetic files");
    std::printf("PASS: %u checks, actual stream preparation; no game assets, game run or Vita evidence\n", checks);
}
