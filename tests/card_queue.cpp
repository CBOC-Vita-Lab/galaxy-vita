// hitch30: host proof of port/card_queue.h (the card-write queue's pool, order, coalescing, drops and file work).
// nice g++ -std=c++17 -Iplatform/include tests/card_queue.cpp -pthread -o /tmp/card_queue && /tmp/card_queue
#include "port/card_queue.h"

#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <string>

using namespace port::card;
static int fails = 0;
#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);        \
            ++fails;                                                \
        }                                                           \
    } while (0)

static std::string ReadAll(const char* p) {
    std::string s;
    FILE* f = std::fopen(p, "rb");
    if (!f) return "<none>";
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    std::fclose(f);
    return s;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    char dir[] = "/tmp/card_queue_XXXXXX";
    if (!mkdtemp(dir)) return 2;
    auto P = [&](const char* n) { return std::string(dir) + "/" + n; };
    static Queue q;  // (~250 KiB: static)

    // 1. replace, append, order: appends keep their order, a replace after them ends the file.
    CHECK(q.Enqueue(P("a.txt").c_str(), kAppend, "one\n", 4));
    CHECK(q.Enqueue(P("a.txt").c_str(), kAppend, "two\n", 4));
    CHECK(q.Enqueue(P("b.txt").c_str(), kReplace, "first", 5));
    CHECK(q.Pending() == 3);
    CHECK(q.RunAll() == 3 && q.Pending() == 0);
    CHECK(ReadAll(P("a.txt").c_str()) == "one\ntwo\n");
    CHECK(ReadAll(P("b.txt").c_str()) == "first");
    CHECK(ReadAll((P("b.txt") + ".tmp").c_str()) == "<none>");  // (no temporary left)

    // 2. two replaces of one path before the writer runs: the older is skipped, the newer wins; a replace of another path is not skipped.
    CHECK(q.Enqueue(P("b.txt").c_str(), kReplace, "second", 6));
    CHECK(q.Enqueue(P("c.txt").c_str(), kReplace, "cc", 2));
    CHECK(q.Enqueue(P("b.txt").c_str(), kReplace, "third", 5));
    CHECK(q.RunAll() == 2);
    CHECK(q.st.coalesced.load() == 1);
    CHECK(ReadAll(P("b.txt").c_str()) == "third" && ReadAll(P("c.txt").c_str()) == "cc");

    // 3. a replace that replaces a longer file leaves no tail; an append after a replace appends.
    CHECK(q.Enqueue(P("b.txt").c_str(), kReplace, "x", 1));
    CHECK(q.Enqueue(P("b.txt").c_str(), kAppend, "y", 1));
    q.RunAll();
    CHECK(ReadAll(P("b.txt").c_str()) == "xy");

    // 4. the pool is bounded: 12 slots; more are dropped, counted, never blocked. Payload classes: 4 KiB goes to a small slot, 30 KiB to a big one.
    std::vector<char> big(70 * 1024, 'z');
    CHECK(!q.Enqueue(P("big").c_str(), kReplace, big.data(), static_cast<uint32_t>(big.size())));  // oversize
    CHECK(q.st.oversize.load() == 1);
    uint32_t accepted = 0;
    for (int i = 0; i < 40; ++i)
        if (q.Enqueue(P("d.txt").c_str(), kAppend, big.data(), 3000)) ++accepted;
    CHECK(accepted == kSlots);  // (small payloads may use the larger slots too)
    CHECK(q.st.dropped.load() == 40 - kSlots);
    CHECK(q.RunAll() == kSlots);
    CHECK(ReadAll(P("d.txt").c_str()).size() == kSlots * 3000u);
    // a 60 KiB payload needs a big slot: with both free it works, with both taken it drops while smaller slots are free
    CHECK(q.Enqueue(P("e1").c_str(), kReplace, big.data(), 60 * 1024));
    CHECK(q.Enqueue(P("e2").c_str(), kReplace, big.data(), 60 * 1024));
    const uint32_t d0 = q.st.dropped.load();
    CHECK(!q.Enqueue(P("e3").c_str(), kReplace, big.data(), 60 * 1024) && q.st.dropped.load() == d0 + 1);
    CHECK(q.Enqueue(P("e4").c_str(), kReplace, "small", 5));  // (small slots are still free)
    q.RunAll();
    CHECK(ReadAll(P("e1").c_str()).size() == 60 * 1024);

    // 5. bad arguments are refused without a count that hides them: long path, empty payload, bad mode.
    std::string longPath(200, 'p');
    CHECK(!q.Enqueue(longPath.c_str(), kReplace, "a", 1));
    CHECK(!q.Enqueue(P("f").c_str(), kReplace, "", 0));
    CHECK(!q.Enqueue(P("f").c_str(), 7, "a", 1));

    // 6. a failing path (a directory that does not exist) counts as failed, the slot is freed.
    CHECK(q.Enqueue("/nonexistent_dir_hitch30/x.txt", kReplace, "a", 1));
    CHECK(q.RunAll() == 1 && q.st.failed.load() == 1 && q.Pending() == 0);

    // 7. producers on several threads while a consumer runs: every accepted write is done exactly once, in per-producer order (appends of a
    //    producer's numbers to its own file come out ascending), and nothing is lost or doubled.
    {
        std::atomic< bool > stop{false};
        std::thread w([&] {
            while (!stop.load() || q.Pending()) q.RunAll();
        });
        std::vector< std::thread > ps;
        std::vector< uint32_t > acc(3, 0);
        for (int t = 0; t < 3; ++t)
            ps.emplace_back([&, t] {
                const std::string path = P(("p" + std::to_string(t) + ".txt").c_str());
                for (int i = 0; i < 400; ++i) {
                    char b[16];
                    const int n = std::snprintf(b, sizeof b, "%04d\n", i);
                    // (a dropped write is the caller's to count: here a retry loop would hide drops, so the test keeps what was accepted)
                    if (q.Enqueue(path.c_str(), kAppend, b, static_cast< uint32_t >(n))) {
                        ++acc[t];
                    } else {
                        std::this_thread::sleep_for(std::chrono::microseconds(50));
                    }
                }
            });
        for (auto& p : ps) p.join();
        stop.store(true);
        w.join();
        for (int t = 0; t < 3; ++t) {
            const std::string s = ReadAll(P(("p" + std::to_string(t) + ".txt").c_str()).c_str());
            CHECK(s.size() == acc[t] * 5u);
            int last = -1;
            bool asc = true;
            for (size_t i = 0; i + 4 < s.size() + 1 && i + 4 <= s.size(); i += 5) {
                const int v = std::atoi(s.substr(i, 4).c_str());
                if (v <= last) asc = false;
                last = v;
            }
            CHECK(asc);
            std::printf("test7: producer %d: %u of 400 accepted, file %zu bytes\n", t, acc[t], s.size());
        }
    }
    char line[400];
    q.Line(line, sizeof line);
    std::printf("%s\n", line);
    std::string cmd = std::string("rm -rf ") + dir;
    if (system(cmd.c_str()) != 0) {}
    std::printf(fails ? "FAILED %d\n" : "card_queue: all passed\n", fails);
    return fails ? 1 : 0;
}
