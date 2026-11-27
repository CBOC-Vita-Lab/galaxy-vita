// Galaxy 3.0 multiplayer: what the multiplayer panels draw, as plain data (owner: ui). Pure code: no game, no SDK.
//
// MpMenuSequence fills a Model (title, rows, buttons...); Build turns it into coloured triangles and runs of characters in the
// game's two fonts (message font, picture font), in "square units" of the picture (y down; the height is the game's screen height,
// 456, the width that times the aspect: 810.7 at 16:9). MpPanelDraw.cpp draws the plan with GX and nw4r::ut::CharWriter, the way
// platform/src/port/settings_panel.cpp does; tools/mp/panel_preview draws the same plan into PNGs on the host.
// The look is the game's windows (platform/src/port/settings_panel_plan.cpp): deep indigo, a light rim, white words with a soft
// shadow, yellow for what is selected.
#pragma once

#include <stdint.h>

#include "Game/MP/MpCharColor.hpp"

namespace MpPanel {

struct Rgba { uint8_t r, g, b, a; };
struct Vertex { float x, y; Rgba c; };
// FONT_DISPLAY: the game's MenuFont64 (a big face for the invite code); an entry with no metrics means "not there": the writer falls back to FONT_MESSAGE
enum Font : uint8_t { FONT_MESSAGE = 0, FONT_PICTURE = 1, FONT_DISPLAY = 2, FONT_COUNT = 3 };

struct FontMetrics {
    float height = 1.0f, baseline = 1.0f;
    void* context = nullptr;
    float (*advance)(void* context, uint16_t code) = nullptr;   // in the font's pixels
    bool (*has)(void* context, uint16_t code) = nullptr;
};

struct TextRun {
    Font font;
    float x, y, scaleX, scaleY;   // left of the first character, top of the line; font pixels -> square units
    Rgba color;
    int start, length;            // in Plan::text
};

// ---------------------------------------------------------------- the model
enum Icon : uint8_t {
    ICON_NONE = 0, ICON_MARIO, ICON_LUIGI, ICON_TOAD, ICON_COSMIC, ICON_YELLOW_TOAD, ICON_COSMIC_LUIGI,   // ICON_MARIO + MpLook
    ICON_DOT_ONLINE, ICON_DOT_ROOM, ICON_DOT_OFFLINE, ICON_FILE, ICON_STAMP,
    ICON_SRC_WIFI, ICON_SRC_NEARBY, ICON_SRC_ONLINE   // flow: where a room of the Join list is (Wi-Fi arcs, two consoles, a globe)
};
enum Mark : uint16_t {
    MARK_CROWN = 1 << 0,        // the host
    MARK_VERIFIED = 1 << 1,     // leaderboard: a verified time
    MARK_BAD = 1 << 2,          // a cross: incompatible room
    MARK_YOU = 1 << 3,          // the local player (name in yellow)
    MARK_LAG = 1 << 4,          // no packet for a while
    MARK_ARROWS = 1 << 5,       // the value can be changed with left / right (arrows shown when selected)
    MARK_DISABLED = 1 << 6,     // dim, cannot be chosen
    MARK_GOLD = 1 << 7,         // first place
    MARK_HEADER = 1 << 8,       // a picker row above the list (galaxy / star)
    MARK_STATIC = 1 << 9,       // a list row that cannot be selected (leaderboard lines)
    MARK_ARRIVED = 1 << 10,     // flow look: a head that appeared this frame (KIND_HEADS: it pops in)
    MARK_NAV = 1 << 12,         // settingsnative: the row opens a page: a ">" at its right end
    MARK_CHOICE = 1 << 11,      // lobby: a card's live choice (the Star Race results card's Race Again / Next Star / Leave Race): a big row, at least kListRowH high
};

constexpr int kMaxRows = 8;
constexpr int kLabel = 40, kValue = 28, kCol = 10;
// uinative: lists show few, big rows (kListRowH square units, words about 27 high): kListRows at a time, a longer list turns its
// pages (the star list's page number and its round up / down buttons). The leaderboard shows its two pickers and kBoardRows times.
constexpr int kListRows = 3, kBoardRows = 3;   // uxfix #8: touch: a row is 67 units (80 px) high, so three to a page
constexpr float kListRowH = 67.0f;   // (uxfix #8: a finger needs 80 px; was 44)
constexpr float kTouchMin = 67.0f, kGuideBandH = 64.0f;

struct Row {
    char16_t lead[kCol];        // a short first column (rank, place), or empty
    char16_t label[kLabel];
    char16_t value[kValue];     // right-aligned value, or empty
    uint8_t icon;
    uint16_t marks;
    int8_t ping;                // -1 none, else 0..4 bars
    int8_t owner;               // character select: the slot that has this character (-1 nobody)
};

enum Kind : uint8_t {
    KIND_LIST = 0,      // a list in the game's message window (SysInfoWindowMini's frame, resized) under the title band
    KIND_WAITING = 1,   // overlay: a window alone (race / scene barrier waiting screen)
    KIND_RESULTS = 2,   // overlay: a window alone (race results card)
    KIND_BUSY = 3,      // a small window with a turning star under the title band
    KIND_BUTTONS = 4,   // the game's big buttons (RetryButton layout, one per choice) under the title band
    KIND_CHARS = 5,     // character select: four big floating character heads (MiiIcon layout, the file select's icon picker)
    KIND_GRID = 6,      // chat: the Messages page, a grid of phrase tiles in a message window (Model::cells; cursor = cell)
    KIND_HEADS = 7,     // flow look: a row of up to four spinning character heads (the 3D heads), each player's name under it in the
                        // character's colour (rows: icon = ICON_MARIO + look, label = name, owner = slot or -1, MARK_YOU / MARK_CROWN /
                        // MARK_DISABLED = waiting); rows [headCount, rowCount) are big buttons under them (at most three)
};

struct Button { uint8_t pic; char16_t label[20]; };
// chat: one tile of the Messages page (KIND_GRID). `wide`: a full-width tile under the grid ("Write a Message").
constexpr int kMaxCells = 16, kCellText = 28;
struct Cell { char16_t label[kCellText]; bool wide; };

struct Model {
    Kind kind = KIND_LIST;
    float alpha = 1.0f;
    float width = 0.0f;         // race: an overlay window's width (0 = the usual 560)
    float lift = 0.0f;          // race: an overlay window's middle, layout y (0 = the picture's middle; the head start line sits lower)
    char16_t title[48];
    char16_t subtitle[64];      // under the title (connection, mode)
    int tabCount = 0, tab = 0;
    char16_t tabs[3][20];
    char16_t bigLabel[40];      // above the big text ("Room Code"; the gathering screen's "Tell your friend this code.")
    char16_t big[24];           // the room code / the countdown / netx: the host's address ("203.0.113.7:41100"); empty = none
    Row rows[kMaxRows];
    int rowCount = 0;
    int cursor = -1;            // selected row, -1 none
    bool moreAbove = false, moreBelow = false;
    char16_t status[80];        // centred in the list area (Loading..., empty list)
    char16_t hint[80];          // the selected row's description, above the buttons
    Button buttons[4];
    int buttonCount = 0;
    float spinner = -1.0f;      // >= 0: a turning star (busy), the angle in turns
    float pulse = 0.0f;         // 1 just after the cursor moved, easing to 0
    float progress = -1.0f;     // >= 0: a progress bar 0..1 (busy)
    bool selectedArrows = false;   // settingsnative: the selected row's arrows show in the game's look too (it has no cursor furniture)
    int page = 0, pages = 0;    // uinative: a list longer than kListRows: the page shown (0..) of `pages` (0 = no pages)
    uint16_t screenKey = 0;     // uinative: changes when the screen changes (the big buttons end and the new ones appear)
    int8_t decided = -1;        // uinative: the big button just chosen on the screen before (it plays the game's decide)
    int8_t cursors[4] = {-1, -1, -1, -1};   // character select: each player's character (row), by slot; -1 none
    int8_t localSlot = -1;                  // character select: the local player's slot (its cursor is the selection)
    Cell cells[kMaxCells];                  // chat: the Messages page's tiles (KIND_GRID), reading order
    int cellCount = 0, cellCols = 3;
    bool dialogUp = false;                  // uinative3: a native message window is open over the menu (no hand under its dim)
    // flow look (LOOK.md): the file select's look. LOOK_GAME: no title band and no breadcrumb (title / subtitle / hint are not drawn),
    // the screen's one question or line in the game's own bottom message (line), the button highlight is the cursor (no glove)
    uint8_t look = 0;
    char16_t line[96];
    int headCount = 0;                      // KIND_HEADS: rows [0, headCount) are heads, the rest big buttons
    bool zoomRow = false;                   // mppause: the session menu's first screen on a handheld: the port's zoom row (and the one-line "keeps running" note) is drawn
                                            // by platform/src/port/settings_panel.cpp in the pause menu's place, kZoomRowTop down; the buttons end above it, no line
};

void clear(Model* m);
void copy(char16_t* dst, int cap, const char16_t* src);
bool copyChanged(char16_t* dst, int cap, const char16_t* src);   // copy; true if it differed

// ---------------------------------------------------------------- toasts
struct Toast { char16_t text[96]; float alpha; uint8_t icon; };
constexpr int kMaxToasts = 3;

// ---------------------------------------------------------------- the scene: the game's own layouts, placed
// The menus are made of real layouts (LayoutData/*.arc): PauseMenu (the dimmed background and the title band with its title and
// scenario line), RetryButton (one big button each), SysInfoWindowMini (its message-window frame, resized, for lists, waiting,
// results and toasts) and IconMario (Mario / Luigi heads). The GX plan only adds what no layout shows: list rows, values, button
// hints, the spinner. Coordinates of uses: layout units of the 608 x 456 screen layout, origin in the middle, y up (the root
// pane's translation); sizes of windows in the window pane's own units.
// uinative: the game's cursor and list furniture, placed the same way:
//   USE_POINTER  the Star Pointer's hand (DPDPointer, its HandPointer pane), the fingertip at (x, y), w = scale, h = turn (degrees)
//   USE_ARROWS   the Mii select's blue arrows (MiiSelect, its Arrow pane) either side of an option's value: tips at x - w and x + w
//   USE_SCROLL   the star list's page number and round up / down buttons (AllStarList): the column at x, the buttons at y + h and
//                y - h, the number at y; state bit 0 the up button, bit 1 the down button; text = the page ("2/3")
enum UseKind : uint8_t { USE_FRAME = 0, USE_BUTTON, USE_WINDOW, USE_HEAD, USE_CHAR, USE_POINTER, USE_ARROWS, USE_SCROLL,
                         USE_LINE   /* flow look: the game's bottom message (InformationMessage), text = the line */ };
enum Look : uint8_t { LOOK_PANEL = 0, LOOK_GAME = 1 };
enum ButtonState : uint8_t { BUTTON_IDLE = 0, BUTTON_SELECTED = 1, BUTTON_DISABLED = 2 };
constexpr float kBandLift = 30.0f;   // the frame's title band (PauseMenu's PicStagebase) sits this much higher than in the pause menu
constexpr float kLineLift = 10.0f;   // uinative: and its scenario line this much closer to the title (a tighter band)
constexpr float kTitleDrop = 12.0f;  // uinative: the title this much lower than the band's own place (clear of the picture's top)
constexpr float kBandShrink = 28.0f; // uinative: with no line under the title the band rises this much more (a slimmer band)
constexpr float kTitleAlone = 40.0f; // uinative: and the title, alone, sits this much lower in it (the band's middle)
// uinative: the pause menu's lower band (PicStagebase1, the counters' band) carries the button guide, kFootDrop lower than in the
// pause menu so a strip of it shows. Layout y (up) of the bands' inner edges and of the guide's line; the menus' content lies
// between kContentTop and kContentBottom.
constexpr float kFootDrop = 54.0f;
constexpr float kBandBottom = 77.0f + kBandLift;     // 107: the title band's lower edge
constexpr float kFootTop = -123.0f - kFootDrop;      // -177: the guide band's upper edge
constexpr float kFootLine = -209.0f;                 // the guide's line
constexpr float kContentTop = kBandBottom - 8.0f, kContentBottom = kFootTop + 6.0f;
constexpr float kContentTopAlone = kContentTop + kBandShrink;   // (the slimmer band's)
constexpr uint8_t kFrameDim = 1, kFrameFoot = 2, kFrameNoLine = 4;   // LayoutUse::state of a frame; its w / h: the title's / line's scale
// uinative: a screen of big buttons with no title (the entry screen after the title: Single Player / Multiplayer) has no title
// band: the buttons stand in the middle of the picture at the pause menu's size, the guide band (if any) under them
constexpr uint8_t kFrameNoTitle = 8;
constexpr float kContentTopBare = 214.0f;
// flow look (LOOK_GAME): no bands. The line's window (InformationWindow's upper window) hangs from kLineY (its top edge: the box
// grows down with a second line, ~63 units for one line, ~92 for two: seen in the first game run, 12 Nov, where a window centred
// lower ran off the picture and over the guide); the content's room ends kLookBottom (over it); the guide stays on its line under it
// (uxfix B2, 12 Nov game pictures: a two-line message ended on the guide's words: kLineY 6 higher the guide 6 lower)
constexpr float kZoomRowTop = -118.0f;   // mppause: the pause menu's lower button ends here (settings_panel_plan.cpp: its panel starts 4 under, 40 high in the compact form, ends -162, over the guide at kFootTop)
constexpr float kLineY = -90.0f, kLookBottom = -88.0f, kLineOneH = 63.0f, kLineTwoH = 92.0f;
// (menus5, owner 12 Nov: "press to start should be centered"): the line's text is centred in the band. USE_LINE's `state` is
// kLineCentred. The game's own layout (LayoutData/InformationWindow.arc, read 13 Nov): the upper window InfoWinU is 566 wide on the
// root's middle; its text box InfConfirmU is 480 wide, top-left origin, under ConfirmU at -256, left-aligned (the original left-aligns
// both windows). Centring inside that box would sit 16 left of the band's middle, so the box is moved right by kLineBoxShift.
constexpr uint8_t kLineCentred = 1;
constexpr float kLineBandCentreX = 0.0f, kLineBoxLeftX = -256.0f, kLineBoxW = 480.0f, kLineBoxShift = 16.0f;
constexpr float lineTextCentreX() { return kLineBoxLeftX + kLineBoxShift + kLineBoxW * 0.5f; }   // (a centred line's bounding box centre; every line of it is centred)
// (and with a line but no heads, the content stands in the middle of the picture, not hung from its top: kLookTop, owner 12 Nov:
// "the falls question's and Who Can I Play With?'s empty middle")
constexpr float kLookTop = 150.0f;
// (menus2, 12 Nov: such content is centred on the picture's middle, not on the room above the line: two buttons span about +-83 around it,
// clear of the line's window at kLineY; the first game pictures showed it high with an empty middle)
constexpr float kLookMid = 6.0f;
// uinative4: the big-button screens' band: a title alone, kBandTight more of it off the top (the title 15 lower than a slim band's);
// the big buttons: 64 x 1.1 = 70 units high, at least kButtonGap of that between neighbours at rest, the selected one grows 1.15
constexpr uint8_t kFrameTight = 16;
constexpr float kBandTight = 30.0f, kTitleTight = -23.0f, kButtonH = 70.4f, kButtonGap = 0.25f;
// (a title or line wider than the picture's room is drawn smaller, as the game's own text boxes do not wrap). uinative2: the band's
// text boxes are widened to the picture less kBandMargin (layout units; theirs are 450 and break a longer line inside a word)
constexpr float kBandMargin = 56.0f;
constexpr int kMaxUses = 24;
constexpr int kMaxButtons = 5 /* bubble: the host's Play Together session menu has Falls */, kButtonBanks = 2, kMaxWindows = 5, kMaxHeads = 22, kToastHeadBase = 7, kMaxChars = 6;   // (ingame: 14..16 edge heads, 17 the watch badge, 18..21 the star row)
constexpr int kStripHeadBase = 10, kMaxStrip = 3;   // the party strip's heads (the last three)
// touches: one colour per character (MpCharColor.hpp), the name tags', pointers' and lists' too (no slot colours any more)
inline Rgba charColor(int look, uint8_t a = 255) { const MpCharColor::C c = MpCharColor::fill(look); return Rgba{c.r, c.g, c.b, a}; }
inline Rgba charEdge(int look, uint8_t a = 255) { const MpCharColor::C c = MpCharColor::edge(look); return Rgba{c.r, c.g, c.b, a}; }
// MiiIcon's "Character" animation frame of each MpLook (Mario 0, Luigi 4, Toad (Kinopio) 2). No icon exists for the other three:
// Cosmic Mario / Cosmic Luigi are Mario's / Luigi's head with the dark starry tint, Yellow Toad is Toad's head tinted (kCharTint).
constexpr int kCharacters = 6;
constexpr float kCharacterFrame[kCharacters] = {0.0f, 4.0f, 2.0f, 0.0f, 2.0f, 4.0f};
struct CharTint { bool on; int16_t c0[4], c1[4]; bool swapRG = false; };   // swapRG: TEV swap table 0 = (R, R, B, A) on the head's texture
constexpr CharTint kCharTint[kCharacters] = {
    {false, {0, 0, 0, 0}, {255, 255, 255, 255}}, {false, {0, 0, 0, 0}, {255, 255, 255, 255}}, {false, {0, 0, 0, 0}, {255, 255, 255, 255}},
    {true, {0, 2, 34, 0}, {92, 124, 236, 255}},     // Cosmic Mario: dark starry blue
    {true, {0, 0, 0, 0}, {255, 255, 255, 255}, true},  // Yellow Toad: the swap table reads red as yellow (R, R, B, A): the cap's spot turns yellow, white and black stay
    {true, {0, 2, 34, 0}, {92, 124, 236, 255}},     // Cosmic Luigi
};
constexpr uint8_t kCharLookMask = 0x0F;
constexpr int kSceneTexts = 8, kSceneText = 96;
struct LayoutUse {
    uint8_t kind;        // UseKind
    uint8_t slot;        // which instance of that layout (buttons 0..3, windows 0..4, heads 0..7)
    uint8_t state;       // button: ButtonState; head / char: MpLook (char: | 0x10 selected, | 0x20 taken); frame: 1 = dim background
    int8_t text;         // index into Scene::texts (button label, frame title) or -1
    int8_t text2;        // frame: the scenario line; -1 none
    float x, y;          // root translation (layout units)
    float w, h;          // window: the frame's size; head: w = scale
    float alpha;
    // MP3 heads (a character head, USE_CHAR): who has it (-1 nobody; 0..3 the slot), whether that is the local player, and the head's
    // alpha for the 3D heads (the screen's own fade: a taken head is not faded there, `alpha` carries the 2D layout's ghost breathing)
    int8_t owner = -1;
    bool mine = false;
    float alpha3d = 1.0f;
    float y3d = 0.0f;   // (and its place without the ghost's float)
};
constexpr uint8_t kCharSelected = 0x10, kCharTaken = 0x20;
constexpr uint8_t kCharStill = 0x40;  // flow look: a player's head on KIND_HEADS: shown, not chosen (the star pointer does not point it)
constexpr uint8_t kCharArriving = 0x80;  // flow look: that head appeared this frame (MpHead3D pops it in with the game's sound)
// uxfix B3 (game picture 06: the second player's head was drawn from behind): the gathering's heads (kCharStill) are not the file select's
// endless turners: at rest they face the viewer (MpHead3D::Spec::restFacing)
inline bool headRestsFacing(uint8_t state) { return (state & kCharStill) != 0; }
constexpr uint8_t kHeadDark = 0x40;   // a head: washed dark grey (a player who is down); kCharTaken on a head = the pale blue wash (a bubble)
// A character another player has is drawn as its ghost: low, slowly pulsing alpha (in LayoutUse::alpha), an extra float (in y),
// and its head washed pale blue-white (the drawer tints C0 / C1 of the head's materials; it wins over Cosmic Mario's own tint).
constexpr float kGhostAlpha = 0.3f, kGhostPulse = 0.07f, kGhostFloat = 5.0f;
struct GhostTint { int16_t c0[4], c1[4]; };
constexpr GhostTint kGhostTint = {{120, 152, 225, 0}, {238, 246, 255, 255}};
struct Scene {
    LayoutUse uses[kMaxUses];
    int useCount = 0;
    char16_t texts[kSceneTexts][kSceneText];
    int textCount = 0;
    int headCount = 0;   // heads placed (menu scene from 0, toast scene from kToastHeadBase)
    uint16_t screenKey = 0;   // uinative: (menu scene) Model::screenKey; a change sends the big buttons to the other bank
    int8_t decided = -1;      // uinative: Model::decided
};

// ---------------------------------------------------------------- the plan
// memfix: a plan is a view (PlanBase: pointers and capacities) over storage of its own size (PlanStorage<V, R, T>). The menu's plan
// (Plan) holds the biggest screen; the toasts' (ToastPlan) and the strip's (StripPlan) are much smaller. The three were each 161,060 bytes of .bss
// (the menu's size); every compose function takes a PlanBase*, so a pointer to any of the three does. The capacities are checked against
// the largest real screens in tests/mp/ui_menu_test.cpp (testPlanCapacity: peaks recorded by the compose functions, margin >= 25 %).
// Measured peaks (ui_menu_test, every screen of the five languages in both pictures, the overlays, the worst toasts and ring):
// menu 2,523 vertices / 34 runs / 162 characters; toasts + ring 1,482 / 8 / 394; strip 0 / 3 / 3. A plan that runs out sets `overflow`
// (the rest of the frame's shapes are dropped, nothing is overwritten) and the menu logs it.
constexpr int kMaxVertices = 3840, kMaxRuns = 64, kMaxText = 1536;                 // the menu's and the overlays' (triangles: 3 to a triangle)
constexpr int kToastMaxVertices = 2048, kToastMaxRuns = 32, kToastMaxText = 640;   // the toasts (3 windows' words) and the chat ring
constexpr int kStripMaxVertices = 2400, kStripMaxRuns = 32, kStripMaxText = 128;    // the strip's Power Star flashes (and its link bars) and the in-play models (edge heads, badge, star row, prompt: worst frame 1,878)

struct PlanBase {
    Vertex* vertices;
    int maxVertices;
    int vertexCount = 0;
    TextRun* runs;
    int maxRuns;
    int runCount = 0;
    char16_t* text;
    int maxText;
    int textCount = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // the window (for the log and the checks)
    int hintRuns = 0;                       // uinative: runs [0, hintRuns) are the button guide (in the lower band), the rest the content
    float guideX0 = 0, guideX1 = 0;         // uinative: the guide's extent (square units)
    bool overflow = false;
    bool textClipped = false;               // some words were squeezed past their minimum (checks)
    struct Hit { float x0, y0, x1, y1; };   // chat: the Messages page's tiles (square units) for the star pointer / touch
    Hit hits[kMaxCells];     // vitabtn: every touch target of the page in model order: the list's rows, the heads, the grid's tiles
    int hitCount = 0;
    struct GuideHit { float x0, y0, x1, y1; int pic; };   // vitabtn: the guide band's buttons (a touch answers the button it shows)
    GuideHit guideHits[4];
    int guideHitCount = 0;
    Hit arrowHits[2];        // settingsnative: the selected row's two arrows (left, right), each at least 40 units wide: a touch on one is Left / Right
    int arrowHitCount = 0;
protected:
    // (defined in MpPanelPlan.cpp, not inline: a plan made by a constant initialiser would move all its arrays from .bss to .data)
    PlanBase(Vertex* v, int nv, TextRun* r, int nr, char16_t* t, int nt);
    PlanBase(const PlanBase&) = delete;
    PlanBase& operator=(const PlanBase&) = delete;
};
template <int V, int R, int T>
struct PlanStorage : PlanBase {
    Vertex vertexStore[V];
    TextRun runStore[R];
    char16_t textStore[T];
    PlanStorage() : PlanBase(vertexStore, V, runStore, R, textStore, T) {}
};
typedef PlanStorage<kMaxVertices, kMaxRuns, kMaxText> Plan;                        // the menu's (and the overlays')
typedef PlanStorage<kToastMaxVertices, kToastMaxRuns, kToastMaxText> ToastPlan;     // the toasts' (and the chat ring's)
typedef PlanStorage<kStripMaxVertices, kStripMaxRuns, kStripMaxText> StripPlan;     // the party strip's

// The highest use each plan has seen since boot (recorded by the compose functions; the tests check the capacities against them).
enum PlanKind : uint8_t { PLAN_MENU = 0, PLAN_TOAST, PLAN_STRIP, PLAN_KINDS };
struct PlanPeak { int vertices, runs, text; };
PlanPeak planPeak(PlanKind kind);
void resetPlanPeaks();

struct Screen { float width = 810.6667f, height = 456.0f; };

// ---------------------------------------------------------------- the party strip (beside the life counter)
// The other players' heads (MiiIcon, the same heads as the toasts), small, in a row to the right of PlayerLeft's counter. A state per head:
// normal; quiet (no packet for 3 s: dimmed); down (dark wash, sunk a little); in a bubble (the ghost's pale blue wash, floating).
// `flash` >= 0: that player just got a Power Star: the picture font's Power Star pops above the head and fades.
enum PartyState : uint8_t { PARTY_NORMAL = 0, PARTY_QUIET, PARTY_DOWN, PARTY_BUBBLE };
// touches: `link` = how the connection to that player is (LINK_*: three small bars under the head, no numbers); `away` = "Be right back" or idle:
// the head dims and a little "zzz" floats over it.
enum LinkQuality : uint8_t { LINK_GOOD = 0, LINK_WEAK, LINK_LOST };
// ingame: `enter` 0..1 = the head sliding in (a join) or out (a leave), 1 = in place; `pos` = its place in head steps from the first
// (fractional while a neighbour slides out), -1 = its index.
struct StripMember { uint8_t look; uint8_t state; float flash; uint8_t link = LINK_GOOD; bool away = false; float enter = 1.0f; float pos = -1.0f; };
struct Strip {
    int count = 0;
    StripMember m[kMaxStrip];
    float appear = 0.0f;   // 0..1, follows the HUD's own appear / hide (PlayerLeft)
    float counterX = -1.0f, counterY = 0.0f, counterScale = 1.0f;   // ingame: the game's picture pane this frame (square units; scale 1 = at rest); x < 0: not known
    int localLook = -1;    // touches: >= 0 = the local player's own head (Toad, Yellow Toad, ingame: Cosmic Mario, Cosmic Luigi) drawn on the life counter, whose picture has only Mario and Luigi
};
// Layout units of the 608 x 456 HUD layout: the first head's centre, the spacing, the heads' scale (MiiIcon is 100 units across).
constexpr float kStripX = -132.0f, kStripY = -182.0f, kStripStep = 40.0f, kStripScale = 0.38f;   // (ingame: -166 -> -132: a clear gap after the "x 4")
// touches: the life counter's own head (PlayerLeft's "PicMario"): centre and scale in the same layout units
constexpr float kCounterHeadX = -266.0f, kCounterHeadY = -182.0f, kCounterHeadScale = 0.40f;
constexpr float kQuietAlpha = 0.38f, kDownAlpha = 0.7f, kAwayAlpha = 0.45f;   // (touches: away dims like quiet; quiet also shows the lost bars)
// Heads go to `scene` (from kStripHeadBase), the Power Star flashes to `plan`.
void composeStrip(const Strip& strip, const Screen& screen, const FontMetrics fonts[FONT_COUNT], float bob, Scene* scene, PlanBase* plan);

// ---------------------------------------------------------------- ingame: edge heads and the watch badge
// A player off screen: no words, their head (the strip's head) at the picture's edge in a ring of their character's colour, a small
// pointer on the ring toward them (MpPresentLogic.hpp edgePlace keeps it clear of the HUD). Square units.
constexpr int kEdgeHeadBase = 14, kMaxEdges = 3, kBadgeHead = 17;
constexpr float kEdgeHeadSize = 34.0f;
struct EdgeHead { uint8_t look; float x, y, ax, ay, alpha; };
struct Edge { int count = 0; EdgeHead e[kMaxEdges]; };
void composeEdges(const Edge& edge, const Screen& screen, Scene* scene, PlanBase* plan);
// Spectating: the watched player's head, enlarged, in a ring of their colour in the top left corner, with the L and R pictures
// either side (they switch); no words.
struct Badge { uint8_t look; float alpha; bool canSwitch; };
constexpr float kBadgeX = -248.0f, kBadgeY = 168.0f, kBadgeSize = 58.0f;   // layout units (the Star counter's corner: hidden meanwhile)
void composeBadge(const Badge& badge, const Screen& screen, const FontMetrics fonts[FONT_COUNT], Scene* scene, PlanBase* plan);

// ingame: Play Together's Power Star: the faces of everyone in the session in a row under the game's "You got a Star!", a small
// Power Star over the one who touched it (MpPresentLogic.hpp starRowAlpha). No numbers, no names.
constexpr int kStarRowBase = 18, kStarRowMax = 4;
constexpr float kStarRowY = 0.83f, kStarRowHead = 40.0f, kStarRowStep = 50.0f;   // the row's centre (fraction of the height), head size, spacing
struct StarRow { int count = 0; uint8_t look[kStarRowMax]; int8_t by = -1; float alpha = 0.0f; };   // by: the index in `look` (-1 none)
void composeStarRow(const StarRow& row, const Screen& screen, const FontMetrics fonts[FONT_COUNT], Scene* scene, PlanBase* plan);

// ingame: the bubble's prompt: the press button's picture (Cross on the handheld build) on a small round plate, where the game would
// put its button mark (square units: the plate's centre); no words.
struct Prompt { float x = 0.0f, y = 0.0f, alpha = 0.0f; };
void composePrompt(const Prompt& prompt, const Screen& screen, const FontMetrics fonts[FONT_COUNT], PlanBase* plan);

// ---------------------------------------------------------------- chat: the quick ring (hold Select in play)
// Four phrase tiles around the middle of the picture (up, right, down, left), the Select picture in the centre; `picked` lights
// the one just sent (-1 none). Drawn in the toast plan with no layout (it comes and goes in a few frames).
struct Ring {
    char16_t label[4][kCellText];
    int picked = -1;
    float alpha = 0.0f;
    float pop = 0.0f;   // 0..1 the pick's little bounce
};
void composeRing(const Ring& ring, const Screen& screen, const FontMetrics fonts[FONT_COUNT], PlanBase* plan);
// The tile under a point (square units), -1 none.
int hitCell(const PlanBase& plan, float x, float y);
// vitabtn: the guide band's button (its picture number: MpMsgText::PIC_CROSS ...) under a point, -1 none.
int hitGuide(const PlanBase& plan, float x, float y);
int hitArrow(const PlanBase& plan, float x, float y);   // settingsnative: -1 the left arrow, +1 the right arrow, 0 neither

void begin(PlanBase* plan);
void beginScene(Scene* scene);
// A menu or an overlay: the layouts it uses (added to `scene`) and the GX drawing over them (added to `plan`).
// fonts[FONT_MESSAGE], fonts[FONT_PICTURE], fonts[FONT_DISPLAY] (may be empty). `bob` is the gentle float (the frame counter, in turns of a 3 s cycle).
void compose(const Model& model, const Screen& screen, const FontMetrics fonts[FONT_COUNT], float bob, Scene* scene, PlanBase* plan);
// The toasts (top of the picture, newest at the bottom of the stack): windows 2..4 and their words. uinative2: under the title band
// when compose() drew a menu with one since the last beginScene() (a toast never covers a menu's title).
void composeToasts(const Toast* toasts, int count, const Screen& screen, const FontMetrics fonts[FONT_COUNT], Scene* scene, PlanBase* plan);
// Layout units -> square units of the picture (y down, origin top left), and back.
float layoutToSquareX(const Screen& s, float x);
float layoutToSquareY(const Screen& s, float y);

// The picture font character of a picture number (message tag), and the markup character for it.
inline constexpr uint16_t pictureCode(int pic) { return static_cast<uint16_t>(0x30 + pic); }

}  // namespace MpPanel
