// disc26: the drive worker's play-time policies, with the real worker thread and synthetic files:
//   1. a long read is done in pieces and a priority-1 request that arrives between two pieces is served first (and both are right);
//   2. two files read in sequence by turns keep one read-ahead window each (both second pieces come from memory);
//   3. the learned prefetch cache: a listed file is read while the card is idle, a later read of it is answered from memory,
//      a file over the cap is passed over, the list file selects by stage key;
//   4. the "io:" census: counters by class in a play window, the line in the log.
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <vector>

#include "../platform/src/dvd/dvd.cpp"

namespace {
std::mutex gMu;
std::condition_variable gCv;
unsigned gRaised;
std::vector<DVDFileInfo*> gOrder;
void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void completed(s32, DVDFileInfo* fi) { gOrder.push_back(fi); }
unsigned char patternAt(unsigned file, unsigned offset) { return (unsigned char)((offset * 31u + file * 17u + 7u) & 0xFF); }
void writeFile(const std::string& path, unsigned file, unsigned size) {
    FILE* f = fopen(path.c_str(), "wb");
    require(f != nullptr, "file created");
    std::vector<unsigned char> d(size);
    for (unsigned i = 0; i < size; ++i) d[i] = patternAt(file, i);
    require(fwrite(d.data(), 1, size, f) == size, "file written");
    fclose(f);
}
bool matches(const unsigned char* p, unsigned file, unsigned offset, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (p[i] != patternAt(file, offset + i)) return false;
    return true;
}
// Wait until `n` completions have been raised in all, then run the callbacks the way the interrupt does.
void waitRaised(unsigned n) {
    std::unique_lock<std::mutex> lk(gMu);
    require(gCv.wait_for(lk, std::chrono::seconds(10), [&] { return gRaised >= n; }), "the worker completed the requests in time");
    lk.unlock();
    dvdIrq();
}
void waitUntil(const std::function<bool()>& f, const char* why) {
    for (int i = 0; i < 600 && !f(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(f(), why);
}
// One request through the real worker; returns once its callback ran.
void readOne(DVDFileInfo& fi, unsigned entry, void* dst, unsigned n, unsigned offset, int prio) {
    require(DVDFastOpen(entry, &fi), "entry opens");
    const size_t before = gOrder.size();
    unsigned target;
    { std::lock_guard<std::mutex> lk(gMu); target = gRaised + 1; }
    require(DVDReadAsyncPrio(&fi, dst, (s32)n, (s32)offset, completed, prio), "request submits");
    waitRaised(target);
    require(gOrder.size() == before + 1, "exactly one callback");
}
DVDFileInfo gUrgentFi;
unsigned char gUrgentBuf[4096];
bool gHookFired;
void pieceHook(size_t) {
    if (gHookFired) return;
    gHookFired = true;
    require(DVDFastOpen(1, &gUrgentFi), "urgent entry opens");
    DVDReadAsyncPrio(&gUrgentFi, gUrgentBuf, 4096, 8192, completed, 1);  // a music-stream read arrives while the archive is half read
}
}  // namespace

extern "C" void port_host_alloc_scope_enter(void) {}
extern "C" void port_host_alloc_scope_leave(void) {}
extern "C" void port_gpu_memory_written(void*, uint32_t) {}
extern "C" void OSWakeupThread(OSThreadQueue*) {}
extern "C" void port_irq_raise(int) {
    std::lock_guard<std::mutex> lk(gMu);
    ++gRaised;
    gCv.notify_all();
}
extern "C" unsigned port_resume_generation(void) { return 0; }
extern "C" int port_handheld(void) { return 0; }
extern "C" int64_t port_gap_age_ns(void) { return 0; }
extern "C" int64_t port_gap_threshold_ns(void) { return 0; }
extern "C" void port_irq_poll(void) {}

int main() {
    char root[] = "/tmp/galaxy-dvd-io26-XXXXXX";
    require(mkdtemp(root), "private fixture directory");
    const std::string dir(root);
    setenv("GXV_IO_PREFETCH_MB", "1", 1);
    setenv("GXV_DVD_CHUNK_KIB", "64", 1);
    const std::string logPath = dir + "/log.txt", listPath = dir + "/list.txt";
    setenv("GXV_IO_PREFETCH_LIST", listPath.c_str(), 1);
    port_log_file(logPath.c_str());

    // a mini file table: the root and eight files in it
    const unsigned sizes[8] = {0, 4096 * 4, 1 << 20, 6 * 8192, 6 * 8192, 100 * 1024, 2 << 20, 64 * 1024};
    sFst.resize(8);
    sFst[0].isDir = true; sFst[0].parent = 0; sFst[0].next = 8;
    // entry 1: small stream-like file (the urgent read), 2: the 1 MiB "archive", 3/4: the sequence files, 5: prefetched, 6: over the cap, 7: a stream
    for (unsigned i = 1; i < 8; ++i) {
        sFst[i].isDir = false;
        sFst[i].name = "f" + std::to_string(i);
        sFst[i].hostPath = dir + "/" + sFst[i].name;
        sFst[i].size = -1;
        sFst[i].cls = PORT_DISC_OBJECT;
    }
    sFst[1].size = -1;
    writeFile(sFst[1].hostPath, 1, 64 * 1024);
    writeFile(sFst[2].hostPath, 2, sizes[2]);
    writeFile(sFst[3].hostPath, 3, sizes[3]);
    writeFile(sFst[4].hostPath, 4, sizes[4]);
    writeFile(sFst[5].hostPath, 5, sizes[5]);
    writeFile(sFst[6].hostPath, 6, sizes[6]);
    writeFile(sFst[7].hostPath, 7, sizes[7]);
    sFst[7].cls = PORT_DISC_AUDIO; sFst[7].sub = 1;
    sFst[3].cls = PORT_DISC_AUDIO; sFst[3].sub = 2;
    sFst[4].cls = PORT_DISC_STAGE;
    for (auto& d : sReopenDelayMs) d = 0;
    sFstReady.store(true);
    std::thread(workerMain).detach();

    // ---- 1. pieces and the urgent request between them
    {
        static unsigned char big[1 << 20];
        DVDFileInfo fi{};
        require(DVDFastOpen(2, &fi), "archive opens");
        sTestPieceHook = pieceHook;
        unsigned target;
        { std::lock_guard<std::mutex> lk(gMu); target = gRaised + 2; }
        const size_t before = gOrder.size();
        require(DVDReadAsyncPrio(&fi, big, (s32)sizeof big, 0, completed, 2), "archive read submits");
        waitRaised(target);
        sTestPieceHook = nullptr;
        require(gOrder.size() == before + 2, "both callbacks ran");
        require(gOrder[before] == &gUrgentFi && gOrder[before + 1] == &fi, "the priority-1 request completed first, the long read second");
        require(matches(big, 2, 0, sizeof big), "the long read, done in pieces around the urgent one, is byte for byte right");
        require(matches(gUrgentBuf, 1, 8192, 4096), "the urgent read is right");
        require(ioc::yields.load() == 1 && ioc::cuts.load() == 1, "one long read cut, one urgent request served between pieces");
        std::puts("PASS: a long read is cut and a priority-1 request goes first, data exact");
    }

    // ---- 2. two sequences by turns, one read-ahead window each
    {
        unsigned char a[8192], b[8192];
        DVDFileInfo fa{}, fb{};
        const uint64_t hits0 = loadsplit::g.readAheadHits.load();
        for (unsigned piece = 0; piece < 4; ++piece) {
            readOne(fa, 3, a, 8192, piece * 8192, 2);
            require(matches(a, 3, piece * 8192, 8192), "A piece right");
            readOne(fb, 4, b, 8192, piece * 8192, 2);
            require(matches(b, 4, piece * 8192, 8192), "B piece right");
        }
        const uint64_t hits = loadsplit::g.readAheadHits.load() - hits0;
        std::printf("read-ahead hits with two alternating sequences: %u of 6 possible\n", (unsigned)hits);
        require(hits >= 4, "alternating sequences are served from their windows (a single window would give none)");
        std::puts("PASS: one read-ahead window per file");
    }

    // ---- 3. the learned prefetch
    {
        FILE* l = fopen(listPath.c_str(), "w");
        fprintf(l, "0000abcd /f5\n0000abcd /f6\n11111111 /f7\n* /f1\n");
        fclose(l);
        port_io_play_begin("TestGalaxy", 0xabcd);  // opens the stage's play window and asks the worker for the list
        waitUntil([] { return ioc::pfFiles.load() >= 2; }, "the listed files were read ahead");
        waitUntil([] { return ioc::pfSkipped.load() >= 1; }, "the file over the cap was passed over");
        require(ioc::pfFiles.load() == 2, "f5 and the '*' line's f1 were read ahead; f7 (another stage) was not; f6 is over the 1 MiB cap");
        static unsigned char buf[100 * 1024];
        DVDFileInfo fi{};
        readOne(fi, 5, buf, sizeof buf, 0, 2);
        require(matches(buf, 5, 0, sizeof buf), "a prefetched file is answered right");
        readOne(fi, 5, buf, 5000, 7000, 2);
        require(matches(buf, 5, 7000, 5000), "...and a piece in the middle of it");
        require(ioc::pfHits.load() == 2, "both reads came from the prefetch cache");
        readOne(fi, 6, buf, 4096, 0, 2);
        require(matches(buf, 6, 0, 4096), "the file over the cap is read from the card as before");
        require(ioc::pfHits.load() == 2, "...not from the cache");
        std::puts("PASS: learned prefetch (list by stage key, cap, hits)");
    }

    // ---- 4. the census
    {
        unsigned char buf[16384];
        DVDFileInfo fi{};
        readOne(fi, 7, buf, 8192, 0, 1);     // a stream block
        readOne(fi, 4, buf, 4096, 0, 2);     // a StageData read
        require(ioc::cls[ioc::kStream].n.load() == 1 && ioc::cls[ioc::kStage].n.load() >= 1, "counted by class (stream and stage)");
        require(ioc::cls[ioc::kObj].n.load() >= 1, "the play window counts the object archives read before");
        port_io_play_end();
        FILE* f = fopen(logPath.c_str(), "r");
        require(f != nullptr, "log exists");
        std::string text;
        char line[2000];
        while (fgets(line, sizeof line, f)) text += line;
        fclose(f);
        require(text.find("io: stage TestGalaxy (0000abcd) play ") != std::string::npos, "the io: line names the stage and its key");
        require(text.find("io-more: stage TestGalaxy") != std::string::npos, "the detail line follows");
        require(text.find("game thread waited for the card: sync 0") != std::string::npos, "the game thread's waits are in it");
        require(text.find("prefetch 2 hits") != std::string::npos, "the prefetch hits are in the detail line");
        const size_t at = text.find("io: stage");
        const size_t end = text.find('\n', at);
        std::printf("io line is %u characters long\n", (unsigned)(end - at));
        require(end - at < 1000, "the first line fits the console log's line limit");
        // a window that is closed counts nothing
        const uint64_t n = ioc::cls[ioc::kStream].n.load();
        readOne(fi, 7, buf, 8192, 8192, 1);
        require(ioc::cls[ioc::kStream].n.load() == n, "reads outside a play window are not counted");
        std::puts("PASS: io census");
    }
    for (unsigned i = 1; i < 8; ++i) unlink(sFst[i].hostPath.c_str());
    unlink(listPath.c_str());
    unlink(logPath.c_str());
    unlink((logPath + ".prev").c_str());
    rmdir(root);
    std::puts("ALL PASS");
    std::fflush(stdout);
    _exit(0);
}
