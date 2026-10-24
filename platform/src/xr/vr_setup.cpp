// The setup screen: shown instead of the game when its files are not where
// the app looks for them (the first start, or files moved).  A panel in
// front of the player explains what the app needs and lists the folders on
// the headset that hold the game's files, found by a search on a thread:
// the app's own storage folder and, once the player lets the app read all
// files, the headset's shared storage (Download, Documents, a folder of
// their own).  Aim at a folder that is ready and press A or the trigger to
// play from it; xr_app.cpp then boots the game and keeps the choice
// (game_path in petari_vr.ini).  Files extracted from the disc but not
// converted by tools/cook/cook.py show as such: the port cannot read them.
// Files converted by an older converter, without the data it now also takes
// from main.dol, show as to convert again.
#include <GLES3/gl32.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port/heap_routing.h"
#include "port/port.h"
#include "ui_canvas.h"
#include "vr_renderer.h"

namespace {

using ui::Canvas;
using ui::Color;
using ui::kFontLarge;
using ui::kFontSmall;
using ui::rgb;

const int kTexW = 1280, kTexH = 940;
const float kWidthM = 1.36f;  // metres; height from the texture's aspect
// Straight ahead, a little below the eyes.
const xm::Vec3 kCenter{0.0f, -0.12f, -1.45f};

// The folders listed and their buttons.
const int kMaxRows = 5;
const float kListY = 390.0f, kRowH = 78.0f;
const float kButtonsY = 850.0f;

enum Control { kNone, kUse0, kRescan = kUse0 + kMaxRows, kAccess };

struct Found {
    std::string path;
    bool ready;     // converted for the port (else extracted but not converted)
    bool outdated;  // converted by an older converter: to convert again
    bool unknown;   // converted, but not a disc the port knows

    // Why the folder cannot be played from.
    const char* problem() const { return outdated ? "Convert again" : unknown ? "Unknown disc" : "Not converted"; }
};

// Setup state, shared with the search thread.
std::mutex sLock;
std::vector<Found> sFound;
std::atomic<bool> sSearching{false};
std::atomic<unsigned> sSearchCount{0};
std::atomic<bool> sAccess{false};  // the app may read all files

std::atomic<bool> sActive{false};
std::string sAppDir, sTried;
std::string sChoice;
bool sChosen = false, sAccessAsked = false;

// Panel.
bool sReady = false;
GLuint sTex = 0;
Canvas sCanvas;
std::vector<uint32_t> sUpload;
xm::Vec3 sRight, sUp, sNormal;
xm::Mat4 sModel;
bool sDirty = true, sTick = false;
Control sHover = kNone, sPressed = kNone;
bool sClickDown = false, sOwnsClick = false, sPointerOnPanel = false;
float sPointerPx = 0.0f, sPointerPy = 0.0f;
unsigned sShownSearch = ~0u;
bool sShownSearching = false, sShownAccess = false;

float heightM() { return kWidthM * kTexH / kTexW; }

void setupPose() {
    sNormal = xm::normalize(kCenter * -1.0f);
    sRight = xm::normalize(xm::cross(xm::Vec3{0.0f, 1.0f, 0.0f}, sNormal));
    sUp = xm::cross(sNormal, sRight);
    xm::Mat4 m = xm::Mat4::identity();
    xm::Vec3 axes[3] = {sRight * kWidthM, sUp * heightM(), sNormal};
    for (int c = 0; c < 3; c++) {
        m.at(0, c) = axes[c].x;
        m.at(1, c) = axes[c].y;
        m.at(2, c) = axes[c].z;
    }
    m.at(0, 3) = kCenter.x;
    m.at(1, 3) = kCenter.y;
    m.at(2, 3) = kCenter.z;
    sModel = m;
}

// ---------------------------------------------------------------------------
// The search
// ---------------------------------------------------------------------------
bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void scanDir(const std::string& dir, int depth, bool skipAndroid, std::vector<Found>& out, int64_t deadline) {
    bool ready = false, outdated = false, unknown = false;
    if (vr::isGameFolder(dir, &ready, &outdated, &unknown)) {
        out.push_back({dir, ready, outdated, unknown});
        return;
    }
    if (depth <= 0 || out.size() >= 16 || port_host_time_ns() > deadline) {
        return;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    std::vector<std::string> subs;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' || (e->d_type != DT_DIR && e->d_type != DT_UNKNOWN && e->d_type != DT_LNK)) continue;
        // Other apps' folders cannot be read (and the app's own is searched on its own).
        if (skipAndroid && strcmp(e->d_name, "Android") == 0) continue;
        subs.push_back(e->d_name);
        if (subs.size() >= 300) break;
    }
    closedir(d);
    std::sort(subs.begin(), subs.end());
    for (const std::string& s : subs) {
        std::string p = dir + "/" + s;
        if (isDir(p)) {
            scanDir(p, depth - 1, false, out, deadline);
        }
    }
}

void search() {
    if (sSearching.exchange(true)) {
        return;
    }
    std::string appDir = sAppDir;
    bool access = sAccess.load();
    PortHostAllocScope hostAlloc;
    std::thread([appDir, access] {
        PortHostAllocScope scope;
        std::vector<Found> found;
        int64_t deadline = port_host_time_ns() + 4000000000ll;
        scanDir(appDir, 3, false, found, deadline);
        if (access) {
            scanDir("/storage/emulated/0", 4, true, found, deadline);
        }
        // Ready folders first.
        std::stable_sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.ready > b.ready; });
        port_log("setup: %zu game folders found (all files access %s)", found.size(), access ? "on" : "off");
        for (const Found& f : found) {
            port_log("setup:   %s (%s)", f.path.c_str(),
                     f.ready ? "ready" : f.outdated ? "converted by an older version" : f.unknown ? "unknown disc" : "not converted");
        }
        {
            std::lock_guard<std::mutex> lock(sLock);
            sFound = std::move(found);
        }
        sSearchCount.fetch_add(1);
        sSearching.store(false);
    }).detach();
}

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------
// A path as shown: the storage root as "Headset storage", and long paths
// shortened from the left.
std::string shownPath(const std::string& path) {
    std::string p = path;
    const char* roots[] = {"/storage/emulated/0/", "/sdcard/"};
    for (const char* r : roots) {
        if (p.compare(0, strlen(r), r) == 0) {
            p = "Headset storage/" + p.substr(strlen(r));
            break;
        }
    }
    while (p.size() > 8 && ui::textWidth(kFontSmall, p.c_str()) > 860.0f) {
        size_t slash = p.find('/', p.compare(0, 3, "...") == 0 ? 4 : 1);
        if (slash == std::string::npos) break;
        p = "..." + p.substr(slash);
    }
    return p;
}

