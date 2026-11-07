// Host tests of port/title_art.h and port/title_art_png.h (everything that has no console call).
//   title_art logic
//   title_art deflate <dir>            writes <name>.raw and <name>.z for a zlib check in Python
//   title_art png <dir>                writes <name>.rgb and <name>.png (and cases.txt) for a Pillow check in Python
//   title_art synthetic <dir>          a made-up title screen through the whole picture path
//   title_art pictures <capture> <dir> a real capture through the whole picture path
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "title_art_fixture.h"

using namespace port::titleart;

static void Check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
static void WriteFile(const std::string& path, const void* data, size_t n) {
    std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data), std::streamsize(n));
}

// ------------------------------------------------------------------------------------------------ logic
static void Logic() {
    // Decide: disabled, forced, write-once, version upgrades, a package update that put the neutral icon back.
    const Stamp fresh{kCaptureVersion, kBubbleVersion, 1234};
    Check(Decide(0, false, Stamp{}, -1) == Plan::kNone, "GXV_TITLEART=0 does nothing, even with no capture");
    Check(Decide(0, true, Stamp{}, 5) == Plan::kNone, "GXV_TITLEART=0 does nothing with a stale stamp");
    Check(Decide(1, false, Stamp{}, -1) == Plan::kCapture, "first start captures");
    Check(Decide(1, true, Stamp{}, 1234) == Plan::kCapture, "a capture without a stamp is made again");
    Check(Decide(1, false, fresh, 1234) == Plan::kCapture, "a stamp without the capture file captures");
    Check(Decide(1, true, fresh, 1234) == Plan::kNone, "capture, stamp and icon all current: write once, nothing to do");
    Check(Decide(2, true, fresh, 1234) == Plan::kCapture, "GXV_TITLEART=2 forces a new capture");
    Check(Decide(3, true, fresh, 1234) == Plan::kCapture, "values above 2 force too");
    Check(Decide(1, true, Stamp{kCaptureVersion - 1, kBubbleVersion, 1234}, 1234) == Plan::kCapture, "an old capture version captures again");
    Check(Decide(1, true, Stamp{kCaptureVersion, kBubbleVersion - 1, 1234}, 1234) == Plan::kBuildOnly, "an old picture version rebuilds from the capture");
    Check(Decide(1, true, fresh, 4321) == Plan::kBuildOnly, "a different icon size (package update) rebuilds from the capture");
    Check(Decide(1, true, fresh, -1) == Plan::kBuildOnly, "an unreadable icon rebuilds");
    // The stamp text.
    char text[96];
    Check(FormatStamp(fresh, text, sizeof text) > 0, "stamp formats");
    Stamp parsed;
    Check(ParseStamp(text, parsed) && parsed.capture == fresh.capture && parsed.bubble == fresh.bubble && parsed.icon == fresh.icon, "stamp round trip");
    Check(!ParseStamp("garbage", parsed) && parsed.capture == 0 && parsed.icon == -1, "garbage is an empty stamp");
    Check(!ParseStamp(nullptr, parsed), "null is an empty stamp");
    // Title id and targets.
    Check(ValidTitleId("SMGV00001") && ValidTitleId("PCSE00000") && !ValidTitleId("") && !ValidTitleId("smgv00001") && !ValidTitleId("SMGV0000") &&
              !ValidTitleId("SMGV000011") && !ValidTitleId("SMG/00001") && !ValidTitleId(nullptr), "title id validity");
    char targets[2][96];
    Check(Targets(kIcon, "SMGV00001", targets) == 2 && std::string(targets[0]) == "ux0:app/SMGV00001/sce_sys/icon0.png" &&
              std::string(targets[1]) == "ur0:appmeta/SMGV00001/icon0.png", "icon0 goes to the app folder and the shell's cache");
    Check(Targets(kPic0, "SMGV00001", targets) == 2 && std::string(targets[0]) == "ux0:app/SMGV00001/sce_sys/pic0.png" &&
              std::string(targets[1]) == "ur0:appmeta/SMGV00001/pic0.png", "pic0 goes to the app folder and the shell's cache");
    Check(Targets(kBackground, "SMGV00001", targets) == 1 && std::string(targets[0]) == "ux0:app/SMGV00001/sce_sys/livearea/contents/bg.png", "bg.png: app folder only");
    Check(Targets(kStartup, "SMGV00001", targets) == 1 && std::string(targets[0]) == "ux0:app/SMGV00001/sce_sys/livearea/contents/startup.png", "startup.png: app folder only");
    // The stable-frame decision.
    {
        CaptureTrigger t;
        Check(t.Poll(0.0, false) == CaptureTrigger::kWait && t.Poll(100.0, false) == CaptureTrigger::kWait, "never acts while the title is not up with the loading screen gone");
        double now = 10.0;
        Check(t.Poll(now, true) == CaptureTrigger::kWait, "ready: nothing at once");
        now += 1.0;
        Check(t.Poll(now, true) == CaptureTrigger::kWait, "1.0 s: still waiting");
        now += 0.5;
        Check(t.Poll(now, true) == CaptureTrigger::kSample, "1.5 s: the first sample");
        t.Sampled(now, -1);
        Check(t.Poll(now + 0.1, true) == CaptureTrigger::kWait, "no second sample before 0.5 s");
        now += 0.5;
        Check(t.Poll(now, true) == CaptureTrigger::kSample, "2.0 s: the second sample");
        t.Sampled(now, 1.0);
        Check(t.Poll(now, true) == CaptureTrigger::kCapture, "two samples 0.5 s apart that agree, after 2 s: capture");
        t.Captured(true);
        Check(t.Done() && t.Poll(now + 1, true) == CaptureTrigger::kGiveUp, "after a saved capture: nothing more, ever");
    }
    {
        CaptureTrigger t;  // a title that keeps moving
        double now = 0;
        t.Poll(now, true);
        int captures = 0;
        for (int i = 0; i < 200 && !t.Done(); ++i) {
            now = i * 0.1;
            const auto act = t.Poll(now, true);
            if (act == CaptureTrigger::kSample) t.Sampled(now, 9.0);
            else if (act == CaptureTrigger::kCapture) { Check(now >= 10.0, "a moving title is not taken before 10 s"); ++captures; t.Captured(true); }
        }
        Check(captures == 1, "a title that never settles is taken once after 10 s");
    }
    {
        CaptureTrigger t;  // the loading screen comes back (or the title goes away): the wait starts again
        t.Poll(0, true);
        t.Poll(1.6, true);
        t.Sampled(1.6, -1);
        Check(t.Poll(2.2, false) == CaptureTrigger::kWait, "not ready: wait");
        Check(t.Poll(2.3, true) == CaptureTrigger::kWait, "ready again: the dwell starts from zero");
        Check(t.Poll(3.9, true) == CaptureTrigger::kSample, "and the first sample comes 1.5 s later");
    }
    {
        CaptureTrigger t;  // frames that fail their checks: 40 tries (the title camera moves), then it is over for this start
        double now = 0;
        t.Poll(now, true);
        int tries = 0;
        for (int i = 0; i < 1200 && !t.Done(); ++i) {
            now = i * 0.1;
            const auto act = t.Poll(now, true);
            if (act == CaptureTrigger::kSample) t.Sampled(now, 0.5);
            else if (act == CaptureTrigger::kCapture) { ++tries; t.Captured(false); }
        }
        Check(tries == 40 && t.Done(), "40 failed captures end the attempts");
    }
    {
        CaptureTrigger t;  // never ready long enough: gives up after 120 s of being ready without a capture
        double now = 0;
        t.Poll(now, true);
        CaptureTrigger::Act last = CaptureTrigger::kWait;
        for (int i = 0; i < 1300 && last != CaptureTrigger::kGiveUp; ++i) { now = i * 0.1; last = t.Poll(now, true); if (last == CaptureTrigger::kSample) t.Sampled(now, -1); }
        Check(last == CaptureTrigger::kGiveUp && now >= 119.9 && now <= 120.2, "gives up after 120 s when no pair of samples ever completes");
    }
    // Samples.
    {
        auto a = fixture::MakeTitle(), b = a;
        uint8_t sa[kSampleBytes], sb[kSampleBytes];
        SampleFrame(a.data(), size_t(fixture::W) * 4, sa);
        SampleFrame(b.data(), size_t(fixture::W) * 4, sb);
        Check(MeanAbsDiff(sa, sb, kSampleBytes) == 0.0, "identical frames: zero difference");
        // The same frame with a larger pitch (the renderer's display buffer is 1024 pixels wide).
        std::vector<uint8_t> wide(size_t(1024) * 4 * fixture::H, 0xCD);
        for (int y = 0; y < fixture::H; ++y) std::memcpy(&wide[size_t(y) * 1024 * 4], &a[size_t(y) * fixture::W * 4], size_t(fixture::W) * 4);
        uint8_t sw[kSampleBytes];
        SampleFrame(wide.data(), 1024 * 4, sw);
        Check(MeanAbsDiff(sa, sw, kSampleBytes) == 0.0, "the pitch is honoured");
        for (int y = 60; y < 340; ++y) for (int x = 230; x < 720; ++x) b[(size_t(y) * fixture::W + x) * 4] ^= 8;
        SampleFrame(b.data(), size_t(fixture::W) * 4, sb);
        const double d = MeanAbsDiff(sa, sb, kSampleBytes);
        Check(d > 1.0 && d < 8.0, "a changed logo shows in the sample");
        Check(MeanAbsDiff(sa, sb, 0) == 255.0, "no data is never stable");
    }
}

