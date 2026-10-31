// The Vita worker (vita/vgx-game/title_art_runtime.cpp) run as it is against a fake console: a clock that only the worker's sleeps
// advance, an in-memory card, a display whose buffer changes with the clock. Usage: title_art_runtime <mode>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "title_art_fixture.h"

#define Check(cond, what) do { if (!(cond)) { std::fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, what); std::exit(1); } } while (0)

namespace world {
uint64_t clockUs = 0;
std::map<std::string, std::vector<uint8_t>> files;
struct Handle { std::string path; size_t offset; bool write; };
std::map<int, Handle> handles;
std::vector<std::string> denied;       // path prefixes that cannot be opened for writing
std::vector<std::string> log;
std::set<std::string> directories;
double visibleAt = 5.0, splashEnds = 5.6;
std::vector<uint8_t> frameA, frameB;   // 1024-pixel pitch (the renderer's), RGBA
std::function<const std::vector<uint8_t>&(double)> content;  // what the display shows at a time
std::vector<uint8_t> shown;
SceDisplayPixelFormat format = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
unsigned pitch = 1024;
unsigned displayQueries = 0, threadsCreated = 0, threadsStarted = 0, mkdirs = 0;
std::string titleId = "SMGV00001";
int titleIdResult = 0;
double Now() { return double(clockUs) / 1e6; }
}  // namespace world

extern "C" unsigned int _newlib_heap_size_user = 96u * 1024u * 1024u;
extern "C" int port_boot_title_is_visible(void) { return world::Now() >= world::visibleAt; }
extern "C" int port_vgx_vita_splash_active(void) { return world::Now() < world::splashEnds; }
extern "C" void port_log_summary(const char* fmt, ...) {
    char text[300];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    world::log.push_back(text);
}
extern "C" SceUInt64 sceKernelGetProcessTimeWide() { return world::clockUs; }
extern "C" int sceKernelDelayThread(SceUInt us) {
    world::clockUs += us;
    Check(world::clockUs < 400u * 1000000u, "the worker must finish (or give up) within 400 fake seconds");
    return 0;
}
extern "C" int sceKernelExitDeleteThread(int) { return 0; }
extern "C" SceUID sceKernelCreateThread(const char* name, SceKernelThreadEntry, int priority, SceSize, SceUInt, int cpu, const SceKernelThreadOptParam*) {
    Check(std::strcmp(name, "smg_titleart") == 0 && priority == 0xBF && cpu == 0x70000, "lowest user priority, user cores");
    ++world::threadsCreated;
    return 77;
}
extern "C" int sceKernelStartThread(SceUID, SceSize, void*) { ++world::threadsStarted; return 0; }
extern "C" int sceKernelDeleteThread(SceUID) { return 0; }
extern "C" int sceAppMgrAppParamGetString(int, int param, char* out, SceSize size) {
    Check(param == 12, "the title id is parameter 12");
    if (world::titleIdResult < 0) return world::titleIdResult;
    std::snprintf(out, size, "%s", world::titleId.c_str());
    return 0;
}
extern "C" int sceDisplayWaitVblankStart() { return 0; }
extern "C" int sceDisplayGetFrameBuf(SceDisplayFrameBuf* fb, SceDisplaySetBufSync sync) {
    Check(sync == SCE_DISPLAY_SETBUF_NEXTFRAME, "the next-frame buffer is read");
    ++world::displayQueries;
    world::shown = world::content(world::Now());
    std::memset(fb, 0, sizeof *fb);
    fb->size = sizeof *fb;
    fb->base = world::shown.data();
    fb->pitch = world::pitch;
    fb->pixelformat = world::format;
    fb->width = 960;
    fb->height = 544;
    return 0;
}
extern "C" int sceIoMkdir(const char* path, SceMode) { world::directories.insert(path); ++world::mkdirs; return 0; }
extern "C" int sceIoGetstat(const char* path, SceIoStat* st) {
    std::memset(st, 0, sizeof *st);
    auto it = world::files.find(path);
    if (it == world::files.end()) return int(0x80010002u);
    st->st_mode = SCE_S_IFREG;
    st->st_size = it->second.size();
    return 0;
}
static bool Denied(const std::string& path) {
    for (const auto& prefix : world::denied) if (path.rfind(prefix, 0) == 0) return true;
    return false;
}
extern "C" SceUID sceIoOpen(const char* path, int flags, SceMode) {
    static int next = 200;
    const bool write = (flags & SCE_O_WRONLY) != 0;
    if (write) {
        if (Denied(path)) return int(0x80010001u);
        world::files[path].clear();
    } else if (!world::files.count(path)) {
        return int(0x80010002u);
    }
    world::handles[++next] = {path, 0, write};
    return next;
}
extern "C" int sceIoWrite(SceUID fd, const void* data, SceSize n) {
    auto& h = world::handles.at(fd);
    Check(h.write, "only write handles are written");
    auto& file = world::files[h.path];
    file.insert(file.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n);
    return int(n);
}
extern "C" int sceIoRead(SceUID fd, void* out, SceSize n) {
    auto& h = world::handles.at(fd);
    const auto& file = world::files.at(h.path);
    const size_t count = std::min<size_t>(n, file.size() - h.offset);
    std::memcpy(out, file.data() + h.offset, count);
    h.offset += count;
    return int(count);
}
extern "C" int sceIoClose(SceUID fd) { Check(world::handles.erase(fd) == 1, "closing an open handle"); return 0; }
extern "C" int sceIoRemove(const char* path) { return world::files.erase(path) ? 0 : int(0x80010002u); }
extern "C" int sceIoRename(const char* from, const char* to) {
    if (!world::files.count(from) || world::files.count(to) || Denied(to)) return int(0x80010011u);  // (the console's rename does not replace)
    world::files[to] = std::move(world::files[from]);
    world::files.erase(from);
    return 0;
}