void drawButton(Canvas& c, float x0, float y0, float x1, float y1, const char* label, Control ctl, bool primary) {
    const Color accent = rgb(86, 160, 255);
    Color fill = sPressed == ctl ? accent : sHover == ctl ? (primary ? rgb(116, 180, 255) : rgb(84, 93, 116)) : (primary ? accent : rgb(44, 49, 62));
    c.roundRect(x0, y0, x1, y1, (y1 - y0) * 0.5f, fill);
    c.text((x0 + x1) * 0.5f, (y0 + y1) * 0.5f + 10.0f, kFontSmall, label, rgb(240, 243, 248), 1);
}

void drawPanel() {
    Canvas& c = sCanvas;
    c.init(kTexW, kTexH);
    c.roundRect(0, 0, kTexW, kTexH, 36.0f, rgb(78, 86, 106, 0.97f));
    c.roundRect(3, 3, kTexW - 3, kTexH - 3, 33.0f, rgb(22, 25, 33));
    c.text(56, 86, kFontLarge, "GalaxyQuest", rgb(255, 255, 255));
    c.text(56, 136, kFontSmall, "The game's files are needed", rgb(140, 180, 255));
    float y = c.textWrapped(56, 196, kTexW - 112, 38, kFontSmall,
                            "This app plays Super Mario Galaxy from the files of your own disc, converted on a computer with "
                            "tools/cook/cook.py (see the project's README). Copy the converted folder, the one holding sys and "
                            "files, to the headset, then choose it below.",
                            rgb(196, 202, 216));
    (void)y;
    c.roundRect(56, 336, kTexW - 56, 338, 1.0f, rgb(58, 63, 78));

    std::vector<Found> found;
    {
        std::lock_guard<std::mutex> lock(sLock);
        found = sFound;
    }
    bool searching = sSearching.load();
    if (found.empty()) {
        const char* msg = searching ? "Searching the headset..." : "No game files found on the headset yet.";
        c.text(56, kListY + 12, kFontSmall, msg, rgb(230, 234, 242));
        if (!searching) {
            std::string where = "The app looks in its own folder, " + shownPath(sAppDir) +
                                (sAccess.load() ? ", and in the rest of the headset's storage." :
                                                  ". Allow access to all files to have it look in the rest of the headset's storage too.");
            c.textWrapped(56, kListY + 62, kTexW - 112, 36, kFontSmall, where.c_str(), rgb(140, 147, 163));
        }
    }
    for (int i = 0; i < (int)found.size() && i < kMaxRows; i++) {
        float top = kListY - 34.0f + i * kRowH;
        c.roundRect(40, top, kTexW - 40, top + kRowH - 10, 16.0f, rgb(30, 34, 45));
        std::string p = shownPath(found[i].path);
        c.text(64, top + 44, kFontSmall, p.c_str(), rgb(230, 234, 242));
        if (found[i].ready) {
            drawButton(c, kTexW - 250, top + 8, kTexW - 60, top + kRowH - 18, "Play", (Control)(kUse0 + i), true);
        } else {
            c.text(kTexW - 60, top + 44, kFontSmall, found[i].problem(), rgb(255, 170, 110), 2);
        }
    }
    if (!found.empty() && searching) {
        c.text(56, kButtonsY - 70, kFontSmall, "Still searching...", rgb(140, 147, 163));
    }

    drawButton(c, 56, kButtonsY - 36, 400, kButtonsY + 36, "Search again", kRescan, false);
    if (sAccess.load()) {
        c.text(kTexW - 56, kButtonsY + 10, kFontSmall, "All files access: allowed", rgb(140, 147, 163), 2);
    } else {
        drawButton(c, 440, kButtonsY - 36, kTexW - 56, kButtonsY + 36, "Allow access to all files", kAccess, false);
    }
    sShownSearch = sSearchCount.load();
    sShownSearching = searching;
    sShownAccess = sAccess.load();
}

