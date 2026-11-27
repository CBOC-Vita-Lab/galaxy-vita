// GXV_PIPE25 (pipe25, 12 Nov; default 0): the late draw. The J3D walk of picture N (the passes: model by model, its draw matrices and its
// material lists, into command bytes) runs on a worker beside turn N+1's update and calcAnim, from a snapshot that turn N+1 cannot write.
//   1 = on     2 = VERIFY: the walk runs on the game thread as today AND on the worker; the two segments are compared byte for byte per
//                picture; mismatches and fence violations are counted and the first differing model is named.
// This header is the part that is independent of the game: the fence (generation stamps), the segment compare and the counters. The
// game side supplies "walk model m of picture p into a Segment" (the J3D passes) and "the models' halves" (J3DMtxBuffer's two draw
// matrix arrays, J3DDisplayListObj's two lists), which is what tests/pipe25.cpp models.
//
// CORRECTNESS ARGUMENT (the fence). Picture N reads, per model, the half H(N) of each double-buffered array. The update/calc of turn
// N+1 writes only the other half W(N+1) = 1-H(N), then swaps at the next hand-over. So a walk of picture N and a calc of turn N+1 touch
// disjoint memory as long as (a) calc N+1 never writes H(N) and (b) the next swap (hand-over N+1) does not happen before the walk of
// N is finished (the join). (a) is what every array's `gen` stamp checks: a half carries the picture generation that owns it for
// reading; any write to a half whose owner generation is a picture still in flight is a fence violation (counted, and in VERIFY it
// is also visible as a byte difference). (b) is the join: the game thread waits for the worker before the swap.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#if defined(__vita__) || defined(PORT_PIPE25_HOSTVEC)
#include "port/host_vector.h"
#endif

namespace port {
namespace pipe25 {
// pipefix26: every container that outlives a scene lives in the HOST heap on the console. With std::vector the game's operator new takes the
// storage from the scene's JKRHeap (the calling thread is a game thread); the scene's heap is destroyed, the static container keeps pointing
// into it and the helper's next write lands on the next scene's objects (host_vector.h: the same bug, sStaleDL, 6 Nov). Off the console: std::vector.
#if defined(__vita__) || defined(PORT_PIPE25_HOSTVEC)
template <class T> using HVec = PortHostVector<T>;
// (heap36: PassRec::models held std::string in a host vector: a model name longer than the string's inline buffer (15 characters) took its
// characters from the game's current heap, the scene's, at SceneFunction.cpp's push (VERIFY, the game thread) and at each copy of the record)
typedef std::basic_string<char, std::char_traits<char>, PortHostAllocator<char> > HStr;
#else
template <class T> using HVec = std::vector<T>;
typedef std::string HStr;
#endif


// What a double-buffered array of the scene looks like to the fence: two halves, the one the draw reads and the one the logic writes.
// `readGen[h]`: the picture generation (>0) that is reading half h right now (0: none). `Write(h)` is called by the logic around a
// write to half h; `BeginRead/EndRead` by the walk. A write to a half with readGen != 0 is a violation. Atomics so the checks
// themselves are race free; the checked data is plain memory (that is the point: TSan sees a real race if the fence is wrong).
struct HalfStamp {
    std::atomic<uint32_t> readGen{0};
    std::atomic<uint32_t> violations{0};
    void BeginRead(uint32_t gen) { readGen.store(gen, std::memory_order_release); }
    void EndRead() { readGen.store(0, std::memory_order_release); }
    void NoteWrite() {
        if (readGen.load(std::memory_order_acquire) != 0) violations.fetch_add(1, std::memory_order_relaxed);
    }
};

// One picture's command bytes, and which model wrote which bytes (for naming the first difference).
struct Segment {
    HVec<unsigned char> bytes;
    HVec<uint32_t> modelEnd;  // end offset of each model's bytes
    void Clear() { bytes.clear(), modelEnd.clear(); }
    void Put(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        bytes.insert(bytes.end(), c, c + n);
    }
    void EndModel() { modelEnd.push_back(static_cast<uint32_t>(bytes.size())); }
};

struct Stats {
    uint32_t pictures = 0, verified = 0, mismatches = 0, fenceViolations = 0;
    int32_t firstModel = -1;       // first differing model of the first mismatching picture
    uint32_t firstPicture = 0, firstOffset = 0;
    uint64_t bytes = 0;
};

// Compares the game thread's segment (`serial`) with the worker's (`late`); the first difference is named. Returns true when equal.
inline bool Compare(const Segment& serial, const Segment& late, Stats& st, uint32_t picture) {
    ++st.verified;
    st.bytes += serial.bytes.size();
    if (serial.bytes.size() == late.bytes.size() && (serial.bytes.empty() || !std::memcmp(serial.bytes.data(), late.bytes.data(), serial.bytes.size())))
        return true;
    size_t n = serial.bytes.size() < late.bytes.size() ? serial.bytes.size() : late.bytes.size(), at = 0;
    while (at < n && serial.bytes[at] == late.bytes[at]) ++at;
    if (!st.mismatches) {
        st.firstPicture = picture;
        st.firstOffset = static_cast<uint32_t>(at);
        int m = 0;
        while (m < static_cast<int>(serial.modelEnd.size()) && serial.modelEnd[m] <= at) ++m;
        st.firstModel = m;
    }
    ++st.mismatches;
    return false;
}

inline void Log(const Stats& st, int mode, char* line, size_t cap) {
    snprintf(line, cap, "pipe25: GXV_PIPE25=%d, %u pictures, %u verified (%llu bytes), %u mismatches%s, %u fence violations", mode, st.pictures,
             st.verified, static_cast<unsigned long long>(st.bytes), st.mismatches, "", st.fenceViolations);
    if (st.mismatches) {
        size_t o = strlen(line);
        snprintf(line + o, cap - o, " (first: picture %u, model %d, byte %u)", st.firstPicture, st.firstModel, st.firstOffset);
    }
}

}  // namespace pipe25
}  // namespace port

