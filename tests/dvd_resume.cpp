// The disc reader after the system took its files away (console sleep, Home menu): real worker, synthetic files, no device.
// A stream (JASAramStream) reads its file block by block; one failed read silences every stream until the game restarts, so the
// worker must reopen a file whose descriptor stopped working and repeat the read.  The "descriptor taken away" is made real:
// the descriptor the worker cached is closed behind its back, so the next pread answers EBADF.
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <string>
#include <sys/resource.h>

#include "../platform/src/dvd/dvd.cpp"

namespace {
struct WorkerFinished {};
unsigned callbacks;
s32 lastResult;
std::atomic<unsigned> resumeGeneration{0};

void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void completed(s32 result, DVDFileInfo*) { lastResult = result; ++callbacks; }

// One read through the real worker thread; returns the callback's result.
s32 readBlock(unsigned entry, unsigned char* out, unsigned length, unsigned offset) {
    DVDFileInfo file{};
    require(DVDFastOpen(entry, &file), "entry opens");
    require(DVDReadAsyncPrio(&file, out, (s32)length, (s32)offset, completed, 2), "request submits");
    const unsigned before = callbacks;
    std::thread worker([] {
        try { workerMain(); } catch (const WorkerFinished&) {}
    });
    worker.join();
    dvdIrq();
    require(callbacks == before + 1, "exactly one callback");
    return lastResult;
}
unsigned char patternAt(unsigned offset) { return (unsigned char)((offset * 31u + 7u) & 0xFF); }
void writeStream(const std::string& path, unsigned size) {
    FILE* f = fopen(path.c_str(), "wb");
    require(f != nullptr, "stream file created");
    for (unsigned i = 0; i < size; ++i) { const unsigned char b = patternAt(i); fwrite(&b, 1, 1, f); }
    require(fclose(f) == 0, "stream file written");
}
bool blockMatches(const unsigned char* data, unsigned length, unsigned offset) {
    for (unsigned i = 0; i < length; ++i) if (data[i] != patternAt(offset + i)) return false;
    return true;
}
// What the system does to the files while the console sleeps: the descriptor numbers stay in the cache but no longer work.
void takeDescriptorsAway() {
    for (auto& slot : sOpenFiles) if (slot.fd >= 0) close(slot.fd);  // (slot.fd stays: the cache does not know)
}
}

extern "C" void port_host_alloc_scope_enter(void) {}
extern "C" void port_host_alloc_scope_leave(void) {}
extern "C" void port_gpu_memory_written(void*, uint32_t) {}
extern "C" void OSWakeupThread(OSThreadQueue*) {}
extern "C" void port_irq_raise(int irq) {
    require(irq == PORT_IRQ_DVD, "worker raises DVD completion");
    throw WorkerFinished{};
}
extern "C" unsigned port_resume_generation(void) { return resumeGeneration.load(); }