// ------------------------------------------------------------------------------------------------ deflate
static void Deflate(const std::string& dir) {
    struct Case { std::string name; std::vector<uint8_t> data; };
    std::vector<Case> cases;
    cases.push_back({"empty", {}});
    cases.push_back({"one", {42}});
    cases.push_back({"zeros", std::vector<uint8_t>(300000, 0)});
    {
        std::vector<uint8_t> d(100000);
        uint32_t s = 1;
        for (auto& b : d) { s = fixture::Hash(s); b = uint8_t(s >> 11); }
        cases.push_back({"random", d});
        std::vector<uint8_t> big(200000);
        for (auto& b : big) { s = fixture::Hash(s); b = uint8_t(s >> 7); }
        cases.push_back({"random-stored-blocks", big});
    }
    {
        std::vector<uint8_t> d;
        const char* words[] = {"the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dogs. "};
        uint32_t s = 5;
        while (d.size() < 400000) { s = fixture::Hash(s); const char* w = words[s & 7]; d.insert(d.end(), w, w + std::strlen(w)); }
        cases.push_back({"text", d});
    }
    {
        std::vector<uint8_t> d(70000);
        for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(i % 251);
        cases.push_back({"counting", d});
        std::vector<uint8_t> e(500000);
        for (size_t i = 0; i < e.size(); ++i) e[i] = uint8_t((i * i >> 9) ^ (i >> 3));
        cases.push_back({"arith", e});
    }
    {  // a picture-like row structure: 961 bytes per row with indices that change slowly
        std::vector<uint8_t> d(961u * 544u);
        for (size_t y = 0; y < 544; ++y)
            for (size_t x = 0; x < 961; ++x) d[y * 961 + x] = x == 0 ? 0 : uint8_t((x / 3 + y / 7 + fixture::Hash(uint32_t(y * 961 + x)) % 3));
        cases.push_back({"indices", d});
    }
    std::string list;
    for (auto& c : cases) {
        std::vector<uint8_t> z(ZlibBound(c.data.size()));
        size_t n = 0;
        Check(ZlibCompress(c.data.data(), c.data.size(), z.data(), z.size(), n), "zlib compress succeeds");
        Check(n <= ZlibBound(c.data.size()), "within the bound");
        WriteFile(dir + "/" + c.name + ".raw", c.data.data(), c.data.size());
        WriteFile(dir + "/" + c.name + ".z", z.data(), n);
        list += c.name + "\n";
        std::printf("%s: %zu -> %zu\n", c.name.c_str(), c.data.size(), n);
    }
    WriteFile(dir + "/deflate-cases.txt", list.data(), list.size());
}

