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
struct DrawAudit { unsigned textRuns=0, truncatedRuns=0; int minX=960, minY=544, maxX=0, maxY=0; };
struct TextExtent { int advance=0, minX=0, maxX=0, height=0; };
TextExtent MeasureMenuText(const SettingsSession&,const std::string&,bool small=false,unsigned scale=1);
void DrawChecked(const UiState&, const SettingsSession&, Surface, uint64_t, DrawAudit&);
RunResult Run(SettingsSession&);
unsigned RowCount(const UiState&);
bool Dirty(const UiState&);
}
