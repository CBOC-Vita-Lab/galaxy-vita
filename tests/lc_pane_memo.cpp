// GXV_LC_PANEMEMO host proof (logiccut, 6 Nov): port/pane_memo.h used as nw4r::lyt::Pane::FindPaneByName uses it
// (decomp/src/nw4r/lyt/lyt_pane.cpp), on random pane trees that are renamed, re-linked, destroyed and rebuilt at the same
// addresses between lookups (each change bumps, as SetName / InsertChild / RemoveChild / ~Pane do). Every memo answer must equal
// a fresh search. A negative control changes names without the bump and must be caught.
// Build and run: bash tools/test_lc_pane_memo.sh
#include "port/pane_memo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <random>
#include <string>
#include <vector>

static std::mt19937 rng(4242);
static uint32_t R() { return rng(); }

struct Pane;
static port::panememo::Memo<Pane> sMemo;
static bool sBumpOnChange = true;
static void Changed() {
    if (sBumpOnChange) sMemo.Bump();
}

// detail::EqualsResName (lyt_common.cpp, TARGET_PC)
static bool EqualsResName(const char* a1, const char* a2) {
    if (a1[0] != a2[0]) return false;
    return a1[0] == '\0' || std::strncmp(a1 + 1, a2 + 1, 0x10 - 1) == 0;
}

struct Pane {
    char mName[17];
    Pane* mpParent = nullptr;
    std::vector<Pane*> mChildList;
    explicit Pane(const char* name) { SetName(name); }
    ~Pane() { Changed(); }
    void SetName(const char* p) {
        Changed();
        std::strncpy(mName, p, 0x10);
        mName[0x10] = '\0';
    }
    void InsertChild(size_t at, Pane* c) {
        Changed();
        mChildList.insert(mChildList.begin() + (long)std::min(at, mChildList.size()), c);
        c->mpParent = this;
    }
    void RemoveChild(Pane* c) {
        Changed();
        mChildList.erase(std::find(mChildList.begin(), mChildList.end(), c));
        c->mpParent = nullptr;
    }
    // the original search
    Pane* Search(const char* pName, bool recursive) {
        if (EqualsResName(mName, pName)) return this;
        if (recursive)
            for (Pane* c : mChildList)
                if (Pane* p = c->Search(pName, true)) return p;
        return nullptr;
    }
    // FindPaneByName with GXV_LC_PANEMEMO=1
    Pane* FindPaneByName(const char* pName, bool recursive) {
        port::panememo::Key key;
        port::panememo::MakeKey(&key, this, recursive, pName);
        const uint32_t hash = port::panememo::Hash(key);
        Pane* found;
        if (!sMemo.Find(key, hash, &found)) {
            found = Search(pName, recursive);
            sMemo.Store(key, hash, found);
        }
        return found;
    }
};

static const char* kNames[] = {"N_Root", "N_Icon", "N_Icon00", "N_Icon01", "T_Count", "P_Base", "P_Frame", "W_Window", "", "a",
                               "ABCDEFGHIJKLMNOP", "ABCDEFGHIJKLMNOPQ", "ABCDEFGHIJKLMNOPZ", "ABCDEFGHIJKLMNO", "DomeIcon", "DomeIconA"};
static const char* Name() { return kNames[R() % (sizeof kNames / sizeof kNames[0])]; }

struct World {
    // panes live in fixed slots, so a destroyed pane's address is reused by the next one built there
    alignas(Pane) unsigned char slots[64][sizeof(Pane)];
    bool live[64] = {};
    Pane* At(int i) { return reinterpret_cast<Pane*>(slots[i]); }
    void Build(int i) {
        new (slots[i]) Pane(Name());
        live[i] = true;
    }
    void Destroy(int i) {  // unlinks it and its children first (Layout frees whole sub-trees the same way)
        Pane* p = At(i);
        if (p->mpParent) p->mpParent->RemoveChild(p);
        while (!p->mChildList.empty()) p->RemoveChild(p->mChildList.back());
        p->~Pane();
        live[i] = false;
    }
    bool IsAncestor(Pane* a, Pane* b) {  // a is b or above b
        for (; b; b = b->mpParent)
            if (a == b) return true;
        return false;
    }
};

static long Run(int rounds, bool bump, long* lookups, long* hitsOut) {
    sBumpOnChange = bump;
    sMemo = port::panememo::Memo<Pane>();
    World w;
    for (int i = 0; i < 64; i++) w.Build(i);
    for (int i = 1; i < 64; i++) w.At((int)(R() % (unsigned)i))->InsertChild(R() % 4, w.At(i));
    long bad = 0;
    for (int r = 0; r < rounds; r++) {
        const int op = (int)(R() % 100);
        const int i = (int)(R() % 64);
        if (op < 3 && w.live[i]) {
            w.At(i)->SetName(Name());
        } else if (op < 5 && w.live[i] && w.At(i)->mpParent) {
            w.At(i)->mpParent->RemoveChild(w.At(i));
        } else if (op < 7 && w.live[i] && !w.At(i)->mpParent) {
            const int j = (int)(R() % 64);
            if (w.live[j] && !w.IsAncestor(w.At(i), w.At(j))) w.At(j)->InsertChild(R() % 5, w.At(i));
        } else if (op < 8 && w.live[i]) {
            w.Destroy(i);
        } else if (op < 9 && !w.live[i]) {
            w.Build(i);
            const int j = (int)(R() % 64);
            if (w.live[j] && j != i) w.At(j)->InsertChild(R() % 5, w.At(i));
        } else if (op < 10 && !bump && w.live[i]) {
            std::strncpy(w.At(i)->mName, Name(), 16);  // (negative control only: a name written behind the memo's back)
        } else {
            int root = (int)(R() % 64);
            if (!w.live[root]) continue;
            for (int q = 0; q < 8; q++) {
                std::string n = Name();
                if (R() % 4 == 0) n += "XYZ";  // longer than 16 characters, or differs after them
                const bool rec = (R() % 8) != 0;
                Pane* got = w.At(root)->FindPaneByName(n.c_str(), rec);
                Pane* want = w.At(root)->Search(n.c_str(), rec);
                ++*lookups;
                if (got != want) bad++;
            }
        }
    }
    *hitsOut = (long)sMemo.hits;
    return bad;
}

int main() {
    long lookups = 0, hits = 0;
    const long bad = Run(400000, true, &lookups, &hits);
    std::printf("memo: %ld lookups, %ld memo hits, %ld mismatches\n", lookups, hits, bad);
    long l2 = 0, h2 = 0;
    const long control = Run(400000, false, &l2, &h2);
    std::printf("negative control (changes without the bump): %ld lookups, %ld mismatches (must be > 0)\n", l2, control);
    const bool ok = bad == 0 && control > 0 && hits > lookups / 20;  // (a change retires every entry: hits come between changes)
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
