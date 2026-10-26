// Real DVD worker and basic logger, with synthetic files and no game or device.
#include <array>
#include <atomic>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <string>

#include "../platform/src/dvd/dvd.cpp"

namespace {
struct WorkerFinished {};
unsigned callbacks;
uint32_t resultHash = 2166136261u;
bool expectFailure;
std::atomic<unsigned> interrupts;

void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void completed(s32 result, DVDFileInfo* file) {
    require(result == (expectFailure ? -1 : 32), "actual callback result is unchanged");
    require(file->cb.state == (expectFailure ? kStateFatal : kStateEnd), "actual command state is unchanged");
    require(file->cb.transferredSize == (expectFailure ? 0u : 32u), "actual transfer count is unchanged");
    const auto* bytes = static_cast<const unsigned char*>(file->cb.addr);
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned char want = expectFailure ? 0xCC : (i < 8 ? static_cast<unsigned char>(i + 1) : 0);
        require(bytes[i] == want, "failed destination or successful EOF padding is unchanged");
        resultHash = (resultHash ^ bytes[i]) * 16777619u;
    }
    resultHash = (resultHash ^ static_cast<uint32_t>(result)) * 16777619u;
    resultHash = (resultHash ^ static_cast<uint32_t>(file->cb.state)) * 16777619u;
    ++callbacks;
}
void readEntry(unsigned entry, bool failure) {
    expectFailure = failure;
    DVDFileInfo file{};
    std::array<unsigned char, 32> bytes;
    bytes.fill(0xCC);
    require(DVDFastOpen(entry, &file), "FST-listed entry retains its open semantics");
    require(DVDReadAsyncPrio(&file, bytes.data(), 32, 0, completed, 2), "real async request submits");
    const unsigned before = callbacks;
    std::thread worker([] {
        try { workerMain(); } catch (const WorkerFinished&) {}
    });
    worker.join();
    dvdIrq();
    require(callbacks == before + 1 && sDone.empty() && sPending.empty(), "exactly one completed callback");
    require(DVDGetDriveStatus() == 0, "failure does not change drive idle semantics");
}
std::string readLog(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
unsigned count(const std::string& text, const std::string& needle) {
    unsigned n = 0;
    for (size_t p = 0; (p = text.find(needle, p)) != std::string::npos; p += needle.size()) ++n;
    return n;
}
void writeFixture(const std::string& path) {
    FILE* f = fopen(path.c_str(), "wb");
    const unsigned char bytes[] = {1, 2, 3, 4, 5, 6, 7, 8};
    require(f && fwrite(bytes, 1, sizeof(bytes), f) == sizeof(bytes) && fclose(f) == 0, "synthetic source written");
}
}

extern "C" void port_host_alloc_scope_enter(void) {}
extern "C" void port_host_alloc_scope_leave(void) {}
extern "C" void port_gpu_memory_written(void*, uint32_t) {}
extern "C" void OSWakeupThread(OSThreadQueue*) {}
extern "C" void port_irq_raise(int irq) {
    require(irq == PORT_IRQ_DVD, "actual worker raises DVD completion");
    ++interrupts;
    throw WorkerFinished{};
}

int main(int argc, char** argv) {
    require(argc == 2, "expected enabled or disabled");
    const bool enabled = std::string(argv[1]) == "enabled";
    char root[] = "/tmp/galaxy-dvd-errors-XXXXXX";
    require(mkdtemp(root), "private fixture directory");
    const std::string directory(root), log = directory + "/basic.log";
    port_log_file(log.c_str());
    port_log("ordinary diagnostic must stay quiet");
    sFst.resize(68);
    for (unsigned i = 0; i < sFst.size(); ++i) {
        sFst[i].hostPath = directory + "/file-" + std::to_string(i);
        sFst[i].isDir = false;
        sFst[i].size = -1;
    }
    writeFixture(sFst[1].hostPath);
    require(mkdir(sFst[2].hostPath.c_str(), 0700) == 0, "directory produces a real read error");

    // Missing diagnostics must not affect the existing size-zero/open/callback contract.
    require(entrySize(0) == 0, "missing file still has size zero");
    require(errno == ENOENT, "diagnostic does not change the failing syscall errno");
    const std::string first = readLog(log);
    require(first.find("ordinary diagnostic") == std::string::npos, "real basic logger suppresses port_log");
    require(enabled ? first.find("dvd-error: operation=stat") != std::string::npos : first.empty(),
            "opt-in missing-file diagnostic reaches the real quiet-build log");
    if (enabled) {
        require(first.find(sFst[0].hostPath) != std::string::npos, "missing diagnostic identifies the exact path");
        require(first.find("result=-1 errno=" + std::to_string(ENOENT)) != std::string::npos, "missing diagnostic retains syscall errno");
    }
    // The setting is read once. Later environment edits cannot enable/disable it.
    setenv("GXV_DVDERRORS", enabled ? "0" : "1", 1);
    for (unsigned i = 0; i < 256; ++i) readEntry(0, true);
    const std::string repeated = readLog(log);
    if (enabled) {
        require(count(repeated, "operation=open") == 3, "256 identical failures log only attempts 1, 8 and 64");
        require(repeated.find("repeat=64") != std::string::npos, "bounded retry count is visible");
    } else require(repeated.empty(), "disabled switch remains quiet after environment changes");

    // Repair and retry use the same real worker; successful padded reads never log an error.
    writeFixture(sFst[0].hostPath);
    readEntry(0, false);
    readEntry(1, false);
    require(readLog(log) == repeated, "recovery and expected EOF padding produce no diagnostic");
    readEntry(2, true);
    const std::string readError = readLog(log);
    if (enabled) {
        require(readError.find("operation=read") != std::string::npos && readError.find(sFst[2].hostPath) != std::string::npos,
                "real pread failure names the operation and path");
        require(readError.find("errno=" + std::to_string(EISDIR)) != std::string::npos, "real pread errno survives clock/log calls");
    }
    // The real game-side stat and host worker can fail concurrently; each owns a different FST entry.
    require(entrySize(3) == 0, "next missing entry is sized before concurrent work");
    std::thread statter([] { require(entrySize(67) == 0, "concurrent guest-side stat retains missing-file semantics"); });
    readEntry(3, true);
    statter.join();
    // Changed paths remain visible; then a global ceiling bounds even many unique failures.
    for (unsigned i = 4; i < sFst.size(); ++i) readEntry(i, true);
    const std::string all = readLog(log);
    if (enabled) {
        require(all.find(sFst[3].hostPath) != std::string::npos, "a changed failing path is not hidden by repeated first error");
        require(count(all, "dvd-error:") == 24, "process-wide ceiling bounds unique failures too");
        require(all.find("further errors suppressed") != std::string::npos, "the last record explains the ceiling");
        std::fprintf(stderr, "%s", all.c_str());
    } else require(all.empty(), "all disabled failures remain absent from the basic log");
    require(callbacks == 324 && interrupts == callbacks, "all repeated, repaired and distinct reads completed");
    std::printf("PASS: 324 real DVD callbacks; results-and-bytes=%08x; quiet logger; synthetic host fixture\n", resultHash);

    for (auto& slot : sOpenFiles) if (slot.fd >= 0) { close(slot.fd); slot.fd = -1; }
    require(unlink(sFst[0].hostPath.c_str()) == 0 && unlink(sFst[1].hostPath.c_str()) == 0, "fixture files removed");
    require(rmdir(sFst[2].hostPath.c_str()) == 0 && unlink(log.c_str()) == 0 && rmdir(root) == 0, "fixture removed");
}
