// samp57: host test of port/sampler_hist.h (the histogram and the .bin writer). Usage: sampler_hist_test OUT.bin  (also writes a synthetic file tools/sampler_report.py reads).
#include "port/sampler_hist.h"
#include <assert.h>
using namespace port::sampler;
int main(int argc, char** argv) {
    static BinEntry store[1024];
    Hist h; h.Init(store, 1024);
    for (int i = 0; i < 100; ++i) h.Add(0x81791d08 + 4 * (i % 5), 0x81000100, MakeTag(0, kRun, 0));
    h.Add(0x81791d08, 0x81000100, MakeTag(1, kRun, 0));   // another slot: another key
    h.Add(0x40010077, 0, MakeTag(0, kWait, 2));
    assert(h.Used() == 7 && h.Dropped() == 0);
    uint32_t total = 0; for (uint32_t i = 0; i < 1024; ++i) total += store[i].count;
    assert(total == 102);
    for (uint32_t i = 0; i < 5000; ++i) h.Add(0x81000000 + i * 4, i, 0);   // overflow: dropped, never a crash, never a lost count
    assert(h.Used() <= 1024 - 128 && h.Dropped() > 0);
    total = 0; for (uint32_t i = 0; i < 1024; ++i) total += store[i].count;
    assert(total + h.Dropped() == 102 + 5000);
    h.Clear(); assert(h.Used() == 0);
    h.Add(0x81791d08, 0x81000100, MakeTag(0, kRun, 0));
    BinHeader hd; memset(&hd, 0, sizeof hd);
    memcpy(hd.magic, "GXVSAMP1", 8); hd.version = 1; hd.index = 1; hd.periodUs = 1000; hd.intervalMs = 1000; hd.runSamples = 1;
    FILE* f = fopen(argc > 1 ? argv[1] : "/tmp/sampler_hist_test.bin", "wb");
    assert(f && WriteBin(f, hd, h) && fclose(f) == 0);
    assert(hd.nEntries == 1);
    puts("sampler_hist_test: ok");
    return 0;
}
