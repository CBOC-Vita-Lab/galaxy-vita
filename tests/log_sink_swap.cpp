// Host test for port::LogSwap (platform/include/port/log_sink.h): no line lost or torn with a writer swapping while producers append.
// g++ -std=c++17 -O1 -pthread -Iplatform/include tests/log_sink_swap.cpp -o /tmp/lss && /tmp/lss
// (fix31: also the long line as one unit: SplitLong / LongLineBlock, the filter asked once, the pieces kept together by one Append)
#include "port/log_sink.h"
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
int main() {
    static char a[4096], b[4096];
    { // basics
        port::LogSwap s(a, b, 16);
        const char* d = nullptr;
        CHECK(s.Take(&d) == 0);
        CHECK(s.Append("12345678", 8));
        CHECK(s.Append("12345678", 8));
        CHECK(!s.Append("x", 1));            // full: rejected, nothing stored
        CHECK(s.Length() == 16);
        CHECK(s.Take(&d) == 16 && memcmp(d, "1234567812345678", 16) == 0);
        CHECK(s.Length() == 0);
        CHECK(s.Append("abc", 3));           // goes to the other buffer; the taken one is untouched
        CHECK(memcmp(d, "1234567812345678", 16) == 0);
        const char* d2; CHECK(s.Take(&d2) == 3 && memcmp(d2, "abc", 3) == 0 && d2 != d);
    }
    { // threads: 4 producers x 20000 lines, one writer swapping, the lock held for Append/Take only
        static char A[1 << 16], B[1 << 16];
        port::LogSwap s(A, B, sizeof A);
        std::mutex m;
        std::atomic<bool> stop{false};
        std::string out;
        std::thread w([&] {
            for (;;) {
                const char* d; size_t n;
                { std::lock_guard<std::mutex> g(m); n = s.Take(&d); }
                out.append(d, n);            // outside the lock, as the writer does
                if (!n && stop.load()) { std::lock_guard<std::mutex> g(m); if (s.Length() == 0) break; }
            }
        });
        std::vector<std::thread> ps;
        std::atomic<int> dropped{0};
        for (int t = 0; t < 4; ++t) ps.emplace_back([&, t] {
            for (int i = 0; i < 20000; ++i) {
                char l[48]; int n = snprintf(l, sizeof l, "T%d %06d\n", t, i);
                for (;;) { std::lock_guard<std::mutex> g(m); if (s.Append(l, n)) break; ++dropped; std::this_thread::yield(); }
            }
        });
        for (auto& p : ps) p.join();
        stop = true; w.join();
        int last[4] = {-1, -1, -1, -1}, cnt = 0; size_t pos = 0;
        while (pos < out.size()) {
            size_t e = out.find('\n', pos); CHECK(e != std::string::npos); if (e == std::string::npos) break;
            int t, i; CHECK(sscanf(out.c_str() + pos, "T%d %d", &t, &i) == 2);
            if (t >= 0 && t < 4) { CHECK(i == last[t] + 1); last[t] = i; }
            ++cnt; pos = e + 1;
        }
        CHECK(cnt == 80000);
    }
    { // fix31: a long line is one unit. Its pieces (<= 900 bytes, cut at a space, "+ " after the first) reassemble to the text.
        std::string text = "pace: ";   // (one periodic line: its first word names it, as the census lines do)
        for (int i = 0; text.size() < 2500; ++i) text += "k" + std::to_string(i) + "=" + std::to_string(i * 7) + " ";
        text.pop_back();
        std::vector<std::string> pieces;
        port::SplitLong(text.data(), text.size(), 900, [&](const char* p, size_t n, bool first) { CHECK(first == pieces.empty()); pieces.emplace_back(p, n); });
        CHECK(pieces.size() == 3);
        std::string back;
        for (const auto& x : pieces) { CHECK(x.size() <= 900 && x.size() > 450); back += (back.empty() ? "" : " ") + x; }
        CHECK(back == text);
        const auto periodic = [](const char* t, size_t n) { return n >= 5 && !memcmp(t, "pace:", 5); };
        // the filter decides for the whole line: a long periodic line is dropped whole (before, its "+ " pieces were written without their first line)
        CHECK(port::LongLineBlock("TS ", 3, text.data(), text.size(), 900, periodic).empty());
        int asked = 0;
        const auto count = [&](const char*, size_t) { ++asked; return false; };
        const std::string block = port::LongLineBlock("TS ", 3, text.data(), text.size(), 900, count);
        CHECK(asked == 1);                                   // (once, not once a piece)
        CHECK(block == "TS " + pieces[0] + "\nTS + " + pieces[1] + "\nTS + " + pieces[2] + "\n");
        // FATAL is never dropped
        const std::string fatal = "pace: FATAL " + text;
        bool urgent = false;
        CHECK(!port::LongLineBlock("", 0, fatal.data(), fatal.size(), 900, periodic, &urgent).empty() && urgent);
        // threads: long lines appended as one block per line under the lock, short lines beside them: a "+ " line always follows a piece of its own line
        static char A[1 << 16], B[1 << 16];
        port::LogSwap sw(A, B, sizeof A);
        std::mutex m;
        std::string out;
        std::atomic<bool> stop{false};
        std::thread w([&] {
            for (;;) {
                const char* d; size_t n;
                { std::lock_guard<std::mutex> g(m); n = sw.Take(&d); }
                out.append(d, n);
                if (!n && stop.load()) { std::lock_guard<std::mutex> g(m); if (sw.Length() == 0) break; }
            }
        });
        std::thread longs([&] {
            for (int i = 0; i < 300; ++i) {
                const std::string t = "L" + std::to_string(i) + " " + text;
                const std::string b = port::LongLineBlock("", 0, t.data(), t.size(), 900, [](const char*, size_t) { return false; });
                for (;;) { std::lock_guard<std::mutex> g(m); if (sw.Append(b.data(), b.size())) break; }
            }
        });
        std::thread shorts([&] {
            for (int i = 0; i < 3000; ++i) { char l[32]; int n = snprintf(l, sizeof l, "S %d\n", i); for (;;) { std::lock_guard<std::mutex> g(m); if (sw.Append(l, n)) break; } }
        });
        longs.join(); shorts.join(); stop = true; w.join();
        size_t pos = 0; int cont = 0, broken = 0; bool prevLong = false;
        while (pos < out.size()) {
            const size_t e = out.find('\n', pos);
            const bool isCont = out.compare(pos, 2, "+ ") == 0, isLong = out[pos] == 'L';
            if (isCont) { ++cont; if (!prevLong) ++broken; }
            prevLong = isLong || isCont;
            pos = e + 1;
        }
        CHECK(cont == 600 && broken == 0);
    }
    { // fable31fix S1: the display thread never writes the card: a full buffer drops and counts, FATAL only raises the urgent flag; every other thread writes as before
        port::LogSwap s(a, b, 32);
        port::DisplayDropRule rule;
        std::atomic<bool> urgent{false};
        int cardWrites = 0;
        // the shape of os_alarm.cpp's put(): returns whether the line was stored
        auto put = [&](bool display, const char* line, bool isUrgent) {
            bool full = !s.Append(line, strlen(line));
            if (isUrgent) urgent = true;
            bool stored = !full;
            if (full && !rule.OnFull(display)) return false;
            if (full) { const char* d; s.Take(&d); ++cardWrites; stored = s.Append(line, strlen(line)); }
            if (isUrgent && port::DisplayDropRule::MayWriteNow(display)) { const char* d; s.Take(&d); ++cardWrites; }
            return stored;
        };
        CHECK(put(true, "0123456789012345678901234567\n", false));    // 29 bytes fit
        CHECK(!put(true, "line two\n", false));                      // full, display thread: dropped, no card write
        CHECK(!put(true, "line three\n", false));
        CHECK(cardWrites == 0 && rule.Pending() == 2);
        CHECK(s.Length() == 29);                                       // nothing was taken or lost from what was there
        CHECK(!put(true, "FATAL x\n", true));                         // FATAL on the display thread: still no card write; the urgent flag is for the writer thread
        CHECK(cardWrites == 0 && urgent.load() && rule.Pending() == 3);
        CHECK(rule.Take() == 3 && rule.Take() == 0);                   // reported once, then cleared
        rule.OnFull(true); rule.Restore(rule.Take());                   // a report that did not fit gives the count back
        CHECK(rule.Pending() == 1);
        rule.Take();
        CHECK(put(false, "line two\n", false) && cardWrites == 1);     // another thread: the batch is written now, the line stored (as before)
        CHECK(rule.Pending() == 0);
        CHECK(put(false, "FATAL y\n", true) && cardWrites == 2);        // FATAL elsewhere: written at once
        // threads: a display-like thread and a normal one hammering a small buffer: the display thread's card writes stay 0
        port::LogSwap t(a, b, 64);
        std::mutex m;
        std::atomic<int> displayWrites{0}, otherWrites{0};
        std::atomic<bool> stop{false};
        port::DisplayDropRule r2;
        auto producer = [&](bool display, int n) {
            for (int i = 0; i < n; ++i) {
                bool full;
                { std::lock_guard<std::mutex> g(m); full = !t.Append("0123456789012345678\n", 20); }
                if (full && r2.OnFull(display)) { { std::lock_guard<std::mutex> g(m); const char* d; t.Take(&d); } ++otherWrites; }
                else if (full) { /* dropped */ }
            }
        };
        std::thread d([&] { producer(true, 20000); });
        std::thread o([&] { producer(false, 20000); });
        d.join(); o.join();
        CHECK(displayWrites == 0 && otherWrites > 0 && r2.Pending() > 0);
        (void)stop;
    }
    printf(fails ? "FAILED\n" : "log_sink_swap: ok\n");
    return fails != 0;
}