int main(int argc, char** argv) {
    require(argc == 2, "expected on or off");
    const bool on = std::string(argv[1]) == "on";
    setenv("GXV_DVD_REOPEN", on ? "1" : "0", 1);
    setenv("GXV_RESUME_RESET", "1", 1);
    char root[] = "/tmp/galaxy-dvd-resume-XXXXXX";
    require(mkdtemp(root), "private fixture directory");
    const std::string directory(root);
    sFst.resize(6);
    for (unsigned i = 0; i < sFst.size(); ++i) {
        sFst[i].hostPath = directory + "/file-" + std::to_string(i);
        sFst[i].isDir = false;
        sFst[i].size = -1;
    }
    constexpr unsigned kBlock = 4096, kBlocks = 12;
    writeStream(sFst[1].hostPath, kBlock * kBlocks);
    writeStream(sFst[2].hostPath, kBlock * kBlocks);
    for (auto& d : sReopenDelayMs) d = 0;

    // The stream: block after block, the descriptors taken away after the fourth.
    unsigned char block[kBlock];
    unsigned failures = 0, good = 0;
    for (unsigned i = 0; i < kBlocks; ++i) {
        if (i == 4) takeDescriptorsAway();
        memset(block, 0xCC, sizeof block);
        const s32 r = readBlock(1, block, kBlock, i * kBlock);
        if (r == (s32)kBlock && blockMatches(block, kBlock, i * kBlock)) ++good; else ++failures;
    }
    if (on) {
        require(failures == 0 && good == kBlocks, "the stream continues through a taken-away descriptor");
        require(sReopenRecovered == 1, "exactly one recovery");
    } else {
        require(failures >= 1, "without the retry the read after the sleep fails (what silenced the music)");
        return std::printf("PASS: GXV_DVD_REOPEN=0 keeps the old behaviour (%u of %u blocks failed after the descriptors went)\n", failures, kBlocks), 0;
    }

    // A file that is really gone (not a resume problem) still fails at once, with no waiting: ENOENT is not retried.
    for (auto& d : sReopenDelayMs) d = 1000;
    require(unlink(sFst[2].hostPath.c_str()) == 0, "file removed");
    const auto t0 = std::chrono::steady_clock::now();
    require(readBlock(2, block, kBlock, 0) == -1, "a vanished file still fails");
    require(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500), "a vanished file is not retried");
    for (auto& d : sReopenDelayMs) d = 0;

    // The card is not ready for a moment: the open is refused (descriptor table full here) and goes through on a later try.
    writeStream(sFst[3].hostPath, kBlock);
    for (auto& slot : sOpenFiles) if (slot.fd >= 0) { close(slot.fd); slot.fd = -1; }
    struct rlimit old{};
    require(getrlimit(RLIMIT_NOFILE, &old) == 0, "rlimit read");
    std::vector<int> hog;
    for (;;) { int fd = open("/dev/null", O_RDONLY); if (fd < 0) break; hog.push_back(fd); if (hog.size() > 70000) break; }
    require(errno == EMFILE, "descriptor table is full");
    sReopenDelayMs[0] = 0; sReopenDelayMs[1] = 40; sReopenDelayMs[2] = 40; sReopenDelayMs[3] = 400;
    std::thread card([&hog] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); for (int fd : hog) close(fd); hog.clear(); });
    const unsigned recoveredBefore = sReopenRecovered;
    const s32 late = readBlock(3, block, kBlock, 0);
    card.join();
    require(late == (s32)kBlock && blockMatches(block, kBlock, 0), "an open refused for a moment is retried and works");
    require(sReopenRecovered == recoveredBefore + 1, "the late open counts as a recovery");
    for (auto& d : sReopenDelayMs) d = 0;

    // The resume report (GXV_RESUME_RESET=1): cached files and windows are dropped before the next request.
    require(readBlock(1, block, kBlock, 0) == (s32)kBlock, "read before the resume");
    bool cached = false;
    for (auto& slot : sOpenFiles) cached |= slot.fd >= 0;
    require(cached, "a descriptor is cached");
    sReadAhead.entry = 1;
    resumeGeneration = 1;
    noteResume();
    cached = false;
    for (auto& slot : sOpenFiles) cached |= slot.fd >= 0;
    require(!cached && sReadAhead.entry == ~0u && sMovieWin.entry == ~0u, "a resume drops descriptors and windows");
    noteResume();
    require(readBlock(1, block, kBlock, 8 * kBlock) == (s32)kBlock && blockMatches(block, kBlock, 8 * kBlock), "reads work after the resume");
    require(sReopenRecovered == recoveredBefore + 1, "the dropped files were opened afresh, not 'recovered'");

    for (auto& slot : sOpenFiles) if (slot.fd >= 0) { close(slot.fd); slot.fd = -1; }
    for (unsigned i = 1; i <= 3; ++i) unlink(sFst[i].hostPath.c_str());
    rmdir(root);
    std::printf("PASS: stream continues after taken-away descriptors; late open retried; vanished file not retried; resume drops caches (%u recoveries)\n", sReopenRecovered);
}
