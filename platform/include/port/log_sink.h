// The Vita log's in-memory sink (platform/src/os/os_alarm.cpp): two fixed buffers. Lines are appended to the FILL buffer under the log lock;
// the writer takes the batch by SWAPPING the two buffers under that lock (a pointer exchange, no copy) and writes the taken one to the card
// outside the lock. Before (strikers2audit, finding 2) the writer copied the whole fill buffer (up to 96 KiB) under the lock, at the lowest
// priority: a writer preempted in that copy held the lock while the game thread logged. The caller holds the lock for every call here, and
// only one writer exists at a time (a second lock in os_alarm.cpp), so the taken buffer is free again when it is next swapped in.
// No heap use: the log is called from everywhere, including while the game's allocator is busy.
#pragma once
#include <stddef.h>
#include <string.h>
#include <string>
#include <atomic>

namespace port {
class LogSwap {
public:
    LogSwap(char* a, char* b, size_t capacity) : fill_(a), spare_(b), cap_(capacity) {}
    // The line goes to the fill buffer; false (nothing stored) when it does not fit.
    bool Append(const char* line, size_t len) {
        if (len_ + len > cap_) return false;
        memcpy(fill_ + len_, line, len);
        len_ += len;
        return true;
    }
    // The gathered batch: *data points at it, the return is its length (0: nothing). The fill buffer is now the other one, empty.
    size_t Take(const char** data) {
        const size_t n = len_;
        *data = fill_;
        char* const t = fill_;
        fill_ = spare_;
        spare_ = t;
        len_ = 0;
        return n;
    }
    size_t Length() const { return len_; }
    size_t Capacity() const { return cap_; }

private:
    char* fill_;
    char* spare_;
    size_t cap_;
    size_t len_ = 0;
};

// fable31fix S1: the display-queue callback thread (4 KiB system stack, tools/check_display_stack.py) must never do a card write, or any call that can block:
// the pending buffer being full, or a FATAL line, used to make logToFileBuf write to the memory card from there. The rule: on that thread a full buffer DROPS the
// line and counts it (OnFull returns false); on every other thread OnFull returns true (the caller writes the batch now, as before). A line with FATAL is handed to
// the writer thread by the same pointer swap as any line (the urgent flag wakes it within 10 ms) and is never written on the callback thread. The next ordinary line
// on another thread reports the count once: "log: N display-thread lines dropped (buffer full)".
class DisplayDropRule {
public:
    // The pending buffer is full. True: the caller may write the batch to the card now. False: it is the display thread: the line is dropped and counted.
    bool OnFull(bool onDisplayThread) {
        if (!onDisplayThread) return true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // May a FATAL line force a card write on this thread? Never on the display thread (it only raises the urgent flag for the writer thread).
    static bool MayWriteNow(bool onDisplayThread) { return !onDisplayThread; }
    // The count of lines dropped since the last call, reset to 0 (so each is reported once).
    unsigned Take() { return dropped_.exchange(0, std::memory_order_relaxed); }
    // Give the count back (the report line did not fit either).
    void Restore(unsigned n) { dropped_.fetch_add(n, std::memory_order_relaxed); }
    unsigned Pending() const { return dropped_.load(std::memory_order_relaxed); }

private:
    std::atomic<unsigned> dropped_{0};
};

// fmt31 / fix31: a formatted line longer than the log's line buffer is written as consecutive file lines of at most `chunk` bytes of text, cut at the
// last space past chunk / 2 where there is one (that space is dropped); every line after the first starts with "+ ". fix31: the line is ONE unit:
// (1) the periodic-line filter is asked once, for the whole text (it matches by prefix: asked per piece it dropped the first piece of a long periodic line
// and kept its "+ " pieces), and (2) the pieces are made into one block that the caller appends with ONE Append under the log lock, so another
// thread's line cannot land between a line and its continuation. (This path is the only one that uses the heap: a line that long is rare.)
template <class Piece>
inline void SplitLong(const char* text, size_t len, size_t chunk, Piece&& piece) {
    size_t pos = 0;
    bool first = true;
    while (pos < len) {
        size_t take = len - pos;
        if (take > chunk) {
            take = chunk;
            for (size_t k = take; k > chunk / 2; --k)
                if (text[pos + k] == ' ') { take = k; break; }
        }
        piece(text + pos, take, first);
        pos += take;
        if (pos < len && text[pos] == ' ') ++pos;
        first = false;
    }
}
// The block the file gets for one long line: per piece `prefix` (the time stamp), "+ " after the first, the piece, "\n". Empty when the line is dropped:
// `dropped(text, len)` is the periodic-line filter, asked once for the whole line and never for a line with FATAL in it (FATAL is always written).
template <class Dropped>
inline std::string LongLineBlock(const char* prefix, size_t prefixLen, const char* text, size_t len, size_t chunk, Dropped&& dropped, bool* urgent = nullptr) {
    const bool fatal = std::string(text, len).find("FATAL") != std::string::npos;
    if (urgent) *urgent = fatal;
    std::string block;
    if (!fatal && dropped(text, len)) return block;
    block.reserve(len + (len / (chunk / 2) + 1) * (prefixLen + 3));
    SplitLong(text, len, chunk, [&](const char* p, size_t n, bool first) {
        block.append(prefix, prefixLen);
        if (!first) block.append("+ ", 2);
        block.append(p, n);
        block.push_back('\n');
    });
    return block;
}
}  // namespace port
