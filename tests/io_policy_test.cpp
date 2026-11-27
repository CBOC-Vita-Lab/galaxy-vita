// disc26: the pure policies of port/io_policy.h: read-ahead windows, prefetch budget, read pieces, interleave rule, verdict.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../platform/include/port/io_policy.h"

using namespace port::iopol;
static void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}

int main() {
    // --- read-ahead windows: two sequences that alternate keep a window each
    {
        RaTable<4> t;
        require(t.Find(7, 0, 100) < 0, "empty table finds nothing");
        require(!t.NoteRead(7, 0, 100), "first read of a file is not a sequence");
        require(!t.NoteRead(9, 0, 50), "first read of another file is not a sequence");
        require(t.NoteRead(7, 100, 100), "a read starting where the file's last one ended is a sequence (the other file read in between)");
        require(!t.NoteRead(7, 500, 100), "a jump is not a sequence");
        require(t.NoteRead(9, 50, 50), "the other file's sequence survived");
        const int a = t.SlotFor(7);
        t.Set(a, 7, 200, 100);
        const int b = t.SlotFor(9);
        require(b != a, "a second file gets its own slot");
        t.Set(b, 9, 100, 50);
        require(t.Find(7, 200, 100) == a && t.Find(9, 100, 50) == b, "both windows are found");
        require(t.Find(7, 200, 99) < 0 && t.Find(7, 201, 100) < 0, "only the exact piece matches");
        require(t.SlotFor(7) == a, "a file refills its own slot");
        // LRU: fill all four, touch 1, a fifth file takes the oldest
        t.Set(t.SlotFor(11), 11, 0, 8);
        t.Set(t.SlotFor(12), 12, 0, 8);
        require(t.Find(7, 200, 100) == a, "touch 7");
        t.Set(t.SlotFor(13), 13, 0, 8);
        require(t.Find(9, 100, 50) < 0, "the least recently used window (9) went");
        require(t.Find(7, 200, 100) >= 0 && t.Find(13, 0, 8) >= 0, "the recently used ones stayed");
        t.ClearAll();
        require(t.Find(7, 200, 100) < 0 && !t.NoteRead(7, 300, 1), "ClearAll forgets windows and marks");
    }
    // --- prefetch budget
    {
        PfBudget<4> b;
        uint32_t dropped[4];
        int nd = 0;
        require(b.Admit(1, 400, 1000, dropped, &nd) >= 0 && nd == 0 && b.used == 400, "first file admitted");
        require(b.Admit(2, 400, 1000, dropped, &nd) >= 0 && b.used == 800, "second file admitted");
        require(b.Admit(1, 400, 1000, dropped, &nd) < 0, "a file already held is not admitted twice");
        b.Touch(b.Find(1));
        require(b.Admit(3, 400, 1000, dropped, &nd) >= 0 && nd == 1 && dropped[0] == 2 && b.used == 800, "the least recently used file (2) made room");
        require(b.Find(2) < 0 && b.Find(1) >= 0 && b.Find(3) >= 0, "who is held");
        require(b.Admit(4, 1001, 1000, dropped, &nd) < 0 && b.used == 800 && nd == 0, "a file over the cap is refused and nothing is dropped for it");
        require(b.Admit(5, 0, 1000, dropped, &nd) < 0, "an empty file is not admitted");
        require(b.Admit(6, 1000, 1000, dropped, &nd) >= 0 && nd == 2 && b.used == 1000, "a file as big as the cap empties the rest");
        b.Drop(b.Find(6));
        require(b.used == 0, "drop");
        // the item table is full before the byte cap is
        PfBudget<2> s;
        require(s.Admit(1, 1, 100, dropped, &nd) >= 0 && s.Admit(2, 1, 100, dropped, &nd) >= 0, "two small files");
        require(s.Admit(3, 1, 100, dropped, &nd) >= 0 && nd == 1 && dropped[0] == 1, "a third evicts the oldest although bytes are free");
    }
    // --- pieces
    {
        require(NextPiece(100, 0) == 100 && NextPiece(100, 100) == 100 && NextPiece(101, 100) == 100 && NextPiece(60, 100) == 60, "piece sizes");
        size_t left = 1000, total = 0, pieces = 0;
        while (left) { const size_t n = NextPiece(left, 300); left -= n; total += n; ++pieces; }
        require(total == 1000 && pieces == 4, "pieces add up");
        require(MayInterleave(1, 2) && MayInterleave(0, 3) && !MayInterleave(2, 2) && !MayInterleave(1, 1) && !MayInterleave(2, 3),
                "only priority 1 and below goes in front of an archive read");
    }
    // --- verdict
    {
        require(std::string(Verdict(0, 0, 16, 0, 0)) == "CLEAN", "clean");
        require(std::string(Verdict(0, 0, 16, 0, 3)).find("below 50") != std::string::npos, "slow reads below 50");
        require(std::string(Verdict(0, 0, 16, 2, 3)).find("SLOW") != std::string::npos, "slow reads");
        require(std::string(Verdict(0, 0, 300, 0, 0)).find("AUDIO") != std::string::npos, "audio at risk");
        require(std::string(Verdict(60, 20, 16, 0, 0)).find("HITCH") != std::string::npos, "hitch by total");
        require(std::string(Verdict(10, 16, 16, 0, 0)).find("HITCH") != std::string::npos, "hitch by one wait");
        require(std::string(Verdict(0, 15.9, 16, 0, 0)) == "CLEAN", "a wait under a frame is clean");
    }
    std::puts("PASS: io_policy (read-ahead windows, prefetch budget, pieces, interleave rule, verdict)");
}
