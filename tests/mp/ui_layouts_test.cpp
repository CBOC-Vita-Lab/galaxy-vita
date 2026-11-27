// memfix: the UI layout registry (MpUiLayouts): groups are made per scene kind, only with room, once, and forgotten at scene end.
//   g++ -std=c++20 -fsanitize=address,undefined -g -Idecomp/include tests/mp/ui_layouts_test.cpp decomp/src/Game/MP/MpUiLayouts.cpp -o /tmp/ui_layouts_test && /tmp/ui_layouts_test
#include <cstdio>
#include "Game/MP/MpUiLayouts.hpp"
using namespace MpUiLayouts;
static int gFails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++gFails; } } while (0)
static int cPanel, cChars, fPanel, fChars;
static void mkPanel() { ++cPanel; }
static void mkChars() { ++cChars; }
static void fgPanel() { ++fPanel; }
static void fgChars() { ++fChars; }
static const char* const kPA[] = {"PauseMenu", "MiiIcon"};
static const char* const kCA[] = {"MiiIcon"};
static const Group kP = {"panel", kMenu | kSession, 32, 1000, mkPanel, fgPanel, kPA, 2};
static const Group kC = {"chars", kMenu, 6, 500, mkChars, fgChars, kCA, 1};
// (integ B1) which layout archives the game has mounted: the scene init thread may not mount any, so a missing one refuses the group
static bool sMounted[3];   // PauseMenu, MiiIcon
static bool mounted(const char* a) { return a[0] == 'P' ? sMounted[0] : sMounted[1]; }
int main() {
    CHECK(add(&kP) && add(&kC) && !add(&kP) && groupCount() == 2);
    // no scene / single player: nothing, ever
    CHECK(ensure(1u << 30, 0) == R_NO_KIND && !ready() && cPanel == 0);
    beginScene(KIND_NONE);
    CHECK(ensure(1u << 30, 0) == R_NO_KIND && cPanel == 0 && cChars == 0);
    endScene();
    // session scene: the panel only (no characters)
    beginScene(KIND_SESSION);
    CHECK(bytesNeeded() == 1000);
    CHECK(ensure(3999, 3000) == R_NO_MEMORY && cPanel == 0 && !ready());   // 3999 - 1000 < 3000: refused, nothing made
    int n = 0;
    CHECK(ensure(4000, 3000, &n) == R_CREATED && n == 32 && cPanel == 1 && cChars == 0 && ready());
    CHECK(ensure(0, 1u << 30) == R_READY && cPanel == 1);   // (made once)
    endScene();
    CHECK(fPanel == 1 && fChars == 0 && sceneKind() == KIND_NONE && !ready());
    // menu scene: nothing until the first need; then both groups
    beginScene(KIND_MENU);
    CHECK(cPanel == 1 && bytesNeeded() == 1500);
    CHECK(ensure(10000, 3000, &n) == R_CREATED && n == 38 && cPanel == 2 && cChars == 1);
    // a session that began in a scene without groups gets the session's (the caller switches the kind)
    endScene();
    CHECK(fPanel == 2 && fChars == 1);
    beginScene(KIND_NONE);
    beginScene(KIND_SESSION);
    CHECK(ensure(1u << 20, 0) == R_CREATED && cChars == 1);
    endScene();
    // integ B1: the init thread's creation needs the archives mounted: refused (nothing made, which one is named), never a crash
    beginScene(KIND_SESSION);
    const char* names[8];
    CHECK(archivesWanted(KIND_SESSION, names, 8) == 2);   // (no duplicates, the groups of the kind that are not made yet)
    CHECK(archivesWanted(KIND_MENU, names, 8) == 2 && archivesWanted(KIND_MENU, names, 1) == 1);
    const int before = cPanel;
    const char* miss = nullptr;
    sMounted[0] = true, sMounted[1] = false;
    CHECK(ensure(1u << 20, 0, &n, mounted, &miss) == R_NO_ARCHIVE && cPanel == before && !ready() && miss && miss[0] == 'M');
    sMounted[0] = sMounted[1] = false;
    CHECK(ensure(1u << 20, 0, &n, mounted, &miss) == R_NO_ARCHIVE && miss && miss[0] == 'P');   // (the first missing one)
    CHECK(ensure(1u << 20, 0, &n, nullptr, nullptr) == R_CREATED && cPanel == before + 1);        // (the main thread: the game mounts its own)
    endScene();
    beginScene(KIND_SESSION);
    sMounted[0] = sMounted[1] = true;
    CHECK(ensure(1u << 20, 0, &n, mounted, &miss) == R_CREATED && miss == nullptr);
    endScene();
    // eager (measuring): every group, kept through endScene
    CHECK(ensureAll(&n) == R_CREATED && n == 38 && cChars == 2);
    endScene();
    CHECK(fChars == 1 && created(0) && created(1));
    fprintf(stderr, "%d failures\n", gFails);
    return gFails ? 1 : 0;
}
