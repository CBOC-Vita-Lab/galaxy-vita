// Host test for platform/include/port/bootio.h (the retry of start-up opens).
//   g++ -std=c++17 -Iplatform/include tests/bootio_retry.cpp -o /tmp/bootio_retry && /tmp/bootio_retry
#include "port/bootio.h"

#include <string>
#include <vector>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { ++fails; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int64_t gNow = 0;
static std::vector<std::string> gLines;
static int64_t FakeNow() { return gNow; }
static void FakeSleep(unsigned ms) { gNow += ms; }
static void FakeLog(const char* l) { gLines.push_back(l); }
static int FakeFds() { return 77; }
static bootio::Env FakeEnv() { bootio::Env e; e.nowMs = FakeNow; e.sleepMs = FakeSleep; e.log = FakeLog; e.fdCount = FakeFds; return e; }

int main() {
    bootio::Options opt;
    // 1. the first 5 tries fail with EMFILE (0x80010018), then it opens: 6 tries, 250 ms, a failure line and a success line
    {
        gNow = 0; gLines.clear(); int n = 0;
        auto o = bootio::Retry("t", "ux0:x", opt, [&]() { bootio::Attempt a; if (++n <= 5) { a.err = EMFILE; a.sce = 0x80010018u; } else a.ok = true; return a; }, FakeEnv());
        CHECK(o.ok && o.tries == 6 && o.elapsedMs == 250);
        CHECK(gLines.size() == 2);
        CHECK(gLines[0].find("0x80010018") != std::string::npos && gLines[0].find("77 descriptors") != std::string::npos);
        CHECK(gLines[1].find("after 6 tries") != std::string::npos);
    }
    // 2. a permanent failure stops at the budget (3000 ms, 50 ms steps), logs the first failure and the give-up, and never sleeps past it
    {
        gNow = 0; gLines.clear(); int n = 0;
        auto o = bootio::Retry("t", "ux0:x", opt, [&]() { ++n; bootio::Attempt a; a.err = EBUSY; a.sce = 0x80010010u; return a; }, FakeEnv());
        CHECK(!o.ok && o.err == EBUSY && o.sce == 0x80010010u);
        CHECK(o.elapsedMs <= 3000 && o.elapsedMs >= 2950 && n == o.tries && n >= 60 && n <= 62);
        CHECK(gLines.size() == 2 && gLines[1].find("gave up") != std::string::npos);
    }
    // 3. "no such file" with missingMs = 0: one try, silent; with 100: about 100 ms
    {
        gNow = 0; gLines.clear(); int n = 0;
        bootio::Options o0; o0.missingMs = 0;
        auto o = bootio::Retry("t", "p", o0, [&]() { ++n; bootio::Attempt a; a.err = ENOENT; return a; }, FakeEnv());
        CHECK(!o.ok && n == 1 && gLines.empty() && gNow == 0);
        bootio::Options o1; o1.missingMs = 100; n = 0;
        o = bootio::Retry("t", "p", o1, [&]() { ++n; bootio::Attempt a; a.err = ENOENT; return a; }, FakeEnv());
        CHECK(!o.ok && o.elapsedMs <= 100 && n >= 2 && n <= 3);
    }
    // 4. success at once: one try, no line
    {
        gNow = 0; gLines.clear();
        auto o = bootio::Retry("t", "p", opt, [&]() { bootio::Attempt a; a.ok = true; return a; }, FakeEnv());
        CHECK(o.ok && o.tries == 1 && gLines.empty());
    }
    // 5. real file + GXV_BOOTIO_INJECT=3: the tag "fst.bin" fails 3 times (EIO), then opens; another tag is not touched; real clock, bounded
    {
        const char* path = "/tmp/bootio_test_file.bin";
        FILE* f = fopen(path, "wb"); CHECK(f); if (f) { fputc(1, f); fclose(f); }
        setenv("GXV_BOOTIO_INJECT", "3", 1);
        gLines.clear();
        bootio::Env env = FakeEnv(); env.nowMs = nullptr; env.sleepMs = nullptr;  // (real time)
        const int64_t t0 = bootio::DefaultNowMs();
        int fd = bootio::OpenRetry(path, O_RDONLY, "fst.bin", opt, env);
        const int64_t ms = bootio::DefaultNowMs() - t0;
        CHECK(fd >= 0); if (fd >= 0) close(fd);
        CHECK(ms >= 140 && ms < 400);
        CHECK(gLines.size() == 2 && gLines[0].find("errno 5") != std::string::npos && gLines[1].find("after 4 tries") != std::string::npos);
        int fd2 = bootio::OpenRetry(path, O_RDONLY, "other", opt, env);  // (the counter is spent and other tags never fail)
        CHECK(fd2 >= 0); if (fd2 >= 0) close(fd2);
        FILE* g = bootio::FopenRetry(path, "rb", "fst.bin", opt, env);
        CHECK(g); if (g) fclose(g);
        unlink(path);
    }
    // 6. a missing file with the real clock and the default policy gives up within 3 s + one step
    {
        gLines.clear();
        bootio::Env env = FakeEnv(); env.nowMs = nullptr; env.sleepMs = nullptr;
        const int64_t t0 = bootio::DefaultNowMs();
        FILE* g = bootio::FopenRetry("/nonexistent/dir/file", "rb", "t", opt, env);
        const int64_t ms = bootio::DefaultNowMs() - t0;
        CHECK(!g && errno == ENOENT && ms >= 2900 && ms <= 3300);
        CHECK(gLines.size() == 2);
    }
    printf(fails ? "bootio_retry: %d FAILED\n" : "bootio_retry: all passed\n", fails);
    return fails != 0;
}
