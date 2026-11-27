#include <util/long_present_trace.h>
#include <cassert>

struct FakeClock {
    using time_point = std::chrono::time_point<FakeClock, std::chrono::microseconds>;
    static inline long long t = 0;
    static inline unsigned calls = 0;
    static time_point now() { ++calls; return time_point(std::chrono::microseconds(t)); }
};

int main(int argc, char **argv) {
    assert(argc == 2);
    bool on = std::string_view(argv[1]) == "on";
    int operations = 0;
    {
        long_present::Batch<FakeClock> trace;
        auto start = trace.start();
        auto ret = trace.call("quiet", [&] { ++operations; FakeClock::t += 50000; return -7; });
        assert(ret == -7);
        trace.call("slow_void", [&] { ++operations; FakeClock::t += 50001; });
        auto other = trace.call("slow_value", [&] { ++operations; FakeClock::t += 60000; return 2; });
        assert(other == 2);
        trace.returned("entry", start, 0);
        assert(test_log::lines.size() == (on ? 1u : 0u)); // no nested I/O
    }
    assert(operations == 3);
    if (!on) {
        assert(FakeClock::calls == 0 && test_log::lines.empty());
    } else {
        assert(test_log::lines.size() == 4);
        assert(test_log::lines[1].find("result void slow_void 50001 60000") != std::string::npos);
        assert(test_log::lines[2].find("slow_value 60000 0 2") != std::string::npos);
        assert(test_log::lines[3].find("entry 160001 0 0") != std::string::npos);
    }
}