#include "../vita/vgx-game/title_art_runtime.cpp"

using namespace port::titleart;

static std::vector<uint8_t> Pitched(const std::vector<uint8_t>& rgba) {  // 960-wide RGBA to a 1024-pixel pitch
    std::vector<uint8_t> out(size_t(1024) * 4 * kH, 0xEE);
    for (int y = 0; y < kH; ++y) std::memcpy(&out[size_t(y) * 1024 * 4], &rgba[size_t(y) * kW * 4], size_t(kW) * 4);
    return out;
}
static bool Logged(const char* part) {
    for (const auto& line : world::log) if (line.find(part) != std::string::npos) return true;
    return false;
}
static std::vector<uint8_t> Unpitch(const std::vector<uint8_t>& pitched) {
    std::vector<uint8_t> out(kRgbaBytes);
    for (int y = 0; y < kH; ++y) std::memcpy(&out[size_t(y) * kW * 4], &pitched[size_t(y) * 1024 * 4], size_t(kW) * 4);
    return out;
}
static std::vector<std::string> PictureFiles(const std::string& id = "SMGV00001") {
    const std::string app = "ux0:app/" + id + "/sce_sys/", meta = "ur0:appmeta/" + id + "/";
    return {app + "icon0.png", app + "pic0.png", app + "livearea/contents/bg.png", app + "livearea/contents/startup.png", meta + "icon0.png", meta + "pic0.png"};
}
static void Setup() {
    const auto title = fixture::MakeTitle();
    world::frameA = Pitched(title);
    world::frameB = std::vector<uint8_t>(size_t(1024) * 4 * kH, 0);  // the loading screen's dark picture
    for (size_t i = 3; i < world::frameB.size(); i += 4) world::frameB[i] = 255;
    world::content = [](double t) -> const std::vector<uint8_t>& { return t >= world::visibleAt ? world::frameA : world::frameB; };
    // The neutral pictures a package update puts back.
    world::files["ux0:app/SMGV00001/sce_sys/icon0.png"] = std::vector<uint8_t>(5239, 1);
}
static void ExpectEverything(const char* why) {
    for (const auto& path : PictureFiles()) {
        Check(world::files.count(path) && world::files[path].size() > 100, why);
        Check(world::files[path].size() != 5239, "the neutral icon was replaced");
        Check(world::files[path].size() > 8 && std::memcmp(world::files[path].data(), "\x89PNG", 4) == 0, "a PNG");
    }
    for (const auto& [path, data] : world::files) Check(path.size() < 4 || path.compare(path.size() - 4, 4, ".tmp") != 0, "no temporary file is left");
    Check(world::handles.empty(), "every file is closed");
}
static void ExpectCapture(const std::vector<uint8_t>& frame) {
    Check(world::files.count(kTitlePath) && world::files[kTitlePath].size() == kRgbaBytes, "title.rgba has the exact size");
    Check(world::files[kTitlePath] == Unpitch(frame), "title.rgba is the presented frame, row by row, with the display's pitch dropped");
}

