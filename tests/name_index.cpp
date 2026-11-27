// update25 host test (tools/test_name_index.sh): port/name_index.h, GXV_UPD_SNDNAME's index, against the scan it replaces
// (AudSoundNameConverter::getSoundID: the first entry at or after `start`, below n, with the hash and the name). Random tables of up to 9000
// names with forced hash collisions (few distinct hashes), the same name in several places, empty-string names, and starts anywhere; asked for
// every name with every kind of start, and for names that are not there. A negative control (PORT_NAMEIDX_NEG: chains built in descending
// order) must fail.
#include "port/name_index.h"
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

struct Ent { uint32_t hash; std::string name; };

int main() {
    std::mt19937_64 rng(777);
    long asks = 0, bad = 0, found = 0;
    for (int round = 0; round < 120; ++round) {
        const int n = round < 10 ? round : static_cast< int >(rng() % 9000);
        const uint32_t distinctHashes = (round % 3 == 0) ? 7u : (round % 3 == 1 ? 600u : 0xFFFFFFFFu);  // heavy collisions / some / none
        const int distinctNames = n > 0 ? 1 + static_cast< int >(rng() % static_cast< uint64_t >(n)) : 1;  // repeats of the same name
        std::vector< Ent > t(static_cast< size_t >(n));
        for (int i = 0; i < n; ++i) {
            const int id = static_cast< int >(rng() % static_cast< uint64_t >(distinctNames));
            t[static_cast< size_t >(i)].name = id == 0 ? "" : "SE_" + std::to_string(id) + "_name";
            // hash is a function of the name, as getHashCode is (but folded into few values for the collision rounds)
            uint32_t h = 2166136261u;
            for (char c : t[static_cast< size_t >(i)].name) h = (h ^ static_cast< uint8_t >(c)) * 16777619u;
            t[static_cast< size_t >(i)].hash = distinctHashes == 0xFFFFFFFFu ? h : h % distinctHashes;
        }
        port::NameIndex<> idx;
        auto hashOf = [&](int i) { return t[static_cast< size_t >(i)].hash; };
        const bool built = idx.Build(n, hashOf);
        if (n == 0) { if (built) { printf("empty table built\n"); return 1; } continue; }
        if (!built) { printf("not built n=%d\n", n); return 1; }
#ifdef PORT_NAMEIDX_NEG
        // descending chains: the walk then finds the LAST entry of a bucket first
        for (uint32_t b = 0; b <= idx.mask; ++b) {
            std::vector< uint16_t > chain;
            for (uint16_t k = idx.head[b]; k != port::NameIndex<>::kEnd; k = idx.next[k]) chain.push_back(k);
            for (size_t j = 0; j < chain.size(); ++j) idx.next[chain[j]] = j + 1 < chain.size() ? chain[j + 1] : port::NameIndex<>::kEnd;
            if (!chain.empty()) { idx.head[b] = chain.back(); for (size_t j = chain.size(); j-- > 1;) idx.next[chain[j]] = chain[j - 1]; idx.next[chain[0]] = port::NameIndex<>::kEnd; }
        }
#endif
        auto ask = [&](const std::string& name, uint32_t hash, int start) {
            int want = -1;
            for (int i = start; i < n; ++i) {
                if (hash == t[static_cast< size_t >(i)].hash && strcmp(name.c_str(), t[static_cast< size_t >(i)].name.c_str()) == 0) { want = i; break; }
            }
            const int got = idx.Find(hash, start, hashOf, [&](int i) { return strcmp(name.c_str(), t[static_cast< size_t >(i)].name.c_str()) == 0; });
            ++asks;
            found += want >= 0;
            if (want != got && ++bad <= 5) printf("MISMATCH round %d n %d name '%s' start %d: scan %d index %d\n", round, n, name.c_str(), start, want, got);
        };
        for (int k = 0; k < 300; ++k) {
            const Ent& e = t[rng() % static_cast< uint64_t >(n)];
            ask(e.name, e.hash, static_cast< int >(rng() % static_cast< uint64_t >(n + 1)));
            ask(e.name, e.hash, 0);
            ask(e.name, e.hash, n - 1);
            ask(e.name, e.hash, n);                      // nothing at or after the end
            ask(e.name + "x", e.hash, 0);                // right hash, wrong name
            ask("SE_" + std::to_string(rng() % 50000) + "_zz", static_cast< uint32_t >(rng()), static_cast< int >(rng() % static_cast< uint64_t >(n)));
        }
    }
    printf("asks %ld (found %ld), mismatches %ld\n", asks, found, bad);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