// ------------------------------------------------------------------------------------------------ png
static void Png(const std::string& dir) {
    struct Case { std::string name; int w, h; std::vector<uint8_t> rgb; bool dither; };
    std::vector<Case> cases;
    auto make = [&](const std::string& name, int w, int h, auto f, bool dither = true) {
        Case c{name, w, h, std::vector<uint8_t>(size_t(w) * h * 3), dither};
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = &c.rgb[(size_t(y) * w + x) * 3];
                f(x, y, p);
            }
        cases.push_back(std::move(c));
    };
    make("gradient", 200, 100, [](int x, int y, uint8_t* p) { p[0] = uint8_t(x * 255 / 199); p[1] = uint8_t(y * 255 / 99); p[2] = uint8_t(255 - x * 255 / 199); });
    make("noise", 64, 64, [](int x, int y, uint8_t* p) { const uint32_t h = fixture::Hash(uint32_t(y * 64 + x)); p[0] = uint8_t(h); p[1] = uint8_t(h >> 8); p[2] = uint8_t(h >> 16); });
    make("flat", 10, 10, [](int, int, uint8_t* p) { p[0] = 30; p[1] = 90; p[2] = 160; });
    make("two", 33, 17, [](int x, int y, uint8_t* p) { const bool a = (x + y) & 1; p[0] = a ? 250 : 4; p[1] = a ? 10 : 200; p[2] = a ? 128 : 64; });
    make("few", 128, 64, [](int x, int y, uint8_t* p) { const int k = (x / 4 + y / 2 * 5) % 200; p[0] = uint8_t(4 * (k % 50)); p[1] = uint8_t(4 * (k / 4)); p[2] = uint8_t(4 * ((k * 7) % 60)); });
    make("tiny", 1, 1, [](int, int, uint8_t* p) { p[0] = 200; p[1] = 100; p[2] = 50; });
    make("thin-tall", 1, 300, [](int, int y, uint8_t* p) { p[0] = uint8_t(y); p[1] = uint8_t(y / 2); p[2] = uint8_t(255 - y); });
    make("thin-wide", 300, 1, [](int x, int, uint8_t* p) { p[0] = uint8_t(x); p[1] = uint8_t(x / 2); p[2] = uint8_t(255 - x); });
    make("stripes", 320, 180, [](int x, int y, uint8_t* p) {
        const int v = int(127.5 + 127.5 * std::sin(x * 0.05) * std::cos(y * 0.07));
        p[0] = uint8_t(v); p[1] = uint8_t((x / 40) * 40); p[2] = uint8_t(255 - v);
    });
    make("flat-nodither", 20, 20, [](int x, int y, uint8_t* p) { p[0] = uint8_t(x * 12); p[1] = uint8_t(y * 12); p[2] = 77; }, false);
    std::string list;
    for (auto& c : cases) {
        std::vector<uint8_t> idx(size_t(c.w) * c.h), png(PngBound(c.w, c.h));
        Palette pal;
        size_t n = 0;
        Check(Quantize(c.rgb.data(), c.w, c.h, idx.data(), pal, c.dither), "quantize");
        Check(pal.count >= 1 && pal.count <= 256, "palette size");
        Check(EncodePng8(idx.data(), c.w, c.h, pal, png.data(), png.size(), n), "png encode");
        WriteFile(dir + "/" + c.name + ".rgb", c.rgb.data(), c.rgb.size());
        WriteFile(dir + "/" + c.name + ".png", png.data(), n);
        list += c.name + " " + std::to_string(c.w) + " " + std::to_string(c.h) + " " + (c.dither ? "1" : "0") + "\n";
        std::printf("%s: %dx%d, %d colours, %zu bytes\n", c.name.c_str(), c.w, c.h, pal.count, n);
    }
    WriteFile(dir + "/png-cases.txt", list.data(), list.size());
    // Failure paths that must not crash.
    Palette pal;
    pal.count = 1;
    uint8_t one = 0, out[16];
    size_t n = 0;
    Check(!EncodePng8(&one, 0, 1, pal, out, sizeof out, n), "zero width is refused");
    Check(!EncodePng8(&one, 1, 1, pal, out, sizeof out, n), "a too-small output buffer is refused");
}

