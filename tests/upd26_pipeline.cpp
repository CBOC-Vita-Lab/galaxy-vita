// upd26: host test of platform/include/port/upd26.h (GXV_UPD_PREFETCH3's stage reach). The list loop below is the shape of NameObjCategoryList::execute with the
// three stages as callbacks that record which entry they were asked about. Proved: for every list length 0..40 each stage looks only at entries inside the list,
// at each entry at most once, at exactly the entries [stage distance, n-1]; the loop itself visits every entry once, in order (the stages never change it).
// A negative control drops the end test (a plain `it + k`) and must be caught as an out-of-range look.
// Build and run: tools/test_upd26.sh
#include "port/upd26.h"

#include <stdio.h>
#include <vector>

struct Obj { int id; };

static long gChecks = 0, gFail = 0;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++gChecks;                                                              \
        if (!(c)) {                                                             \
            ++gFail;                                                            \
            if (gFail < 30) printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                       \
    } while (0)

template < bool kGuard >
static Obj* const* AheadX(Obj* const* it, int k, Obj* const* end) {
    if (kGuard) return port::upd26::Ahead(it, k, end);
    return it + k;  // negative control: no end test
}

template < bool kGuard >
static void Run(int n, std::vector< int >* looks[3], std::vector< int >* visited, long* outOfRange) {
    std::vector< Obj > objs(n);
    std::vector< Obj* > list(n);
    for (int i = 0; i < n; i++) { objs[i].id = i; list[i] = &objs[i]; }
    Obj* const* begin = list.data();
    Obj* const* end = begin + n;
    const int dist[3] = {port::upd26::kStageLines, port::upd26::kStageParts, port::upd26::kStageBlocks};
    for (Obj* const* it = begin; it != end; it++) {
        for (int s = 0; s < 3; s++) {
            Obj* const* a = AheadX< kGuard >(it, dist[s], end);
            if (a) {
                if (a >= end) { ++*outOfRange; continue; }  // what the real loop would read past the list
                looks[s]->push_back((*a)->id);
            }
        }
        visited->push_back((*it)->id);
    }
}

int main() {
    for (int n = 0; n <= 40; n++) {
        std::vector< int > l0, l1, l2, vis;
        std::vector< int >* looks[3] = {&l0, &l1, &l2};
        long oor = 0;
        Run< true >(n, looks, &vis, &oor);
        CHECK(oor == 0);
        CHECK(static_cast< int >(vis.size()) == n);
        for (int i = 0; i < n; i++) CHECK(vis[i] == i);
        const int dist[3] = {port::upd26::kStageLines, port::upd26::kStageParts, port::upd26::kStageBlocks};
        for (int s = 0; s < 3; s++) {
            const int want = n > dist[s] ? n - dist[s] : 0;
            CHECK(static_cast< int >(looks[s]->size()) == want);
            for (int j = 0; j < want; j++) CHECK((*looks[s])[j] == dist[s] + j);  // each entry once, in order
        }
        // the stage distances are ordered: the lines first, then the parts, then the blocks (a stage may read what the stage before asked for)
        CHECK(dist[0] > dist[1] && dist[1] > dist[2] && dist[2] >= 1);
        if (n >= 1) {
            std::vector< int > m0, m1, m2, vis2;
            std::vector< int >* l2s[3] = {&m0, &m1, &m2};
            long oor2 = 0;
            Run< false >(n, l2s, &vis2, &oor2);
            CHECK(oor2 > 0);  // negative control: without the end test the stages do look past the list
        }
    }
    printf("upd26 pipeline test: %ld checks, %ld failed\n", gChecks, gFail);
    printf(gFail == 0 ? "PASS\n" : "FAIL\n");
    return gFail == 0 ? 0 : 1;
}
