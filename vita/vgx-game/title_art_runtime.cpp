// The player's own pictures. The public package carries no game artwork: the first time the real title screen has been fully
// shown, this worker copies the presented frame to ux0:data/smg/art/title.rgba (the loading screen and the Settings backdrop read
// it from then on) and makes the bubble and LiveArea pictures from it (the console shows them after the home screen has been
// refreshed). Everything that is not a console call is in port/title_art.h and is tested on the host.
//
// It never touches the render path: the thread runs at the lowest user priority, reads the display buffer once in a few large
// slices (the buffer is in video memory, which the CPU reads slowly), and does the rest from its own memory. Memory is bounded
// (about 8 MiB at the peak: the 2 MiB capture and 5.5 MiB for the pictures, freed afterwards, and not started at all when the C heap has less than 32 MiB free). Every failure
// is silent apart from one log line.
#include "title_art_runtime.h"
#include "port/title_art.h"
#include "vgx_common.h"

#include <malloc.h>
#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <cstdio>
#include <cstring>

extern "C" int port_boot_title_is_visible(void);
extern "C" int port_vgx_vita_splash_active(void);
extern "C" void port_log_summary(const char*, ...);
extern "C" unsigned int _newlib_heap_size_user;

namespace {
using namespace port::titleart;

uint64_t NowUs() { return sceKernelGetProcessTimeWide(); }

long FileSize(const char* path) {
    SceIoStat st{};
    if (sceIoGetstat(path, &st) < 0 || !SCE_S_ISREG(st.st_mode) || st.st_size < 0) return -1;
    return static_cast<long>(st.st_size);
}
bool WriteAll(const char* path, const uint8_t* data, size_t size) {
    const SceUID f = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (f < 0) return false;
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        const size_t piece = size - done < 65536 ? size - done : 65536;
        const int n = sceIoWrite(f, data + done, static_cast<SceSize>(piece));
        if (n <= 0) { ok = false; break; }
        done += static_cast<size_t>(n);
    }
    if (sceIoClose(f) < 0) ok = false;
    return ok && done == size;
}
// Written beside the target and renamed over it. The console's rename need not replace an existing file, so the old one is
// removed first; should the rename then fail, the data (still in memory) is written to the target directly.
bool ReplaceFile(const char* path, const uint8_t* data, size_t size) {
    char temporary[112];
    std::snprintf(temporary, sizeof temporary, "%s.tmp", path);
    if (!WriteAll(temporary, data, size)) { sceIoRemove(temporary); return false; }
    sceIoRemove(path);
    if (sceIoRename(temporary, path) >= 0) return true;
    const bool ok = WriteAll(path, data, size);
    sceIoRemove(temporary);
    return ok;
}
bool ReadSmall(const char* path, char* out, size_t size) {
    const SceUID f = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (f < 0) return false;
    const int n = sceIoRead(f, out, static_cast<SceSize>(size - 1));
    sceIoClose(f);
    if (n <= 0) return false;
    out[n] = 0;
    return true;
}

bool HeapAllows() {
    const struct mallinfo mi = mallinfo();
    const long freeBytes = static_cast<long>(_newlib_heap_size_user) - static_cast<long>(mi.uordblks);
    return freeBytes >= 32L * 1024 * 1024;
}

