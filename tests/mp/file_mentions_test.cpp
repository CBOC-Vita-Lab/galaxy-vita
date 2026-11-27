// optfix item 10: mp_file_mentions (the Wi-Fi Turbo detection's file search, mp_core_vita.cpp) reads in 1 KB chunks instead of a 32 KB
// static buffer. It must answer exactly as the old whole-prefix read did: case-insensitive, first 32 KB only, a needle across a chunk
// boundary found, a missing file "not mentioned".   bash tests/mp/file_mentions_build.sh
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unistd.h>

extern bool mp_file_mentions(const char* path, const char* needle);
static int gChecks, gFails;
#define CHECK(c) do { ++gChecks; if (!(c)) { ++gFails; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::string gPath;
static void put(const std::string& body) { FILE* f = fopen(gPath.c_str(), "wb"); fwrite(body.data(), 1, body.size(), f); fclose(f); }

void mp_sys_sleep_ms(unsigned) {}
int main() {
    char tmpl[] = "/tmp/fmtestXXXXXX";
    int fd = mkstemp(tmpl);
    close(fd);
    gPath = tmpl;
    const char* needle = "wifiturbo_kernel.skprx";
    CHECK(!mp_file_mentions("/nonexistent/file", needle));
    put("");
    CHECK(!mp_file_mentions(gPath.c_str(), needle));
    put("*KERNEL\nur0:tai/WiFiTurbo_Kernel.skprx\n");
    CHECK(mp_file_mentions(gPath.c_str(), needle));   // (case-insensitive)
    put("*KERNEL\nur0:tai/other.skprx\n");
    CHECK(!mp_file_mentions(gPath.c_str(), needle));
    // the needle at every offset around the chunk boundaries (1024, 2048) and the end of the 32 KB prefix
    const size_t n = strlen(needle);
    int found = 0, wanted = 0;
    for (size_t off = 0; off < 3300; off++) {
        std::string body(off, 'x');
        body += needle;
        body += std::string(40, 'y');
        put(body);
        wanted++;
        if (mp_file_mentions(gPath.c_str(), needle)) found++;
    }
    CHECK(found == wanted);
    for (size_t off : {(size_t)32768 - n, (size_t)32768 - n - 1, (size_t)32768 - n + 1, (size_t)32768, (size_t)32773}) {
        std::string body(off, 'x');
        body += needle;
        put(body);
        const bool want = off + n <= 32 * 1024;   // (the first 32 KB only, as before)
        CHECK(mp_file_mentions(gPath.c_str(), needle) == want);
    }
    // a partial needle at a boundary that does not complete
    std::string body(1024 - 5, 'x');
    body += "wifit";
    body += std::string(100, 'z');
    put(body);
    CHECK(!mp_file_mentions(gPath.c_str(), needle));
    unlink(gPath.c_str());
    printf("file_mentions_test: %d checks, %d failed\n", gChecks, gFails);
    return gFails ? 1 : 0;
}