// ------------------------------------------------------------------------------------------------ pictures
struct Collected {
    std::string dir;
    size_t sizes[kPictureCount] = {0, 0, 0, 0};
    int calls = 0;
};
static const char* const kNames[kPictureCount] = {"pic0", "bg", "startup", "icon0"};
static bool Collect(void* context, int which, const uint8_t* png, size_t length) {
    auto& c = *static_cast<Collected*>(context);
    ++c.calls;
    c.sizes[which] = length;
    if (!c.dir.empty()) WriteFile(c.dir + "/" + kNames[which] + ".png", png, length);
    return true;
}
static bool IsTextLike(const uint8_t* p) { return p[0] + p[1] + p[2] > 330 || p[2] < p[0] + 30; }

static void Synthetic(const std::string& dir) {
    const fixture::Logo logo;
    const auto title = fixture::MakeTitle(true, true, logo);
    Check(LooksLikeTitle(title.data(), 4), "the made-up title looks like a title");
    LogoBox box;
    Check(FindLogoBox(title.data(), 4, box) && box.left == logo.left && box.right == logo.right && box.top == logo.top && box.bottom == logo.bottom, "the logo box is found exactly");
    // Not titles.
    Check(!LooksLikeTitle(fixture::MakeTitle(false, true).data(), 4), "no logo: not a title");
    {   // the title camera passes over the planet: a glowing cyan horizon fills the lower part of the frame
        auto horizon = fixture::MakeTitle();
        for (int y = 350; y < fixture::H; ++y)
            for (int x = 0; x < fixture::W; ++x) { uint8_t* p = &horizon[(size_t(y) * fixture::W + x) * 4]; if (p[2] > p[0] + 35) { p[0] = 40; p[1] = 190; p[2] = 205; } }
        Check(LooksLikeTitle(horizon.data(), 4), "a cyan planet horizon below the logo: still the title");
    }
    { std::vector<uint8_t> black(kRgbaBytes, 0); Check(!LooksLikeTitle(black.data(), 4), "black frame: not a title"); }
    { std::vector<uint8_t> white(kRgbaBytes, 255); Check(!LooksLikeTitle(white.data(), 4), "white frame: not a title"); }
    Check(!LooksLikeTitle(fixture::MakeTitle(true, true, {20, 300, 50, 340}).data(), 4), "a logo off to the left: not a title");
    Check(!LooksLikeTitle(fixture::MakeTitle(true, true, {400, 560, 60, 130}).data(), 4), "a small logo: not a title");
    Check(!LooksLikeTitle(fixture::MakeTitle(true, true, {100, 860, 50, 380}).data(), 4), "a huge logo: not a title");
    {  // the stage: no sky down the sides
        auto stage = fixture::MakeTitle();
        for (int y = 0; y < 544; ++y) for (int x = 0; x < 100; ++x) { uint8_t* p = &stage[(size_t(y) * 960 + x) * 4]; p[0] = 90; p[1] = 140; p[2] = 60; }
        Check(!LooksLikeTitle(stage.data(), 4), "grass at the sides: not a title");
    }
    // The text goes and nothing else changes.
    std::vector<uint8_t> rgb(size_t(kW) * kH * 3), mask(size_t(kW) * kH);
    for (size_t i = 0; i < size_t(kW) * kH; ++i) { rgb[i * 3] = title[i * 4]; rgb[i * 3 + 1] = title[i * 4 + 1]; rgb[i * 3 + 2] = title[i * 4 + 2]; }
    const std::vector<uint8_t> before = rgb;
    CleanTitle(rgb.data(), mask.data());
    long textLike = 0, region = 0, changedOutside = 0, strokes = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const bool band = y >= kTextY0 - 4 && y < kTextY1 + 4 && x >= kTextX0 - 4 && x < kTextX1 + 4;
            const uint8_t* p = &rgb[(size_t(y) * kW + x) * 3];
            if (y >= kTextY0 && y < kTextY1 && x >= kTextX0 && x < kTextX1) { ++region; textLike += IsTextLike(p); strokes += (p[0] == 250 && p[1] == 250 && p[2] == 250) || (p[0] == 25 && p[1] == 25 && p[2] == 25); }
            if (!band && std::memcmp(p, &before[(size_t(y) * kW + x) * 3], 3)) ++changedOutside;
        }
    Check(changedOutside == 0, "nothing outside the text band changes (the logo and the rest of the sky are exact)");
    Check(strokes == 0, "no stroke or outline pixel of the text is left");
    Check(textLike * 100 < region * 3, "under 3 % of the text rectangle is bright or grey afterwards (the stars the sky itself has)");
    // Deterministic.
    {
        std::vector<uint8_t> again = before, mask2(size_t(kW) * kH);
        CleanTitle(again.data(), mask2.data());
        Check(again == rgb, "cleaning is deterministic");
    }
    Collected c;
    c.dir = dir;
    int ok = 0;
    Check(BuildPictures(title.data(), Collect, &c, &ok) && ok == 4 && c.calls == 4, "all four pictures are made");
    for (int i = 0; i < kPictureCount; ++i) Check(c.sizes[i] > 100, "each picture has content");
    // A failing sink does not stop the others.
    struct Flaky { int calls = 0; } flaky;
    auto sink = [](void* ctx, int, const uint8_t*, size_t) { return ++static_cast<Flaky*>(ctx)->calls != 2; };
    ok = 0;
    Check(BuildPictures(title.data(), sink, &flaky, &ok) && ok == 3 && flaky.calls == 4, "a refused picture is counted, the others still come");
    // A frame with no logo cannot make pictures.
    const auto empty = fixture::MakeTitle(false, true);
    Check(!BuildPictures(empty.data(), Collect, &c, &ok) && ok == 0, "no logo: no pictures, no crash");
    std::printf("synthetic: pic0 %zu, bg %zu, startup %zu, icon0 %zu bytes\n", c.sizes[0], c.sizes[1], c.sizes[2], c.sizes[3]);
}

