#include "port/diagnostics.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using u32 = std::uint32_t;
#include "vgx_anim_enums.inc"

#if defined(TARGET_PC) && defined(__vita__) && PORT_LOGGING
static constexpr bool Hooked = true;
#else
static constexpr bool Hooked = false;
#endif
static std::vector<std::string> Events;
static bool Movie, TimeUp, Skipped, MutateTimeUp, NewTimeUp, Nested;
static unsigned MovieReads, NerveReads, ContextBegins, ContextEnds, CategoryBegins, CategoryEnds, IgnoredEnds;
static u32 NextToken, ContextToken, CategoryToken;
static int ActiveDomain, ActiveCategory;

[[noreturn]] static void Fail(const char* why) {
    std::fprintf(stderr, "FAIL: %s\n", why);
    for (const auto& event : Events) std::fprintf(stderr, "  %s\n", event.c_str());
    std::exit(1);
}
static void Require(bool yes, const char* why) { if (!yes) Fail(why); }
static std::string Key(const char* label, int domain, int category) {
    return std::string(label) + ':' + std::to_string(domain) + ':' + std::to_string(category);
}

extern "C" u32 port_workload_anim_begin() {
    Require(ContextToken == 0, "context must not be nested in this fixture");
    ContextToken = ++NextToken;
    ++ContextBegins;
    Events.emplace_back("anim-begin");
    return ContextToken;
}
extern "C" void port_workload_anim_end(u32 token) {
    Require(token && token == ContextToken && !CategoryToken, "context end must match its token after categories");
    ContextToken = 0;
    ++ContextEnds;
    Events.emplace_back("anim-end");
}
extern "C" u32 port_workload_anim_category_begin(unsigned domain, unsigned category) {
    Events.push_back(Key("category-begin", domain, category));
    ++CategoryBegins;
    if (CategoryToken || !ContextToken) return 0;
    CategoryToken = ++NextToken;
    ActiveDomain = domain;
    ActiveCategory = category;
    return CategoryToken;
}
extern "C" void port_workload_anim_category_end(u32 token) {
    if (!token) { ++IgnoredEnds; Events.emplace_back("category-end-inactive"); return; }
    Require(token == CategoryToken && ContextToken, "typed hook must end its returned token, not an ID");
    Events.push_back(Key("category-end", ActiveDomain, ActiveCategory));
    CategoryToken = 0;
    ++CategoryEnds;
}
extern "C" void port_workload_view_entry_begin() {
    Require(!ContextToken && !CategoryToken, "view must be outside non-view context");
    Events.emplace_back("view-begin");
}
extern "C" void port_workload_view_entry_end() { Events.emplace_back("view-end"); }
extern "C" int port_turn_skipped() {
    Require(!ContextToken && !CategoryToken, "skip guard must follow completed non-view context");
    Events.emplace_back("skip-query");
    return Skipped;
}

