// dive27: host test of port/rehearse.h (the registry and the runner).
#include <cstdio>
#include <cstdlib>
#include "port/rehearse.h"
using namespace port::rehearse;
static int calls[4];
static int fa() { ++calls[0]; return 3; }
static int fb() { ++calls[1]; return kSkip; }
static int fc() { ++calls[2]; return calls[2] < 3 ? kRetry : 1; }
static int fd() { ++calls[3]; return kRetry; }
static uint64_t clk = 0;
static uint64_t Now() { return clk += 100; }
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
int main() {
    static Registrar ra("a", fa), rb("b", fb), rc("c", fc), rd("d", fd);
    CHECK(g_reg.n == 4);
    CHECK(!add("a", fa));          // a name is kept once
    CHECK(!add(nullptr, fa));
    Runner r;
    CHECK(r.Sync(7));
    CHECK(!r.Sync(7));
    CHECK(r.Pending() == 4 && !r.started);
    int opens = 0, closes = 0;
    auto open = [&] { ++opens; };
    auto close = [&] { ++closes; };
    unsigned ran = r.Step(Now, 1000000, open, close);
    CHECK(ran == 4 && opens == 4 && closes == 4 && r.started);
    CHECK(r.r[0].state == kDone && r.r[0].draws == 3);
    CHECK(r.r[1].state == kSkipped);
    CHECK(r.r[2].state == kTodo && r.r[2].tries == 1);
    CHECK(r.Pending() == 2);
    r.Step(Now, 1000000, open, close);
    CHECK(r.r[2].state == kTodo && r.r[2].tries == 2);
    r.Step(Now, 1000000, open, close);
    CHECK(r.r[2].state == kDone && calls[0] == 1 && calls[1] == 1);
    for (unsigned i = 0; i < kMaxTries; ++i) r.Step(Now, 1000000, open, close);
    CHECK(r.r[3].state == kGaveUp && r.Pending() == 0);
    CHECK(r.Count(kDone) == 2 && r.Count(kSkipped) == 1 && r.Count(kGaveUp) == 1);
    // a budget of 0 still runs one entry a picture
    Runner q;
    q.Sync(8);
    clk = 0;
    CHECK(q.Step(Now, 0, open, close) == 1);
    // a new scene starts again
    CHECK(q.Sync(9) && q.Pending() == 4 && !q.started);
    std::puts("rehearse_model: ok");
    return 0;
}