// =================================================================================================================== pipe26 (12 Nov)
// What pipe26 adds: the pieces the game side needs, kept free of game types so tests/pipe25.cpp can run them under ThreadSanitizer.
//   RawChain      the game thread's bytes AFTER the first deferred pass: written into chunks of their own while the passes wait as tokens,
//                 spliced into the real stream behind the passes by whoever runs them (gx_fifo.cpp: the raw FIFO).
//   MergeSerial   the GX library's data (GXData) as the serial draw would have left it, from the snapshots taken at the tokens, what each
//                 pass left, and what the game thread's open lists left (they ran without the passes' effects).
//   PassStore     VERIFY: the bytes of every pass as the game thread made them in place, to be compared with the worker's.
//   Census        the "pipe25:" line.
// WHY THE SPLICE IS EXACT (the argument the host tests check): the stream is a sequence A P1 B1 P2 B2 ... Pk Bk where A is what the
// game thread wrote before the first pass, Pi is pass i and Bi is what the open lists wrote between pass i and pass i+1 (Bk: up to the
// end of endRender). The game thread writes A into the stream, then each Bi into the chain (the passes do not run), the worker writes
// Pi into the stream and copies Bi behind it. Same order, same bytes if (a) every Pi is the same bytes whichever thread runs it when
// the GX library state (GXData and the port's shadows) at its start is the serial one (snapshot at the token, shadows dropped), and
// (b) every Bi is the same bytes with or without Pi having run before it (only shadows could tell: they are dropped at the token, so
// Bi resends what it needs, a superset of the serial bytes with the same effect on the front end's registers).
#include <algorithm>
#include <cstdlib>
#include <string>