static void Pictures(const std::string& capture, const std::string& dir) {
    std::vector<uint8_t> data(kRgbaBytes);
    std::ifstream in(capture, std::ios::binary);
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()));
    Check(size_t(in.gcount()) == data.size(), "the capture is 960x544 RGBA");
    Check(LooksLikeTitle(data.data(), 4), "the real capture looks like a title");
    LogoBox box;
    Check(FindLogoBox(data.data(), 4, box), "the real capture has a logo");
    std::printf("logo box %d %d %d %d\n", box.left, box.right, box.top, box.bottom);
    Collected c;
    c.dir = dir;
    int ok = 0;
    Check(BuildPictures(data.data(), Collect, &c, &ok) && ok == 4, "pictures made from the real capture");
    std::printf("pictures: pic0 %zu, bg %zu, startup %zu, icon0 %zu bytes\n", c.sizes[0], c.sizes[1], c.sizes[2], c.sizes[3]);
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "logic" && argc == 2) Logic();
    else if (mode == "deflate" && argc == 3) Deflate(argv[2]);
    else if (mode == "png" && argc == 3) Png(argv[2]);
    else if (mode == "synthetic" && argc == 3) Synthetic(argv[2]);
    else if (mode == "pictures" && argc == 4) Pictures(argv[2], argv[3]);
    else { std::fprintf(stderr, "usage: title_art logic | deflate <dir> | png <dir> | synthetic <dir> | pictures <capture> <dir>\n"); return 2; }
    std::printf("PASS: %s\n", mode.c_str());
    return 0;
}
