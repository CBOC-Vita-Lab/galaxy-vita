#pragma once

// Start-up watchdog decisions, kept free of console calls so that a host test can compile this file as it is.
// The loading screen (vgx_host_galaxy_vita.cpp, namespace splash) feeds these functions the numbers it already
// has and acts on the answers. Nothing here waits, sleeps or allocates.
//
//   StallWatch         no sign of life for a time: the start-up counters, the content the renderer has seen and
//                      the bytes the disc layer has read all stood still
//   TitleFallbackDue   the game thread keeps running pictures but never shows its title (opt-in)
//   HoldExpired        a shader warm-up hold that would otherwise wait for a warm-up that never finishes
//   ForceFinishDue     the title has appeared but the screen has not faded out and gone
//   WrapTwoLines       a message for the loading screen's two footer lines
//   Describe*/Format*  the texts of the shader-compiler and graphics-start failures, with the result codes

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vgx::bootwatch {

// ---- a time without any sign of life -----------------------------------------------------------------
constexpr int kSignatureWords = 8;

class StallWatch {
public:
    struct Result {
        bool stalled = false;         // no change for at least the limit
        bool becameStalled = false;   // the first call of this episode (log it once)
        bool resumed = false;         // the first call after an episode that ended (log it once)
        double quietSeconds = 0.0;
    };
    // limitSeconds <= 0: the watch is off and never reports a stall.
    explicit StallWatch(double limitSeconds) : limit_(limitSeconds) {}
    // `words`: every number that moves while the start-up is alive. Any change of any word is a sign of life.
    Result Update(uint64_t nowUs, const uint64_t* words, int count) {
        Result r;
        if (count > kSignatureWords) count = kSignatureWords;
        bool changed = !started_ || count != count_;
        for (int i = 0; !changed && i < count; ++i) changed = words[i] != last_[i];
        if (changed) {
            started_ = true;
            count_ = count;
            for (int i = 0; i < count; ++i) last_[i] = words[i];
            changeUs_ = nowUs;
            if (stalled_) {
                stalled_ = false;
                r.resumed = true;
            }
            return r;
        }
        r.quietSeconds = nowUs > changeUs_ ? static_cast<double>(nowUs - changeUs_) / 1e6 : 0.0;
        if (limit_ > 0.0 && r.quietSeconds >= limit_) {
            r.stalled = true;
            if (!stalled_) {
                stalled_ = true;
                r.becameStalled = true;
            }
        }
        return r;
    }
    bool Stalled() const { return stalled_; }

private:
    double limit_;
    bool started_ = false, stalled_ = false;
    int count_ = 0;
    uint64_t last_[kSignatureWords] = {};
    uint64_t changeUs_ = 0;
};

// The game thread has run at least `minFrames` pictures in the last `windowSeconds` seconds.
inline bool FramesAdvancing(uint64_t framesBefore, uint64_t framesNow, double windowSeconds, double minFramesPerSecond) {
    if (windowSeconds <= 0.0 || framesNow < framesBefore) return false;
    return static_cast<double>(framesNow - framesBefore) >= windowSeconds * minFramesPerSecond;
}

// Opt-in (GXV_BOOT_TITLE_FALLBACK=<seconds>): the last stage has lasted `secondsInStage`, the game thread has been
// running pictures for `advancingSeconds` of it, and no title logo has been announced. 0: off.
inline bool TitleFallbackDue(double fallbackSeconds, double secondsInStage, double advancingSeconds, bool titleSeen) {
    if (fallbackSeconds <= 0.0 || titleSeen) return false;
    return secondsInStage >= fallbackSeconds && advancingSeconds >= 5.0;
}

// A shader warm-up hold (GXV_WARM_HOLD=1) ends after `limitSeconds` whatever the warm-up says. 0: never.
inline bool HoldExpired(uint64_t holdSinceUs, uint64_t nowUs, double limitSeconds) {
    if (limitSeconds <= 0.0 || nowUs <= holdSinceUs) return false;
    return static_cast<double>(nowUs - holdSinceUs) / 1e6 >= limitSeconds;
}

// The title has been seen; the fade and three dark presents normally take under a second. 0: never.
inline bool ForceFinishDue(uint64_t titleSeenUs, uint64_t nowUs, double limitSeconds) {
    if (limitSeconds <= 0.0 || titleSeenUs == 0 || nowUs <= titleSeenUs) return false;
    return static_cast<double>(nowUs - titleSeenUs) / 1e6 >= limitSeconds;
}

// A graphics start that failed has put its reason on the screen: stay on it instead of ending the process.
inline bool ParkOnError(bool enabled, bool errorOnScreen) { return enabled && errorOnScreen; }

// ---- texts ------------------------------------------------------------------------------------------
// The console's file errors are 0x80010000 + the C library's errno.
inline const char* DescribeSceError(uint32_t code) {
    if ((code & 0xFFFF0000u) == 0x80010000u) {
        switch (code & 0xFFFFu) {
            case 1: return "not permitted";
            case 2: return "file not found";
            case 5: return "read error";
            case 12: return "out of memory";
            case 13: return "access denied";
            case 17: return "already exists";
            case 24: return "too many open files";
            case 28: return "card full";
            case 30: return "read-only card";
            default: return "file error";
        }
    }
    if ((code & 0xFFFFF000u) == 0x8002D000u) return "module loader error";
    if ((code & 0xFFFF0000u) == 0x80020000u) return "kernel error";
    return "";
}

// One probe of a file: what a status call and a read-only open said (negative: the console's error).
struct FileProbe {
    int stat = 0;
    int open = 0;
};
// The file can be reached when either call worked: a status call alone is not proof that the file is missing.
inline bool Reachable(const FileProbe& p) { return p.stat >= 0 || p.open >= 0; }
inline bool CompilerMissing(const FileProbe& a, const FileProbe& b) { return !Reachable(a) && !Reachable(b); }

inline void CopyPart(char* out, size_t size, const char* text) {
    if (!out || !size) return;
    std::snprintf(out, size, "%s", text ? text : "");
}

// "line one\nline two", each line at most 88 characters (the footer is about 90 characters wide).
inline void FormatCompilerMissing(char* out, size_t size, const FileProbe& a, const FileProbe& b) {
    const int ca = a.stat < 0 ? a.stat : a.open, cb = b.stat < 0 ? b.stat : b.open;
    char l1[96], l2[96];
    std::snprintf(l1, sizeof l1, "libshacccg.suprx was not found: ur0:data/ 0x%08X, ur0:data/external/ 0x%08X.", static_cast<unsigned>(ca), static_cast<unsigned>(cb));
    std::snprintf(l2, sizeof l2, "Set it up again with the Vita installer, restart the Vita and start the game.");
    std::snprintf(out, size, "%.88s\n%.88s", l1, l2);
}

// Both loads of the shader compiler module failed; the codes are what sceKernelLoadStartModule answered.
inline void FormatCompilerLoadFailure(char* out, size_t size, int codeA, int codeB) {
    char l1[96], l2[96];
    std::snprintf(l1, sizeof l1, "libshacccg.suprx is there but did not load: 0x%08X (ur0:data/), 0x%08X (external/).", static_cast<unsigned>(codeA), static_cast<unsigned>(codeB));
    const char* why = DescribeSceError(static_cast<uint32_t>(codeA));
    if (!*why) why = DescribeSceError(static_cast<uint32_t>(codeB));
    std::snprintf(l2, sizeof l2, "%s%sSet it up again, restart the Vita and start the game. See log.txt.", why, *why ? ": " : "");
    std::snprintf(out, size, "%.88s\n%.88s", l1, l2);
}

// The first line of an info log, without control characters, cut to `limit` characters.
inline void FirstLine(char* out, size_t size, const char* text, size_t limit) {
    if (!out || !size) return;
    size_t n = 0;
    for (const char* p = text ? text : ""; *p && *p != '\n' && *p != '\r' && n + 1 < size && n < limit; ++p)
        out[n++] = static_cast<unsigned char>(*p) >= 32 && static_cast<unsigned char>(*p) < 127 ? *p : '?';
    out[n] = 0;
}

// Splits `text` into the loading screen's two footer lines: at the first newline, else at the last space that fits `width`.
// Returns the number of lines used (1 or 2). Each output is NUL terminated.
inline int WrapTwoLines(const char* text, char* line1, size_t size1, char* line2, size_t size2, size_t width = 88) {
    if (!line1 || !size1) return 0;
    line1[0] = 0;
    if (line2 && size2) line2[0] = 0;
    if (!text || !*text) return 0;
    const char* nl = std::strchr(text, '\n');
    size_t len = std::strlen(text);
    size_t cut = len;
    const char* rest = nullptr;
    if (nl && static_cast<size_t>(nl - text) <= width) {
        cut = static_cast<size_t>(nl - text);
        rest = nl + 1;
    } else if (len > width) {
        cut = width;
        while (cut > width / 2 && text[cut] != ' ') --cut;
        if (text[cut] != ' ') cut = width;
        rest = text + cut + (text[cut] == ' ' ? 1 : 0);
    }
    std::snprintf(line1, size1, "%.*s", static_cast<int>(cut < size1 - 1 ? cut : size1 - 1), text);
    if (rest && line2 && size2) {
        std::snprintf(line2, size2, "%s", rest);
        for (char* p = line2; *p; ++p)
            if (*p == '\n' || *p == '\r') *p = ' ';
        return *line2 ? 2 : 1;
    }
    return 1;
}

// vitaGL keeps a compiled shader at <root>/{v|f}/<top byte, two capitals>/<hash in capitals, not padded>.gxp, hash = XXH3_64 of the
// source text (tools/vitagl/patches/0002). Returns false when the path does not fit.
inline bool ShaderCachePath(char* out, size_t size, const char* root, bool fragment, uint64_t hash) {
    if (!out || !size || !root) return false;
    const int n = std::snprintf(out, size, "%s/%c/%02X/%llX.gxp", root, fragment ? 'f' : 'v', static_cast<unsigned>(hash >> 56),
                                static_cast<unsigned long long>(hash));
    return n > 0 && static_cast<size_t>(n) < size;
}

// The tail of a path that fits `width` characters, with a leading "..." when it was cut.
inline void PathTail(char* out, size_t size, const char* path, size_t width) {
    if (!out || !size) return;
    const size_t len = path ? std::strlen(path) : 0;
    if (len <= width) {
        std::snprintf(out, size, "%s", path ? path : "");
        return;
    }
    std::snprintf(out, size, "...%s", path + len - (width > 3 ? width - 3 : 0));
}

}  // namespace vgx::bootwatch
