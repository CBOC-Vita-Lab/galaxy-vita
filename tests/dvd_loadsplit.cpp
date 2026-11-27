// cook24: the "load-split:" counters of the disc reader (reads / bytes / drive time per folder, probes, async reads, opens, stats, decode notes): real worker, synthetic files.
// (the original header of the fixture follows)
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

int main() {
    char root[] = "/tmp/galaxy-dvd-split-XXXXXX";
    require(mkdtemp(root), "private fixture directory");
    const std::string directory(root);
    require(loadsplit::classOf("ObjectData/Foo.arc") == PORT_DISC_OBJECT && loadsplit::classOf("stagedata/Bar.arc") == PORT_DISC_STAGE &&
                loadsplit::classOf("LayoutData/L.arc") == PORT_DISC_LAYOUT && loadsplit::classOf("AudioRes/x.afc") == PORT_DISC_AUDIO &&
                loadsplit::classOf("Movie/a.thp") == PORT_DISC_OTHER && loadsplit::classOf("ObjectDataX") == PORT_DISC_OTHER,
            "folder classes");
    sFst.resize(4);
    for (unsigned i = 0; i < sFst.size(); ++i) {
        sFst[i].hostPath = directory + "/file-" + std::to_string(i);
        sFst[i].isDir = false;
        sFst[i].size = -1;
        sFst[i].cls = (uint8_t)(i % PORT_DISC_CLASSES);
    }
    constexpr unsigned kBlock = 4096;
    for (unsigned i = 1; i <= 3; ++i) writeStream(sFst[i].hostPath, kBlock * 4);
    for (auto& d : sReopenDelayMs) d = 0;
    PortDiscSplit a, b;
    port_load_split_disc(&a);
    unsigned char block[kBlock];
    require(readBlock(1, block, kBlock, 0) == (s32)kBlock, "read 1");
    require(readBlock(1, block, 100, kBlock) == 100, "read 2");
    require(readBlock(2, block, kBlock, 0) == (s32)kBlock, "read 3");
    port_load_note_decomp(65536, 2500000);
    port_load_split_disc(&b);
    require(b.reads[1] - a.reads[1] == 2 && b.reads[2] - a.reads[2] == 1, "reads counted per folder (entry 1 = class 1, entry 2 = class 2)");
    require(b.bytes[1] - a.bytes[1] == kBlock + 100 && b.bytes[2] - a.bytes[2] == kBlock, "bytes counted per folder");
    require(b.asyncReads - a.asyncReads == 3, "async reads counted");
    require(b.workerNs[1] - a.workerNs[1] > 0, "drive time counted");
    require(b.opens - a.opens >= 2, "opens counted (descriptor cache misses)");
    require(b.decodes - a.decodes == 1 && b.decodeBytes - a.decodeBytes == 65536 && b.decodeNs - a.decodeNs == 2500000, "decoder notes counted");
    char name[128]; double ms = 0;
    require(port_load_split_slowest(name, sizeof name, &ms) == 1 && ms >= 0, "slowest read reported");
    require(port_load_split_slowest(name, sizeof name, &ms) == 0, "...and reset");
    char text[600];
    const int n = port::loadsplit::DiscText(text, sizeof text, a, b);
    require(n > 0 && strstr(text, "3 reads") && strstr(text, "StageData 2") && strstr(text, "LayoutData 1"), "the log text names the folders");
    std::printf("PASS: load-split disc counters: %s\n", text);
    for (auto& slot : sOpenFiles) if (slot.fd >= 0) { close(slot.fd); slot.fd = -1; }
    for (unsigned i = 1; i <= 3; ++i) unlink(sFst[i].hostPath.c_str());
    rmdir(root);
}
