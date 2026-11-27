// upd26 (update26, 12 Nov 2026): the pure part of GXV_UPD_PREFETCH3, shared by decomp/src/Game/NameObj/NameObjCategoryList.cpp and the host
// test (tests/upd26_pipeline.cpp, tools/test_upd26.sh). No game types.
//
// The list loop of NameObjCategoryList::execute asks for memory ahead of the object it runs, in three stages that each look at an object a fixed
// number of places on: the object's own lines (kStageLines places on), the first lines of its parts (kStageParts), the blocks the parts point to
// (kStageBlocks). A stage must never look at an entry at or beyond the end of the list, and must look at each entry once per stage. Ahead() is
// the one place that decides it; the difference is taken before the compare, so no pointer beyond one past the end is ever formed.
#pragma once

namespace port {
namespace upd26 {

constexpr int kStageLines = 3;
constexpr int kStageParts = 2;
constexpr int kStageBlocks = 1;

// The entry k places on from `it`, or nullptr if the list ends before it (k >= 0, it < end).
template < class T > inline T* const* Ahead(T* const* it, int k, T* const* end) {
    return (end - it > k) ? it + k : nullptr;
}

}  // namespace upd26
}  // namespace port
