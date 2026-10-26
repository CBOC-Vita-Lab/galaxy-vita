#include "port/settings_lifecycle.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

using namespace port::settings;
struct Fixture {
    int fail = -1;
    std::vector<int> events;
    static bool Acquire(void* p, Resource r, unsigned timeout) {
        auto& f = *static_cast<Fixture*>(p);
        assert(timeout == 250);
        f.events.push_back(static_cast<int>(r));
        return f.fail != static_cast<int>(r);
    }
    static void Release(void* p, Resource r) {
        static_cast<Fixture*>(p)->events.push_back(100 + static_cast<int>(r));
    }
    Hooks hooks() { return {this, Acquire, Release}; }
};
int main() {
    constexpr int count = static_cast<int>(Resource::Count);
    for (int fail = -1; fail < count; ++fail) {
        Fixture f;
        f.fail = fail;
        auto hooks = f.hooks();
        Lease lease;
        assert(lease.Acquire(hooks, 250) == (fail < 0));
        assert(lease.Active() == (fail < 0));
        lease.Release(hooks);
        const auto events = f.events;
        lease.Release(hooks);
        assert(events == f.events);
        const int acquired = fail < 0 ? count : fail;
        int at = 0;
        for (int i = 0; i < acquired; ++i) assert(events[at++] == i);
        if (fail >= 0) assert(events[at++] == fail);
        for (int i = acquired - 1; i >= 0; --i) assert(events[at++] == 100 + i);
        assert(at == static_cast<int>(events.size()));
    }
    HoldGate gate;
    assert(gate.Requested() == 0);
    for (unsigned i = 0; i != 10000; ++i) {
        const unsigned old = gate.Begin();
        assert(old && gate.Requested() == old);
        gate.Cancel(old);
        const unsigned current = gate.Begin();
        std::thread late([&] { assert(!gate.Acknowledge(old)); gate.Cancel(old); });
        assert(gate.Acknowledge(current));
        late.join();
        assert(gate.Acknowledged(current));
        gate.Cancel(current);
        assert(!gate.Acknowledged(current));
        assert(!gate.Acknowledge(current));
    }
    std::puts("PASS: settings lease unwind at every step; 10000 stale-generation races");
}