class CategoryList {
public:
    static void execute(MR::MovementType);
    static void execute(MR::CalcAnimType);
    static void execute(MR::DrawType);
};
struct NameObj {
    int domain, category;
    void Run() {
        Require(!Hooked || !ContextToken || CategoryToken, "actor callback must lie inside typed category span");
        Events.push_back(Key("actor", domain, category));
    }
};
struct ObjArray {
    std::vector<NameObj*> objects;
    std::size_t size() const { return objects.size(); }
    NameObj** begin() { return objects.data(); }
    NameObj** end() { return objects.data() + objects.size(); }
};
struct BeforeFunction {
    int domain, category;
    void operator()() {
        Require(!Hooked || !ContextToken || CategoryToken, "before-function must lie inside typed span");
        Events.push_back(Key("before", domain, category));
        if (MutateTimeUp && domain == 1 && category == MR::MovementType_ShadowControllerHolder) TimeUp = NewTimeUp;
        if (Nested && domain == 0 && category == MR::CalcAnimType_MapObj) {
            Nested = false;
            const u32 parent = CategoryToken;
            CategoryList::execute(MR::CalcAnimType_MapObj);
            Require(!Hooked || CategoryToken == parent, "inactive nested same-ID end cannot close outer category");
        }
    }
};
struct Delegator { void operator()(NameObj* obj) { obj->Run(); } };
class NameObjCategoryList {
public:
    struct CategoryInfo { ObjArray mNameObjArr; BeforeFunction* mExecuteBeforeFunction = nullptr; };
    CategoryInfo mCategoryInfo[64];
    Delegator delegator;
    Delegator* mDelegator = &delegator;
    void execute(int);
};
class NameObjListExecutor {
public:
    NameObjCategoryList* mMovementList;
    NameObjCategoryList* mCalcAnimList;
    void executeMovement(int);
    void executeCalcAnim(int);
    void executeDraw(int category) { Events.push_back(Key("draw", 2, category)); }
};
struct SceneController {
    NameObjListExecutor* executor;
    NameObjListExecutor* getNameObjListExecutor() {
        Require(!Hooked || !ContextToken || CategoryToken, "executor lookup must lie inside category span");
        Events.emplace_back("get-executor");
        return executor;
    }
};
struct GameSystem { SceneController* mSceneController; };
static GameSystem* System;
template <typename T> struct SingletonHolder {
    static T* get() {
        Require(!Hooked || !ContextToken || CategoryToken, "singleton lookup must lie inside category span");
        Events.emplace_back("get-singleton");
        return System;
    }
};
namespace MR { static void loadViewMtx() { Events.emplace_back("load-view"); } }
struct SceneFunction {
    static void executeCalcAnimList();
    static void executeCalcAnimListOnPlayingMovie();
    static void executeCalcViewAndEntryList() {
        Require(!ContextToken && !CategoryToken, "view dispatch outside context");
        Events.emplace_back("view");
    }
};
struct GameScene {
    bool isPlayMovie() const { ++MovieReads; Events.emplace_back("movie-query"); return Movie; }
    bool isNerve(int) const { ++NerveReads; Events.emplace_back("nerve-query"); return TimeUp; }
    void calcAnim();
};
#define GET_NERVE(owner, name) 0
// (CategoryList::execute's GXV_ANIM_LOD category marker: no far-model deferral in these tests)
static void port_anim_lod_set_category(int) {}
// (NameObjCategoryList::execute's GXV_ANIM_PROF timing: off in these tests, the object is simply run)
#define PORT_ANIM_PROF_CALL(call, obj) call(obj)
// (CategoryList::execute(DrawType)'s GXV_DRAW_PROF timing: off in these tests, the list is simply run)
#define PORT_DRAW_PROF(row, call) call
// (GameScene::calcAnim's GXV_DRAWDONE_DEFER 3/4 view deferral: off here, mode 0, so the skip guard alone decides as before)
static int TestDrawdoneMode() { return 0; }
static int TestTurnSkippedRaw() { return 0; }
static bool sViewPending = false;
// (GameScene::calcAnim's GXV_PAR_VIEW batch around the view pass: off in these tests, nothing to record)
static void port_par_view_begin() {}
static void port_par_view_end() {}
// (SceneFunction::executeCalcAnimList's GXV_PAR_ANIM batch at the end of calc-anim: off in these tests)
static void port_par_anim_dispatch() {}
#define port_gx_drawdone_defer_mode TestDrawdoneMode
#define port_turn_skipped_raw TestTurnSkippedRaw
#include "vgx_anim_methods.inc"
#undef port_gx_drawdone_defer_mode
#undef port_turn_skipped_raw
#undef GET_NERVE

static void ExpectedCategory(std::vector<std::string>& expected, int domain, int category, bool actor = true) {
    if (Hooked) expected.push_back(Key("category-begin", domain, category));
    expected.emplace_back("get-singleton");
    expected.emplace_back("get-executor");
    if (actor) {
        expected.push_back(Key("before", domain, category));
        expected.push_back(Key("actor", domain, category));
    }
    if (Hooked) expected.push_back(Key("category-end", domain, category));
}