struct Display {
    const uint8_t* base = nullptr;
    size_t pitchBytes = 0;
    bool Query() {
        SceDisplayFrameBuf fb;
        std::memset(&fb, 0, sizeof fb);
        fb.size = sizeof fb;
        // The buffer set for the next frame: finished (the renderer sets a buffer only once it has been drawn) and not being drawn into.
        if (sceDisplayGetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME) < 0 || !fb.base) return false;
        if (fb.pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 || fb.width < kW || fb.height < kH || fb.pitch < kW) return false;
        base = static_cast<const uint8_t*>(fb.base);
        pitchBytes = static_cast<size_t>(fb.pitch) * 4;
        return true;
    }
};
bool SampleDisplay(uint8_t* out) {
    Display d;
    if (!d.Query()) return false;
    SampleFrame(d.base, d.pitchBytes, out);
    return true;
}
// The presented frame as 960x544 RGBA, in slices with a short pause between them (the video memory is slow to read and the game
// is drawing into it); checked afterwards against a fresh sample of the display, so that a swap during the copy (a torn frame)
// is not kept.
bool CaptureFrame(uint8_t* rgba) {
    sceDisplayWaitVblankStart();
    Display d;
    if (!d.Query()) return false;
    for (int y = 0; y < kH; ++y) {
        std::memcpy(rgba + static_cast<size_t>(y) * kW * 4, d.base + static_cast<size_t>(y) * d.pitchBytes, static_cast<size_t>(kW) * 4);
        if ((y & 31) == 31) sceKernelDelayThread(400);
    }
    for (size_t i = 3; i < kRgbaBytes; i += 4) rgba[i] = 255;
    uint8_t now[kSampleBytes], mine[kSampleBytes];
    if (!SampleDisplay(now)) return false;
    SampleFrame(rgba, static_cast<size_t>(kW) * 4, mine);
    return MeanAbsDiff(now, mine, kSampleBytes) <= 4.0;
}

struct SinkContext {
    const char* titleId;
    int written = 0, wanted = 0;
};
bool Sink(void* context, int which, const uint8_t* png, size_t length) {
    auto& c = *static_cast<SinkContext*>(context);
    char targets[2][96];
    const int count = Targets(which, c.titleId, targets);
    bool all = true;
    for (int i = 0; i < count; ++i) {
        ++c.wanted;
        if (ReplaceFile(targets[i], png, length)) ++c.written;
        else all = false;
    }
    return all;
}

// Makes the pictures from `rgba`, records the stamp. Returns true when every file was written.
bool MakePictures(const uint8_t* rgba, const char* titleId) {
    const uint64_t t0 = NowUs();
    SinkContext context{titleId};
    int made = 0;
    const bool built = BuildPictures(rgba, Sink, &context, &made);
    char iconPath[96];
    std::snprintf(iconPath, sizeof iconPath, "ux0:app/%s/sce_sys/icon0.png", titleId);
    Stamp stamp;
    stamp.capture = kCaptureVersion;
    stamp.bubble = built ? kBubbleVersion : 0;  // a build that failed is tried again at the next start
    stamp.icon = FileSize(iconPath);
    char text[96];
    const int length = FormatStamp(stamp, text, sizeof text);
    if (length > 0) ReplaceFile(kStampPath, reinterpret_cast<const uint8_t*>(text), static_cast<size_t>(length));
    port_log_summary("title art: %d of %d picture files written (%d of 4 pictures made, ID %s) in %u ms", context.written, context.wanted, made, titleId,
                     static_cast<unsigned>((NowUs() - t0) / 1000u));
    return built && context.written == context.wanted;
}