namespace port {
namespace pipe25 {

class RawChain {
public:
    static constexpr uint32_t kSlack = 64;  // (the FIFO's limit leaves this much behind it, as the staging buffer does)
    struct Chunk {
        uint8_t* mem = nullptr;
        uint32_t cap = 0;       // usable bytes (mem has cap + kSlack)
        uint32_t used = 0;      // bytes written when the chunk was left
        uint64_t base = 0;      // logical offset of mem[0]
    };
    uint32_t chunkBytes = 256u << 10;
    ~RawChain() {
        for (Chunk& c : mChunks) std::free(c.mem);
    }
    // A picture's chain begins. Returns where the FIFO writes; Limit() is where it must call Roll.
    uint8_t* Open() {
        mCount = 0, mTotal = 0, mOpen = true;
        mMarks.clear();
        return NewChunk() ? mChunks[0].mem : nullptr;
    }
    bool IsOpen() const { return mOpen; }
    uint8_t* Base() const { return mChunks[mCount - 1].mem; }
    uint8_t* Limit() const { return mChunks[mCount - 1].mem + mChunks[mCount - 1].cap - kSlack; }
    uint8_t* End() const { return mChunks[mCount - 1].mem + mChunks[mCount - 1].cap + kSlack; }  // (the memory's end, for range tests)
    bool Owns(const uint8_t* p) const { return mOpen && p >= mChunks[mCount - 1].mem && p < End(); }
    // The FIFO is at `ptr` (>= Limit): this chunk is full, writing goes on in the next. nullptr: no memory.
    uint8_t* Roll(uint8_t* ptr) {
        Chunk& c = mChunks[mCount - 1];
        c.used = static_cast<uint32_t>(ptr - c.mem);
        mTotal = c.base + c.used;
        if (!NewChunk()) return nullptr;
        return mChunks[mCount - 1].mem;
    }
    // A pass waits here: the bytes that follow are its Bi.
    void Mark(const uint8_t* ptr) { mMarks.push_back(Logical(ptr)); }
    uint32_t Marks() const { return static_cast<uint32_t>(mMarks.size()); }
    void Close(const uint8_t* ptr) {
        mTotal = Logical(ptr);
        Chunk& c = mChunks[mCount - 1];
        c.used = static_cast<uint32_t>(ptr - c.mem);
        mOpen = false;
    }
    uint64_t Total() const { return mTotal; }
    uint32_t Chunks() const { return mCount; }
    uint64_t HighWater() const { return mHigh; }
    // Bi: the bytes after mark i up to mark i+1 (or the end). `put(ptr, n)` receives them in pieces, in order.
    template <class Put> void CopyAfter(uint32_t i, Put&& put) const {
        const uint64_t from = mMarks[i], to = i + 1 < mMarks.size() ? mMarks[i + 1] : mTotal;
        for (uint32_t k = 0; k < mCount && from < to; ++k) {
            const Chunk& c = mChunks[k];
            const uint64_t lo = std::max<uint64_t>(from, c.base), hi = std::min<uint64_t>(to, c.base + c.used);
            if (lo < hi) put(c.mem + (lo - c.base), static_cast<size_t>(hi - lo));
        }
    }
    uint64_t SizeAfter(uint32_t i) const { return (i + 1 < mMarks.size() ? mMarks[i + 1] : mTotal) - mMarks[i]; }
    void Release() { mCount = 0, mOpen = false, mMarks.clear(); }  // (the chunks are kept for the next picture)

private:
    uint64_t Logical(const uint8_t* p) const {
        const Chunk& c = mChunks[mCount - 1];
        return c.base + static_cast<uint64_t>(p - c.mem);
    }
    bool NewChunk() {
        const uint64_t base = mCount ? mChunks[mCount - 1].base + mChunks[mCount - 1].used : 0;
        if (mCount == mChunks.size()) {
            Chunk c;
            c.mem = static_cast<uint8_t*>(std::malloc(chunkBytes + kSlack));
            if (!c.mem) return false;
            c.cap = chunkBytes;
            mChunks.push_back(c);
        }
        mChunks[mCount].base = base;
        mChunks[mCount].used = 0;
        ++mCount;
        mHigh = std::max<uint64_t>(mHigh, base);
        return true;
    }
    HVec<Chunk> mChunks;
    HVec<uint64_t> mMarks;
    uint32_t mCount = 0;
    uint64_t mTotal = 0, mHigh = 0;
    bool mOpen = false;
};

// GXData (or any state of 32-bit words) as the serial draw leaves it. snap[i]: the state at token i, as the game thread had it after
// flushing the state the pass starts from; end[i]: the state the worker's pass i left; `final`: the game thread's state after the
// open lists and endRender, which ran without the passes. In: final. Out: final, with the passes' writes put in where the open lists
// after them did not write the word (they run after the pass in the serial order and win). Returns the words taken from passes.
// (pipe27) the same over any two sequences of word buffers, `n` of them: S[i] / E[i] have data(). The game's vectors are pools that
// keep their storage from picture to picture, so they are longer than n.
template <class S, class E> inline uint32_t MergeSerialN(uint32_t* final, const S& snap, const E& end, size_t words, size_t n) {
    if (n == 0) return 0;
    HVec<uint32_t> cur(snap[0].data(), snap[0].data() + words);
    for (size_t i = 0; i < n; ++i) {
        const uint32_t* s0 = snap[i].data();
        const uint32_t* e0 = end[i].data();
        for (size_t w = 0; w < words; ++w)
            if (e0[w] != s0[w]) cur[w] = e0[w];  // pass i writes the word
        const uint32_t* next = i + 1 < n ? snap[i + 1].data() : final;
        for (size_t w = 0; w < words; ++w)
            if (next[w] != s0[w]) cur[w] = next[w];  // the open lists after pass i write it (they win)
    }
    uint32_t taken = 0;
    for (size_t w = 0; w < words; ++w) {
        if (cur[w] != final[w]) ++taken;
        final[w] = cur[w];
    }
    return taken;
}
inline uint32_t MergeSerial(uint32_t* final, const HVec<HVec<uint32_t>>& snap, const HVec<HVec<uint32_t>>& end, size_t words) {
    if (snap.empty() || end.size() != snap.size()) return 0;
    return MergeSerialN(final, snap, end, words, snap.size());
}

// VERIFY: one pass as the game thread made it in place (bytes) and as the worker made it later.
struct PassRec {
    uint32_t id = 0;
    int32_t type = 0;
    bool xlu = false, comparable = true;
    uint32_t offset = 0, length = 0;                 // in PassStore::bytes
    HVec<std::pair<uint32_t, HStr>> models;  // (offset in the pass, model name) at each model's start
};
struct PassStore {
    HVec<uint8_t> bytes;
    HVec<PassRec> passes;
    void Clear() { bytes.clear(), passes.clear(); }
};
struct VerifyStats {
    uint64_t pictures = 0, passes = 0, compared = 0, skipped = 0, bytes = 0, mismatches = 0, fenceViolations = 0;
    bool logged = false;
    uint32_t lastAt = 0;  // (pipefix31) the offset of the first differing byte of the pass compared last
    char first[320] = {};
    // revB: the GXData a pass leaves, in place and as the helper's re-run leaves it (the bytes of a pass say nothing about the library state it leaves for the passes and lists after
    // it). passesState: re-runs whose end state was compared; stateDiffer: how many differed; stateFirst: the first (pass, word index into GXData).
    uint64_t passesState = 0, stateDiffer = 0;
    char stateFirst[120] = {};
};
// revB: compares the library's data after a pass in place with after its re-run (both `words` long). Words that hold the stream's own position are what a re-run into the scratch buffer
// would show different; the first differing word's index is logged so the field can be named from GXData's layout (a known one can then be masked).
inline bool CompareGxEnd(const uint32_t* inPlace, const uint32_t* rerun, size_t words, VerifyStats& st, uint32_t picture, uint32_t pass) {
    ++st.passesState;
    size_t at = 0;
    while (at < words && inPlace[at] == rerun[at]) ++at;
    if (at == words) return true;
    if (!st.stateDiffer) snprintf(st.stateFirst, sizeof st.stateFirst, "picture %u pass %u word %u of %u (in place 0x%08X, re-run 0x%08X)", picture, pass, static_cast<unsigned>(at),
                                  static_cast<unsigned>(words), inPlace[at], rerun[at]);
    ++st.stateDiffer;
    return false;
}
// Compares one pass; the first mismatch of the session is named: picture, pass, the model whose bytes differ first.
inline bool ComparePass(const PassRec& rec, const uint8_t* serial, const uint8_t* late, uint32_t lateLen, VerifyStats& st, uint32_t picture) {
    ++st.passes;
    if (!rec.comparable) { ++st.skipped; return true; }
    ++st.compared;
    st.bytes += rec.length;
    if (rec.length == lateLen && (rec.length == 0 || !std::memcmp(serial, late, rec.length))) return true;
    uint32_t at = 0;
    const uint32_t n = std::min(rec.length, lateLen);
    while (at < n && serial[at] == late[at]) ++at;
    if (!st.mismatches) {
        const char* model = "?";
        for (const auto& m : rec.models) {
            if (m.first > at) break;
            model = m.second.c_str();
        }
        snprintf(st.first, sizeof st.first, "first mismatch: picture %u, pass %u (buffer type 0x%X, %s), byte %u of %u (worker made %u), model '%s'", picture, rec.id,
                 static_cast<unsigned>(rec.type), rec.xlu ? "translucent" : "opaque", at, rec.length, lateLen, model);
    }
    st.lastAt = at;
    ++st.mismatches;
    return false;
}

// The census line (every window): everything the console run is read for. Times are totals over the window; the caller divides.
struct Census {
    uint32_t pictures = 0, deferred = 0, tokens = 0, serial = 0, abandoned = 0, early = 0, taken = 0, waited = 0, takenOver = 0;
    uint64_t chainBytes = 0, chainHigh = 0, tokenNs = 0, jobNs = 0, passNs = 0, spliceNs = 0, waitNs = 0, gameInPassNs = 0;
    uint32_t mergeWords = 0, abandonFail = 0, noRoom = 0;
};
inline void LogCensus(const Census& c, int mode, const VerifyStats& v, char* line, size_t cap) {
    const double per = c.deferred ? 1.0 / c.deferred : 0.0;
    int o = snprintf(line, cap,
                     "pipe25: GXV_PIPE25=%d, %u drawn turns: %u late (%.1f passes deferred a turn, %.1f KiB of open-list bytes through the chain a turn, longest chain %u KiB), %u serial, "
                     "%u abandoned mid-draw (%u could not), %u joined early by the logic, %u taken back unstarted; game thread: tokens %.3f ms a turn, join wait %.3f ms (%u waited); "
                     "worker: job %.3f ms a turn (the passes %.3f of it, the splice %.3f); merged %u GXData words",
                     mode, c.pictures, c.deferred, c.tokens * per, c.chainBytes * per / 1024.0, static_cast<unsigned>(c.chainHigh / 1024), c.serial, c.abandoned, c.abandonFail,
                     c.early, c.taken, c.tokenNs * per / 1e6, c.waitNs * per / 1e6, c.waited, c.jobNs * per / 1e6, c.passNs * per / 1e6, c.spliceNs * per / 1e6, c.mergeWords);
    if (mode == 2 && o < static_cast<int>(cap) - 200) {
        o += snprintf(line + o, cap - o, "; VERIFY: %llu passes, %llu compared (%llu bytes), %llu left out (crossed a segment), %llu MISMATCHES%s%s",
                 static_cast<unsigned long long>(v.passes), static_cast<unsigned long long>(v.compared), static_cast<unsigned long long>(v.bytes),
                 static_cast<unsigned long long>(v.skipped), static_cast<unsigned long long>(v.mismatches), v.mismatches ? "; " : "", v.mismatches ? v.first : "");
        if (o < static_cast<int>(cap) - 160)
            snprintf(line + o, cap - o, "; VERIFY end state: %llu re-runs compared, %llu left GXData otherwise than the pass in place%s%s%s", static_cast<unsigned long long>(v.passesState),
                     static_cast<unsigned long long>(v.stateDiffer), v.stateDiffer ? " (first: " : "", v.stateDiffer ? v.stateFirst : "", v.stateDiffer ? ")" : "");
    }
}

}  // namespace pipe25
}  // namespace port