void upload() {
    sUpload.resize((size_t)kTexW * kTexH);
    for (int y = 0; y < kTexH; y++) {
        memcpy(&sUpload[(size_t)y * kTexW], &sCanvas.px[(size_t)(kTexH - 1 - y) * kTexW], kTexW * 4);
    }
    glBindTexture(GL_TEXTURE_2D, sTex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kTexW, kTexH, GL_RGBA, GL_UNSIGNED_BYTE, sUpload.data());
    glGenerateMipmap(GL_TEXTURE_2D);
}

void refresh() {
    if (sDirty || sShownSearch != sSearchCount.load() || sShownSearching != sSearching.load() || sShownAccess != sAccess.load()) {
        drawPanel();
        upload();
        sDirty = false;
    }
}

Control controlAt(float x, float y) {
    std::lock_guard<std::mutex> lock(sLock);
    for (int i = 0; i < (int)sFound.size() && i < kMaxRows; i++) {
        float top = kListY - 34.0f + i * kRowH;
        if (sFound[i].ready && x >= kTexW - 260 && x <= kTexW - 50 && y >= top && y <= top + kRowH - 10) return (Control)(kUse0 + i);
    }
    if (x >= 48 && x <= 408 && y >= kButtonsY - 44 && y <= kButtonsY + 44) return kRescan;
    if (!sAccess.load() && x >= 432 && x <= kTexW - 48 && y >= kButtonsY - 44 && y <= kButtonsY + 44) return kAccess;
    return kNone;
}

}  // namespace

