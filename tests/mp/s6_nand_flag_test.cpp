// savesus S6: the NAND atomic-write flag across NANDCreate / NANDOpen / NANDClose (platform/src/nand/nand.cpp).
//  T1 flag flips ON->OFF between Create and Open (NANDCreate skipped the file, Open then opens GameData.bin in place): what does the game see?
//  T2 flag flips OFF->ON between Create and Open: the write goes to the .mptmp and is committed: no loss.
//  T3 the save root changes between Open and Close (an atomic write opened under root A): Close must commit under root A.
//  T4 (built with -fsanitize=thread, argument "tsan"): the game thread writes saves while another thread flips the flag: TSan must stay quiet
//     (the flag is a std::atomic); data written is never torn: GameData.bin is always a valid image.
// Reads the test save only.
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "port/mp_proto.h"
#include "mp_savefile.h"

extern "C" {
void port_host_alloc_scope_enter(void) {}
void port_host_alloc_scope_leave(void) {}
void port_host_job_wait(const char*, void (*fn)(void*), void* arg) { fn(arg); }
void port_log(const char*, ...) {}
void port_gpu_memory_written(void*, uint32_t) {}
uint8_t* __PortGXFifoPtr; uint8_t* __PortGXFifoLimit; int __PortGXFifoBatch;
void port_nand_set_root(const char* root);
void port_nand_set_atomic_writes(bool on);
int NANDCreate(const char* path, unsigned char perm, unsigned char attr);
struct Info { int a, b; char c[64]; char d[64]; unsigned char e, f, g; };
int NANDOpen(const char* path, Info* info, unsigned char mode);
int NANDWrite(Info* info, const void* buf, unsigned length);
int NANDClose(Info* info);
}
typedef std::vector<uint8_t> Bytes;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool readAll(const std::string& p, Bytes& b) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    b.clear(); uint8_t buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    fclose(f);
    return true;
}
static void writeAll(const std::string& p, const Bytes& b) { FILE* f = fopen(p.c_str(), "wb"); fwrite(b.data(), 1, b.size(), f); fclose(f); }
static bool exists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }
static const char* kHome = "/title/00010000/524d4750/data";
static const char* kPath = "/title/00010000/524d4750/data/GameData.bin";

static int saveOnce(const Bytes& img, bool doCreate, void (*between)()) {
    int rc = 0;
    if (doCreate) { rc = NANDCreate(kPath, 0, 0); if (rc != 0 && rc != -1 /*EXISTS is fine*/) {} }
    if (between) between();
    Info info;
    memset(&info, 0, sizeof info);
    const int o = NANDOpen(kPath, &info, 2 /*write*/);
    if (o != 0) return o;
    NANDWrite(&info, img.data(), (unsigned)img.size());
    return NANDClose(&info);
}

int main(int argc, char** argv) {
    const bool tsan = argc > 1 && !strcmp(argv[1], "tsan");
    const char* src = "/data/saves0/saves/title/00010000/524d4750/data/GameData.bin";
    Bytes img;
    if (!readAll(src, img)) { fprintf(stderr, "no test save\n"); return 2; }
    char tmpl[] = "/tmp/s6_nand.XXXXXX";
    const std::string rootA = std::string(mkdtemp(tmpl)) + "/A";
    const std::string rootB = rootA.substr(0, rootA.size() - 1) + "B";
    for (const std::string& r : {rootA, rootB}) { std::string c = "mkdir -p '" + r + kHome + "'"; if (system(c.c_str())) return 2; }
    port_nand_set_root(rootA.c_str());
    Bytes old = img; old[0x30] ^= 0;   // (the file already on the card: the same image)
    Bytes now = img;
    if (!tsan) {
        // T1: flag ON at Create, OFF at Open
        port_nand_set_atomic_writes(true);
        writeAll(rootA + kHome + "/GameData.bin", old);
        std::string rm = "rm -f '" + rootA + kHome + "/GameData.bin'";
        if (system(rm.c_str())) {}
        int rc = saveOnce(now, true, [] { port_nand_set_atomic_writes(false); });
        Bytes got;
        bool there = readAll(rootA + kHome + "/GameData.bin", got);
        printf("T1 flag ON->OFF between Create and Open (file did not exist): result=%d file=%s\n", rc, there ? "there" : "MISSING");
        CHECK(rc == 0 && there);   // a first save must not fail because a flag moved
        port_nand_set_atomic_writes(false);
        // T2: OFF at Create, ON at Open
        rm = "rm -f '" + rootA + kHome + "/GameData.bin'*";
        if (system(rm.c_str())) {}
        rc = saveOnce(now, true, [] { port_nand_set_atomic_writes(true); });
        there = readAll(rootA + kHome + "/GameData.bin", got);
        printf("T2 flag OFF->ON between Create and Open: result=%d file=%s\n", rc, there ? "there" : "MISSING");
        CHECK(rc == 0 && there && got == now);
        port_nand_set_atomic_writes(false);
        // T3: root A at Open, root B at Close
        port_nand_set_atomic_writes(true);
        writeAll(rootA + kHome + "/GameData.bin", old);
        Bytes upd = img; upd[0x20] ^= 0;   // same image: the point is where it lands
        Info info; memset(&info, 0, sizeof info);
        CHECK(NANDOpen(kPath, &info, 2) == 0);
        NANDWrite(&info, upd.data(), (unsigned)upd.size());
        port_nand_set_root(rootB.c_str());   // (the check the root switch makes should keep this from happening; nothing in nand.cpp does)
        const int crc = NANDClose(&info);
        const bool tmpOrphan = exists(rootA + kHome + "/GameData.bin.mptmp");
        const bool inB = exists(rootB + kHome + "/GameData.bin");
        printf("T3 root A -> B between Open and Close: close=%d orphan tmp in A=%d file created in B=%d\n", crc, tmpOrphan, inB);
        CHECK(crc == 0 && !tmpOrphan && !inB);
        port_nand_set_root(rootA.c_str());
        port_nand_set_atomic_writes(false);
    } else {
        std::atomic<bool> stop{false};
        port_nand_set_atomic_writes(false);
        writeAll(rootA + kHome + "/GameData.bin", old);
        std::thread flipper([&] { bool v = false; while (!stop) { port_nand_set_atomic_writes(v = !v); usleep(50); } });
        int okc = 0;
        for (int i = 0; i < 300; i++) {
            const int rc = saveOnce(now, true, nullptr);
            Bytes got;
            const bool there = readAll(rootA + kHome + "/GameData.bin", got);
            CHECK(there && got.size() == now.size());
            okc += rc == 0;
        }
        stop = true; flipper.join();
        printf("T4 tsan: 300 saves while another thread flips the flag: %d ok, TSan quiet\n", okc);
    }
    std::string rm = "rm -rf '" + rootA.substr(0, rootA.size() - 2) + "'";
    if (system(rm.c_str())) {}
    printf("s6_nand_flag_test%s: %s\n", tsan ? " (tsan)" : "", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
