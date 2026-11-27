// parprio31: a scheduling model of core 3 (not the console). Two threads share it: the finishing thread (bursts of work, 5-9 ms a 33 ms pair in the 12 Nov
// logs) and the particle worker (wakes once a turn, needs C us of CPU, sleeps). The game thread joins on the worker, so its wait is the worker's wake-to-done time.
// Equal priority: the kernel rotates by time slice (a woken thread queues behind the running one; a running thread is not pre-empted by an equal). Worker above:
// pre-empts at once. The model shows the shape (2-4 ms waits at equal priority, none above), not the console's numbers.
//   g++ -std=c++17 -O1 tests/par_prio_model.cpp -o par_prio_model && ./par_prio_model
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

struct Result { std::vector<int> waits; };

// us-resolution simulation of one pair (33333 us, two turns). fin: burst intervals [start,end). worker job arrives at t0 needing c.
static int Wait(const std::vector<std::pair<int,int>>& fin, int t0, int c, int slice, bool above) {
    // equal: worker starts at the first moment the finishing thread is not running, or when the slice of the current finishing burst expires.
    int t = t0, left = c;
    auto finRunning = [&](int x) { for (auto& b : fin) if (x >= b.first && x < b.second) return true; return false; };
    int sliceEnd = -1;  // the end of the finishing thread's current quantum (equal priority)
    int ran = 0;
    for (; left > 0; ++t) {
        if (above) { --left; continue; }
        const bool f = finRunning(t);
        if (!f) { --left; sliceEnd = -1; continue; }
        // the finishing thread holds the core; the worker gets it only when the finishing thread's quantum ends
        if (sliceEnd < 0) sliceEnd = t + slice;
        if (t >= sliceEnd) { // rotation: the worker runs one quantum (or to the end of its job)
            int q = std::min(slice, left); left -= q; t += q - 1; sliceEnd = -1; ran += q;
        }
    }
    (void)ran;
    return t - t0;
}

int main() {
    std::mt19937 rng(31);
    const int pairs = 20000, c = 300;
    for (int above = 0; above <= 1; ++above) {
        std::vector<int> w;
        for (int p = 0; p < pairs; ++p) {
            std::vector<std::pair<int,int>> fin;
            int pos = 0;
            for (int k = 0; k < 6; ++k) {  // six bursts, 0.5-1.7 ms each, about 6.5 ms in all
                pos += rng() % 4000;
                int len = 500 + rng() % 1200;
                fin.push_back({pos, pos + len});
                pos += len;
            }
            for (int turn = 0; turn < 2; ++turn) w.push_back(Wait(fin, rng() % 16000 + turn * 16000, c, 4000, above != 0));
        }
        std::sort(w.begin(), w.end());
        double mean = 0; for (int x : w) mean += x; mean /= w.size();
        const int over = (int)(std::count_if(w.begin(), w.end(), [](int x) { return x > 1500; }));
        std::printf("%s: waits mean %.0f us, p99 %d us, max %d us, over 1.5 ms %d of %zu\n", above ? "worker above the finishing thread" : "equal priority", mean,
                    w[w.size() * 99 / 100], w.back(), over, w.size());
        if (above && (w.back() != c || over)) { std::puts("FAIL: a worker above must never wait for the finishing thread"); return 1; }
        if (!above && over == 0) { std::puts("FAIL: the model does not reproduce the equal-priority stalls"); return 1; }
    }
    std::puts("PASS: scheduling model");
    return 0;
}