namespace vr {

bool isGameFolder(const std::string& dir, bool* ready, bool* outdated, bool* unknown) {
    struct stat st;
    if (stat((dir + "/sys/fst.bin").c_str(), &st) != 0 || !S_ISREG(st.st_mode) || !isDir(dir + "/files")) {
        return false;
    }
    // The converted archives start "CRAR"; the disc's own are Yaz0-compressed.
    char magic[4] = {};
    FILE* f = fopen((dir + "/files/ObjectData/Coin.arc").c_str(), "rb");
    bool converted = f && fread(magic, 1, 4, f) == 4 && memcmp(magic, "CRAR", 4) == 0;
    if (f) fclose(f);
    // The data the converter takes from main.dol (platform/src/dvd/dol_data.cpp).
    bool dolData = true;
    for (const char* name : {"ErrorMessageArchive.arc", "StoryEvent.bcsv", "GalaxyID.bcsv"}) {
        dolData = dolData && stat((dir + "/sys/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode);
    }
    // The disc's region, which tells the game where its texts are.
    bool known = port_dvd_identify(dir.c_str()) != nullptr;
    if (ready) *ready = converted && dolData && known;
    if (outdated) *outdated = converted && !dolData;
    if (unknown) *unknown = converted && dolData && !known;
    return true;
}

void setupStart(const std::string& appDir, const std::string& tried) {
    sAppDir = appDir;
    sTried = tried;
    if (!sReady) {
        ui::initFonts();
        setupPose();
        glGenTextures(1, &sTex);
        glBindTexture(GL_TEXTURE_2D, sTex);
        int levels = 1 + (int)floorf(log2f((float)kTexW));
        glTexStorage2D(GL_TEXTURE_2D, levels, GL_RGBA8, kTexW, kTexH);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        sReady = true;
    }
    port_log("setup: no game files in %s; the setup screen is shown", tried.c_str());
    sActive.store(true);
    sDirty = true;
    search();
}

bool setupActive() { return sActive.load(); }

void setupSetStorageAccess(bool granted) {
    if (sAccess.exchange(granted) != granted) {
        port_log("setup: all files access %s", granted ? "allowed" : "not allowed");
        if (granted && sActive.load()) {
            search();  // the rest of the storage can be searched now
        }
    }
}

float setupPointer(xm::Vec3 origin, xm::Vec3 dir, bool clickDown) {
    if (!sActive.load()) return 0.0f;
    float hitT = 0.0f, px = 0.0f, py = 0.0f;
    float dn = xm::dot(dir, sNormal);
    if (dn < -1e-4f) {
        float t = xm::dot(kCenter - origin, sNormal) / dn;
        xm::Vec3 p = origin + dir * t - kCenter;
        float u = xm::dot(p, sRight) / kWidthM + 0.5f, v = xm::dot(p, sUp) / heightM() + 0.5f;
        if (t > 0.0f && u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
            hitT = t;
            px = u * kTexW;
            py = (1.0f - v) * kTexH;
        }
    }
    Control over = hitT > 0.0f ? controlAt(px, py) : kNone;
    sPointerOnPanel = hitT > 0.0f;
    sPointerPx = px;
    sPointerPy = py;
    if (over != sHover) {
        sHover = over;
        sDirty = true;
        if (over != kNone) sTick = true;
    }
    if (clickDown && !sClickDown && hitT > 0.0f) {
        sOwnsClick = true;
        sPressed = over;
        sDirty = true;
        if (over >= kUse0 && over < kUse0 + kMaxRows) {
            std::lock_guard<std::mutex> lock(sLock);
            int i = over - kUse0;
            if (i < (int)sFound.size() && sFound[i].ready) {
                sChoice = sFound[i].path;
                sChosen = true;
                port_log("setup: chose %s", sChoice.c_str());
            }
        } else if (over == kRescan) {
            port_log("setup: search again");
            search();
        } else if (over == kAccess) {
            port_log("setup: asking for all files access");
            sAccessAsked = true;
        }
    }
    if (!clickDown) {
        if (sPressed != kNone) sDirty = true;
        sPressed = kNone;
        sOwnsClick = false;
    }
    sClickDown = clickDown;
    return hitT;
}

bool setupTakeTick() {
    bool t = sTick;
    sTick = false;
    return t;
}

bool setupTakeChoice(std::string* folder) {
    if (!sChosen) return false;
    sChosen = false;
    *folder = sChoice;
    sActive.store(false);
    return true;
}

bool setupTakeAccessRequest() {
    bool asked = sAccessAsked;
    sAccessAsked = false;
    return asked;
}

void setupLayerSize(int* width, int* height) {
    *width = kTexW;
    *height = kTexH;
}

UiLayer setupLayer() {
    UiLayer l;
    l.visible = sActive.load() && sReady;
    l.changed = l.visible;  // redrawn each frame it is up (the reticle)
    xm::Mat4 basis = xm::Mat4::identity();
    xm::Vec3 axes[3] = {sRight, sUp, sNormal};
    for (int c = 0; c < 3; c++) {
        basis.at(0, c) = axes[c].x;
        basis.at(1, c) = axes[c].y;
        basis.at(2, c) = axes[c].z;
    }
    l.orientation = xm::quatFromMatrix(basis);
    l.position = kCenter;
    l.width = kWidthM;
    l.height = heightM();
    return l;
}

void setupDrawLayer() {
    if (!sReady) return;
    refresh();
    drawOverlayQuad(sTex, xm::scale(2.0f), 1.0f);
    if (sPointerOnPanel) {
        const float r = 16.0f;
        drawReticle2d(sPointerPx / kTexW * 2.0f - 1.0f, 1.0f - sPointerPy / kTexH * 2.0f, r / kTexW * 2.0f, r / kTexH * 2.0f);
    }
}

void setupDraw(const xm::Mat4& viewProj) {
    if (!sReady || !sActive.load()) return;
    refresh();
    drawOverlayQuad(sTex, viewProj * sModel, 1.0f);
}

}  // namespace vr
