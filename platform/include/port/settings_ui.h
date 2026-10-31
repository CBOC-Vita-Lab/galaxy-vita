#pragma once
#include "port/player_settings.h"
#include "port/settings_font.h"
#include <vector>

namespace port::settings {
enum class OpenMode : uint8_t { ColdStart, Running };
enum class ExitIntent : uint8_t { Play, Exit, Return, Restart };
struct RunResult { ExitIntent intent; bool saved; };
struct Surface { uint32_t* pixels; unsigned width, height, stride; };
enum MenuKey : uint32_t { Up=1, Down=2, Left=4, Right=8, Accept=16, Back=32, Reset=64, PreviousPage=128, NextPage=256 };
struct UiInput { uint32_t menuHeld; uint64_t physicalHeld; controls::ControllerCaps caps; controls::ProfileId detectedProfile=controls::ProfileId::Handheld; };
struct UiHost {
    void* context;
    bool (*poll)(void*, UiInput&);
    bool (*present)(void*, const Surface&);
    uint64_t (*nowUs)(void*);
    void (*waitUntilUs)(void*, uint64_t);
};
struct LanguageCatalog { std::vector<std::string> names; };
struct Override { std::string key, rawValue, effectiveLabel; };
struct FeatureCaps { bool fixed30, remap; };
struct SettingsSession {
    OpenMode mode;
    Document document;
    Config active;
    LanguageCatalog languages;
    std::vector<Override> overrides;
    controls::ControllerCaps controller;
    FeatureCaps features;
    Surface surface;
    UiHost host;
    const char* settingsPath;
    const FileOps* files;
    const controls::ControlApi* controls;
    std::string statusMessage;
    FontSet fonts;
    controls::ProfileId detectedProfile=controls::ProfileId::Handheld;
    // Optional backdrop picture (opaque ABGR pixels, backdropW x backdropH, at most 960x544), drawn scaled to the screen
    // and darkened. Null: the plain gradient. The caller owns the pixels for the life of the session.
    const uint32_t* backdrop=nullptr;
    unsigned backdropW=0, backdropH=0;
    // Optional 960x544 scratch picture the menu keeps its unchanging parts in (backdrop, header band and heading), built on first
    // draw and again when the page changes, so a frame starts with one copy instead of the picture's per-pixel work. The caller
    // owns the pixels; null: everything is drawn every frame. (The array is written through this pointer by Draw.)
    uint32_t* layer=nullptr;
    mutable int layerPage=-1;
    // False when the HD texture pack (ux0:data/smg/hdtex.pack, or GXV_HDTEX_PATH) is not installed: the HD textures row says so.
    bool hdtexPack=true;
};
enum class Page : uint8_t { Display, Controls };
enum class Modal : uint8_t { None, Discard, Restart, Capture, Conflict, ButtonPicker, ResetProfile, Overrides };
struct UiState {
    Config working;
    const SettingsSession* session = nullptr;
    Page page = Page::Display;
    Modal modal = Modal::None;
    unsigned row = 0, modalChoice = 0, bindingSlot = 0, picker = 0;
    uint32_t previousMenu = 0, repeatKey = 0;
    uint64_t repeatAt = 0, changedAt = 0, candidate = 0;
    bool armed = false, captureArmed = false, connected = false, done = false;
    controls::ControllerCaps controller;
    controls::ProfileId detectedProfile=controls::ProfileId::Handheld;
    bool saveRequested = false;
    ExitIntent pendingIntent = ExitIntent::Exit;
    RunResult result{ExitIntent::Exit, false};
    controls::ProfileMapping pendingMapping;
    std::string action, candidateButton, conflictAction, message;
};
void Begin(UiState&, const SettingsSession&);
void Step(UiState&, const UiInput&, uint64_t);
void Draw(const UiState&, const SettingsSession&, Surface, uint64_t);
// The Display page's rows, in order, and what each shows. The text is all static or built in the view's own buffer: drawing a
// frame allocates nothing.
constexpr unsigned kDisplayRows=13;
enum DisplayRow : unsigned { RowPreset, RowRate, RowDynamic, RowSharper, RowMario, RowAntiAliasing, RowSharpness, RowHdTextures, RowEffects, RowDepthOfField, RowCorePlan, RowMotion, RowLanguage };
struct RowView { const char* label=""; char value[72]={}; const char* help1=""; const char* help2=""; };
void DisplayRowView(const UiState&, unsigned row, RowView&);
// Controls page rows: row 0 the profile, then one per action, then the pointer-aiming option.
void ControlsRowView(const UiState&, unsigned row, RowView&);
// Settings rows of the current page; the command rows (save, restart or exit, cancel) follow them.
unsigned SettingRows(const UiState&);
void CommandView(const UiState&, unsigned index, const char*& label, const char*& help1, const char*& help2);
struct DrawAudit { unsigned textRuns=0, truncatedRuns=0; int minX=960, minY=544, maxX=0, maxY=0; };
struct TextExtent { int advance=0, minX=0, maxX=0, height=0; };
TextExtent MeasureMenuText(const SettingsSession&,const std::string&,bool small=false,unsigned scale=1);
void DrawChecked(const UiState&, const SettingsSession&, Surface, uint64_t, DrawAudit&);
RunResult Run(SettingsSession&);
unsigned RowCount(const UiState&);
bool Dirty(const UiState&);
}