int main(int argc, char** argv) {
    Check(argc == 2, "one mode");
    const std::string mode = argv[1];
    Setup();
    if (mode == "disabled" || mode == "enabled-start") {
        setenv("GXV_TITLEART", mode == "disabled" ? "0" : "1", 1);
        port_title_art_start();
        port_title_art_start();  // once only
        Check(world::threadsCreated == (mode == "disabled" ? 0u : 1u) && world::threadsStarted == world::threadsCreated, "GXV_TITLEART=0 starts no worker, the default starts one");
        Check(world::files.size() == 1 && world::displayQueries == 0, "starting touches nothing");
        std::puts(("PASS: " + mode).c_str());
        return 0;
    }
    if (mode == "titleart-off-in-worker") {  // (the worker itself also honours 0: nothing is written even if it were running)
        setenv("GXV_TITLEART", "0", 1);
        Main(0, nullptr);
        Check(world::files.size() == 1 && world::displayQueries == 0, "mode 0: no capture, no pictures");
        std::puts("PASS: titleart-off-in-worker");
        return 0;
    }
    if (mode == "force") {
        setenv("GXV_TITLEART", "1", 1);
        Main(0, nullptr);  // first start makes everything
        const auto first = world::files[kTitlePath];
        world::log.clear();
        world::clockUs = 0;
        world::frameA = Pitched(fixture::MakeTitle(true, true, fixture::Logo(), 99));  // a different sky
        world::displayQueries = 0;
        setenv("GXV_TITLEART", "2", 1);
        Main(0, nullptr);
        Check(world::displayQueries > 0 && world::files[kTitlePath] != first, "GXV_TITLEART=2 captures again");
        ExpectCapture(world::frameA);
        ExpectEverything("forced start rewrites the pictures");
        std::puts("PASS: force");
        return 0;
    }
    setenv("GXV_TITLEART", "1", 1);
    if (mode == "first-run" || mode == "write-once" || mode == "package-update" || mode == "old-bubble-version") {
        Main(0, nullptr);
        ExpectCapture(world::frameA);
        ExpectEverything("first start makes the pictures");
        Check(world::files.count(kStampPath), "the stamp is written");
        Check(world::clockUs / 1e6 >= 7.5 && world::clockUs / 1e6 < 8.5, "captured about 2 s after the loading screen went (5.6 s): not before it is gone and settled");
        Check(world::mkdirs >= 2 && world::directories.count(kArtDir), "the folder is made");
        Check(Logged("captured") && Logged("6 of 6 picture files written") && Logged("4 of 4 pictures made"), "one line each for the capture and the pictures");
        // The sizes of what was written are what the shell will cache: identical files in the app folder and appmeta.
        Check(world::files["ux0:app/SMGV00001/sce_sys/icon0.png"] == world::files["ur0:appmeta/SMGV00001/icon0.png"] &&
              world::files["ux0:app/SMGV00001/sce_sys/pic0.png"] == world::files["ur0:appmeta/SMGV00001/pic0.png"], "app folder and shell cache get the same pictures");
        Stamp stamp;
        Check(ParseStamp(std::string(world::files[kStampPath].begin(), world::files[kStampPath].end()).c_str(), stamp) && stamp.capture == kCaptureVersion &&
              stamp.bubble == kBubbleVersion && stamp.icon == long(world::files["ux0:app/SMGV00001/sce_sys/icon0.png"].size()), "the stamp records the versions and the icon size");
        if (mode == "first-run") { std::puts("PASS: first-run"); return 0; }
        const auto snapshot = world::files;
        world::log.clear();
        world::clockUs = 0;
        world::displayQueries = 0;
        if (mode == "write-once") {
            Main(0, nullptr);
            Check(world::files == snapshot && world::displayQueries == 0 && world::log.empty(), "second start: nothing read from the display, nothing written, nothing logged");
            Check(world::clockUs / 1e6 < 6.0, "and the worker is gone as soon as the title is up");
        } else if (mode == "package-update") {  // the package's own neutral icon is back: the pictures are made again, from the saved capture, no display read
            world::files["ux0:app/SMGV00001/sce_sys/icon0.png"] = std::vector<uint8_t>(5239, 1);
            world::files["ux0:app/SMGV00001/sce_sys/pic0.png"] = std::vector<uint8_t>(777, 2);
            world::frameA = Pitched(fixture::MakeTitle(false, false));  // (nothing on the display is read)
            Main(0, nullptr);
            Check(world::displayQueries == 0, "rebuilt from title.rgba without reading the display");
            Check(world::files[kTitlePath] == snapshot.at(kTitlePath), "the capture is untouched");
            ExpectEverything("the pictures are rebuilt after a package update");
            Check(world::files["ux0:app/SMGV00001/sce_sys/icon0.png"] == snapshot.at("ux0:app/SMGV00001/sce_sys/icon0.png"), "same capture, same picture (deterministic)");
            Check(world::clockUs / 1e6 >= 8.5 && world::clockUs / 1e6 < 10, "a few seconds after the title");
        } else {
            Stamp old{kCaptureVersion, kBubbleVersion - 1, long(snapshot.at("ux0:app/SMGV00001/sce_sys/icon0.png").size())};
            char text[96];
            const int n = FormatStamp(old, text, sizeof text);
            world::files[kStampPath] = std::vector<uint8_t>(text, text + n);
            world::files["ur0:appmeta/SMGV00001/pic0.png"] = std::vector<uint8_t>(10, 3);
            Main(0, nullptr);
            Check(world::displayQueries == 0 && world::files["ur0:appmeta/SMGV00001/pic0.png"].size() > 100, "an older picture version rebuilds from the capture");
        }
        std::puts(("PASS: " + mode).c_str());
        return 0;
    }
    if (mode == "unsettled") {  // a title whose logo region keeps changing: taken anyway after 10 s
        world::content = [](double t) -> const std::vector<uint8_t>& {
            static std::vector<uint8_t> a, b;
            if (a.empty()) { a = world::frameA; b = world::frameA; for (int y = 60; y < 340; ++y) for (int x = 230; x < 720; ++x) b[(size_t(y) * 1024 + x) * 4 + 1] ^= 0x30; }
            if (t < world::visibleAt) return world::frameB;
            return int(t * 2.0) % 2 ? a : b;  // switches every half second
        };
        // (the tear check after the copy compares a fresh read with the copy: with a change every half second it passes when no switch falls inside it)
        Main(0, nullptr);
        Check(world::clockUs / 1e6 >= 15.5, "not taken before 10 s after the loading screen went");
        Check(world::files.count(kTitlePath), "taken after 10 s");
        std::puts("PASS: unsettled");
        return 0;
    }
    if (mode == "not-a-title") {  // the first stage's picture (no logo) is presented instead: three tries, nothing kept
        world::content = [](double) -> const std::vector<uint8_t>& { return world::frameB; };
        Main(0, nullptr);
        Check(!world::files.count(kTitlePath) && !world::files.count(kStampPath), "nothing saved");
        Check(Logged("not the title screen") && Logged("no capture this start"), "said so, once per try, and that it gave up");
        Check(world::files.size() == 1, "nothing at all written");
        std::puts("PASS: not-a-title");
        return 0;
    }
    if (mode == "unsupported-format") {
        world::format = static_cast<SceDisplayPixelFormat>(SCE_DISPLAY_PIXELFORMAT_A8B8G8R8 + 1);
        Main(0, nullptr);
        Check(world::files.size() == 1 && Logged("no capture this start"), "an unknown pixel format is not captured, and the worker gives up with one line");
        std::puts("PASS: unsupported-format");
        return 0;
    }
    if (mode == "tear") {  // the display changes during the first copy: that capture is dropped, the next one kept
        world::content = [](double t) -> const std::vector<uint8_t>& {
            static std::vector<uint8_t> moved;
            if (moved.empty()) { moved = world::frameA; for (int y = 60; y < 340; ++y) for (int x = 230; x < 720; ++x) moved[(size_t(y) * 1024 + x) * 4] ^= 0x40; }
            if (t < world::visibleAt) return world::frameB;
            return world::displayQueries == 4 ? moved : world::frameA;  // (query 4 is the verification read after the first copy)
        };
        Main(0, nullptr);
        Check(Logged("changed while it was copied"), "the torn copy was noticed");
        ExpectCapture(world::frameA);
        ExpectEverything("and the next capture made the pictures");
        std::puts("PASS: tear");
        return 0;
    }
    if (mode == "appmeta-denied" || mode == "app-denied") {
        world::denied.push_back(mode == "app-denied" ? "ux0:app/" : "ur0:appmeta/");
        Main(0, nullptr);
        ExpectCapture(world::frameA);
        Check(world::files.count(kStampPath), "the stamp is still written");
        Check(Logged(mode == "app-denied" ? "2 of 6 picture files written" : "4 of 6 picture files written"), "the count of what could be written is logged");
        // The next start does not try again (the icon is exactly what it was when the stamp was written).
        const auto snapshot = world::files;
        world::clockUs = 0;
        world::displayQueries = 0;
        Main(0, nullptr);
        Check(world::files == snapshot && world::displayQueries == 0, "a refused folder is not retried at every start");
        std::puts(("PASS: " + mode).c_str());
        return 0;
    }
    if (mode == "low-heap") {
        _newlib_heap_size_user = 8u * 1024u * 1024u;
        Main(0, nullptr);
        Check(world::files.size() == 1 && world::displayQueries == 0 && Logged("less than 32 MiB"), "a short heap: nothing is done, one line says so");
        std::puts("PASS: low-heap");
        return 0;
    }
    if (mode == "title-id" || mode == "title-id-invalid" || mode == "title-id-failed") {
        if (mode == "title-id") world::titleId = "ABCD12345";
        if (mode == "title-id-invalid") world::titleId = "bad/id";
        if (mode == "title-id-failed") world::titleIdResult = -1;
        world::files["ux0:app/ABCD12345/sce_sys/icon0.png"] = std::vector<uint8_t>(5239, 1);
        Main(0, nullptr);
        for (const auto& path : PictureFiles(mode == "title-id" ? "ABCD12345" : "SMGV00001")) Check(world::files.count(path), "the pictures go under the running title id (or the default)");
        if (mode == "title-id") Check(!world::files.count("ur0:appmeta/SMGV00001/icon0.png"), "and not under the default id");
        std::puts(("PASS: " + mode).c_str());
        return 0;
    }
    if (mode == "corrupt-capture") {  // the saved capture is junk of the right size: it is removed and captured again at the next start
        Main(0, nullptr);
        world::files[kTitlePath] = std::vector<uint8_t>(kRgbaBytes, 0x55);
        world::files["ux0:app/SMGV00001/sce_sys/icon0.png"] = std::vector<uint8_t>(5239, 1);
        world::log.clear();
        world::clockUs = 0;
        Main(0, nullptr);
        Check(!world::files.count(kTitlePath) && Logged("captured again"), "a saved picture that is not a title is dropped");
        world::clockUs = 0;
        Main(0, nullptr);
        ExpectCapture(world::frameA);
        std::puts("PASS: corrupt-capture");
        return 0;
    }
    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 2;
}