int main() {
    NameObjCategoryList movement, animation;
    NameObjListExecutor executor{&movement, &animation};
    SceneController controller{&executor};
    GameSystem system{&controller};
    System = &system;
    NameObj actors[2][64];
    BeforeFunction before[2][64];
    for (int domain = 0; domain < 2; ++domain) {
        auto& list = domain ? movement : animation;
        for (int category = 0; category < 64; ++category) {
            actors[domain][category] = {domain, category};
            before[domain][category] = {domain, category};
            list.mCategoryInfo[category].mNameObjArr.objects.push_back(&actors[domain][category]);
            list.mCategoryInfo[category].mExecuteBeforeFunction = &before[domain][category];
        }
    }
    // Even an empty category's lookup/dispatch is enclosed, but its before-function must not run.
    animation.mCategoryInfo[MR::CalcAnimType_Environment].mNameObjArr.objects.clear();
    const int normal[] = {MR::CalcAnimType_Environment, MR::CalcAnimType_MapObj, MR::CalcAnimType_NPC, MR::CalcAnimType_Ride,
        MR::CalcAnimType_Enemy, MR::CalcAnimType_Player, MR::CalcAnimType_PlayerDecoration, MR::CalcAnimType_MapObjDecoration,
        MR::CalcAnimType_Item, MR::CalcAnimType_Layout, MR::CalcAnimType_LayoutDecoration, MR::CalcAnimType_UNK_0x12,
        MR::CalcAnimType_MirrorMapObj};
    GameScene scene;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        Movie = scenario == 1;
        TimeUp = scenario == 2 || scenario == 5;
        Skipped = scenario == 3;
        MutateTimeUp = scenario >= 4;
        NewTimeUp = scenario == 4;
        Nested = false;
        MovieReads = NerveReads = ContextBegins = ContextEnds = CategoryBegins = CategoryEnds = IgnoredEnds = 0;
        Events.clear();
        scene.calcAnim();
        Require(MovieReads == 1 && NerveReads == 1, "real method must evaluate each existing state getter exactly once");
        Require(ContextBegins == (Hooked ? 1u : 0u) && ContextEnds == ContextBegins, "one non-view parent pair per scene call");
        std::vector<std::string> expected;
        if (Hooked) expected.emplace_back("anim-begin");
        expected.emplace_back("movie-query");
        if (Movie) ExpectedCategory(expected, 0, MR::CalcAnimType_MovieSubtitles);
        else {
            expected.emplace_back("load-view");
            for (int category : normal) ExpectedCategory(expected, 0, category, category != MR::CalcAnimType_Environment);
            ExpectedCategory(expected, 1, MR::MovementType_ShadowControllerHolder);
            ExpectedCategory(expected, 0, MR::CalcAnimType_AnimParticle);
        }
        expected.emplace_back("nerve-query");
        if (!TimeUp) ExpectedCategory(expected, 0, MR::CalcAnimType_AnimParticleIgnorePause);
        if (Hooked) expected.emplace_back("anim-end");
        expected.emplace_back("skip-query");
        if (!Skipped) {
            if (Hooked) expected.emplace_back("view-begin");
            expected.emplace_back("view");
            if (Hooked) expected.emplace_back("view-end");
        }
        Require(Events == expected, "actual normal/movie/time-up/mutating-state/skip/view and category ordering must remain exact");
        Require(!ContextToken && !CategoryToken, "all real hook tokens must be closed");
    }
    Movie = TimeUp = Skipped = MutateTimeUp = false;
    Nested = true;
    Events.clear();
    scene.calcAnim();
    Require(!ContextToken && !CategoryToken, "same-ID nested callbacks must not unpair outer hooks");
    Require(IgnoredEnds == (Hooked ? 1u : 0u), "nested hook must propagate its inactive token");

    Events.clear();
    const unsigned begins = CategoryBegins;
    CategoryList::execute(MR::DrawType_SwingRope);
    Require(CategoryBegins == begins, "Draw overload must remain uninstrumented");
    Require(Events == std::vector<std::string>{"get-singleton", "get-executor", Key("draw", 2, MR::DrawType_SwingRope)}, "Draw overload's original lookup/dispatch stays unchanged");
    std::printf("PASS: actual animation context/typed category/before-function hooks %s; six scene routes, nested same-ID and Draw invariance\n", Hooked ? "Vita logging" : "inert");
}
