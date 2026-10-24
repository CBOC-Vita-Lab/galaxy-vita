// Use the real variant-set type, including its actual target-library hasher.
#include "../platform/vgx/src/vgx_draw.cpp"
#include <set>

namespace {
using ProductionSet = decltype(vgx::g_varSeen);
uint64_t comparisons = 0;
struct CountEqual {
    bool operator()(uint64_t a, uint64_t b) const { ++comparisons; return a == b; }
};
void Require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
uint64_t Key(unsigned pair, unsigned group) {
    // 3000 distinct VS/FS pairs; 80 percent share the common layout/blend bits.
    const unsigned vs = pair / 64, fs = pair % 64;
    return (static_cast<uint64_t>((vs << 16) | fs) << 32) |
           (static_cast<uint64_t>(group) << 12) | 1u;
}
}

int main(int argc, char** argv) {
    const bool strict = argc > 1 && std::strcmp(argv[1], "--strict") == 0;
    if (strict) Require(sizeof(size_t) == 4, "strict distribution test must execute a real ILP32 binary");
    ProductionSet production;
    std::unordered_set<uint64_t, ProductionSet::hasher, CountEqual> counted;
    std::vector<uint64_t> keys;
    for (unsigned i = 0; i != 3000; ++i) {
        const unsigned group = i < 2400 ? 0 : 1 + i % 20;
        const uint64_t key = Key(i, group);
        keys.push_back(key);
        Require(production.insert(key).second && counted.insert(key).second, "shader-ID-only changes remain distinct variants");
    }
    size_t largest = 0, nonempty = 0;
    for (size_t i = 0; i != production.bucket_count(); ++i) {
        const size_t size = production.bucket_size(i);
        largest = std::max(largest, size);
        nonempty += size != 0;
    }
    comparisons = 0;
    for (uint64_t key : keys) Require(counted.count(key) == 1, "every learned variant remains findable");
    std::printf("variant-hash: size_t=%zu keys=%zu buckets=%zu nonempty=%zu largest-chain=%zu lookup-equality-comparisons=%llu\n",
                sizeof(size_t), production.size(), production.bucket_count(), nonempty, largest,
                static_cast<unsigned long long>(comparisons));
    if (sizeof(size_t) == 4) {
        if (strict) {
            Require(largest <= 16, "ILP32 variant hash must retain shader IDs instead of collapsing their common layout/blend bucket");
            Require(comparisons < keys.size() * 8, "known-variant lookups must avoid long equal-hash chains");
        }
    } else std::puts("SKIP: ILP32 distribution verdict on this 64-bit executable; membership still checked");

    std::set<uint64_t> reference(keys.begin(), keys.end());
    uint64_t random = 0x0123456789abcdefull;
    for (unsigned i = 0; i != 100000; ++i) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        Require(production.insert(random).second == reference.insert(random).second, "membership agrees with ordered-set reference");
        Require(production.count(random) == reference.count(random), "random membership preserved");
        Require(production.count(random ^ 0x100000000ull) == reference.count(random ^ 0x100000000ull),
                "high-word differences never alias in equality");
    }
    std::vector<uint64_t> ordered(production.begin(), production.end());
    std::sort(ordered.begin(), ordered.end());
    Require(std::equal(ordered.begin(), ordered.end(), reference.begin(), reference.end()),
            "sorted recipe rewrite order remains unchanged despite new bucket order");
    std::puts(strict ? "PASS: real variant-set membership and ILP32 hash distribution; not Vita FPS" :
                       "PASS: real variant-set membership; distribution characterized above, not a Vita FPS measurement");
}