int Main(SceSize, void*) {
    const long mode = vgx::EnvInt("GXV_TITLEART", 1);
    char titleId[32] = "";
    if (sceAppMgrAppParamGetString(0, 12, titleId, sizeof titleId) < 0 || !ValidTitleId(titleId)) std::snprintf(titleId, sizeof titleId, "%s", kDefaultTitleId);
    CaptureTrigger trigger;
    Plan plan = Plan::kNone;
    bool planned = false;
    double buildAfter = -1;
    uint8_t before[kSampleBytes], after[kSampleBytes];
    bool haveBefore = false, saved = false;
    for (;;) {
        sceKernelDelayThread(100000);
        const double now = static_cast<double>(NowUs()) / 1e6;
        const bool ready = port_boot_title_is_visible() != 0 && port_vgx_vita_splash_active() == 0;
        if (!ready) { haveBefore = false; buildAfter = -1; trigger.Poll(now, false); continue; }
        if (!planned) {
            planned = true;
            Stamp stamp;
            char text[96];
            if (!ReadSmall(kStampPath, text, sizeof text) || !ParseStamp(text, stamp)) stamp = Stamp{};
            char iconPath[96];
            std::snprintf(iconPath, sizeof iconPath, "ux0:app/%s/sce_sys/icon0.png", titleId);
            plan = Decide(static_cast<int>(mode), FileSize(kTitlePath) == static_cast<long>(kRgbaBytes), stamp, FileSize(iconPath));
            if (plan == Plan::kNone) return sceKernelExitDeleteThread(0);
            if (!HeapAllows()) {
                port_log_summary("title art: skipped, the C heap has less than 32 MiB free");
                return sceKernelExitDeleteThread(0);
            }
            sceIoMkdir("ux0:data/smg", 0777);
            sceIoMkdir(kArtDir, 0777);
        }
        if (plan == Plan::kBuildOnly) {  // the capture is there; only the pictures are out of date: a few seconds after the title
            if (buildAfter < 0) buildAfter = now + 3.0;
            if (now < buildAfter) continue;
            Mem<uint8_t> rgba;
            const SceUID f = rgba.Alloc(kRgbaBytes) ? sceIoOpen(kTitlePath, SCE_O_RDONLY, 0) : -1;
            size_t got = 0;
            if (f >= 0) {
                while (got < kRgbaBytes) {
                    const int n = sceIoRead(f, rgba.p + got, static_cast<SceSize>(kRgbaBytes - got > 65536 ? 65536 : kRgbaBytes - got));
                    if (n <= 0) break;
                    got += static_cast<size_t>(n);
                }
                sceIoClose(f);
            }
            const bool valid = got == kRgbaBytes && LooksLikeTitle(rgba.p, 4);
            if (valid) {
                MakePictures(rgba.p, titleId);
            } else {
                port_log_summary("title art: the saved capture cannot be read or is not a title picture; it is captured again");
                sceIoRemove(kTitlePath);
            }
            return sceKernelExitDeleteThread(0);
        }
        switch (trigger.Poll(now, true)) {
            case CaptureTrigger::kWait: break;
            case CaptureTrigger::kSample: {
                uint8_t* out = haveBefore ? after : before;
                if (!SampleDisplay(out)) { trigger.Sampled(now, -1); haveBefore = false; break; }
                const double diff = haveBefore ? MeanAbsDiff(before, after, kSampleBytes) : -1.0;
                if (haveBefore) std::memcpy(before, after, sizeof before);
                haveBefore = true;
                trigger.Sampled(now, diff);
                break;
            }
            case CaptureTrigger::kCapture: {
                Mem<uint8_t> rgba;
                bool ok = false;
                if (!rgba.Alloc(kRgbaBytes)) {
                    port_log_summary("title art: no memory for the capture");
                } else if (!CaptureFrame(rgba.p)) {
                    port_log_summary("title art: the display could not be read, or the frame changed while it was copied; trying again");
                } else if (!LooksLikeTitle(rgba.p, 4)) {
                    port_log_summary("title art: the presented frame is not the title screen; not saved");
                } else if (!ReplaceFile(kTitlePath, rgba.p, kRgbaBytes)) {
                    port_log_summary("title art: ux0:data/smg/art/title.rgba could not be written");
                    ok = true;  // (nothing more to try this session)
                } else {
                    port_log_summary("title art: the title screen was captured to ux0:data/smg/art/title.rgba");
                    MakePictures(rgba.p, titleId);
                    ok = true;
                }
                saved = saved || ok;
                trigger.Captured(ok);
                haveBefore = false;
                break;
            }
            case CaptureTrigger::kGiveUp:
                if (!saved) port_log_summary("title art: no capture this start (the title screen did not settle, or the frame could not be read)");
                return sceKernelExitDeleteThread(0);
        }
    }
}
}  // namespace

extern "C" void port_title_art_start(void) {
    static bool started = false;
    if (started) return;
    started = true;
    if (vgx::EnvInt("GXV_TITLEART", 1) <= 0) return;  // read once: 0 = off, 1 = on (default), 2 = a new capture at every start
    // Lowest user priority, any user core: it only runs when a core would otherwise idle, and it never waits on the game.
    const SceUID thread = sceKernelCreateThread("smg_titleart", Main, 0xBF, 256 * 1024, 0, 0x70000, nullptr);
    if (thread < 0) return;
    if (sceKernelStartThread(thread, 0, nullptr) < 0) sceKernelDeleteThread(thread);
}
