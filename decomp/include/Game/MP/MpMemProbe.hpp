#pragma once
// MEASUREMENT ONLY (setnat-memprobe): logs the scene heap free sizes at scene start and at the pause screen. PETARI_MEM_PROBE=1.
#include <cstdio>
#include <cstdlib>
#include "Game/System/HeapMemoryWatcher.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/MemoryUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include "port/mp_bridge.h"

namespace MpMemProbe {
    inline void log(const char* what) {
        static const bool on = getenv("PETARI_MEM_PROBE") && *getenv("PETARI_MEM_PROBE") == '1';
        if (!on) return;
        JKRHeap* napa = MR::getSceneHeapNapa();
        JKRHeap* gddr = MR::getSceneHeapGDDR3();
        JKRHeap* cache = SingletonHolder< HeapMemoryWatcher >::get()->mFileCacheHeap;
        auto kib = [](JKRHeap* h, bool total) -> long { return h ? (long)((total ? h->getTotalFreeSize() : h->getFreeSize()) / 1024) : -1; };
        // other: scene GDDR free/largest and file cache free/largest (layout archives mount into the cache, or the scene GDDR when the cache is low)
        mp_log("mem %s probe galaxy=%s scene_free=%ld scene_largest=%ld other=gddr_free:%ld,gddr_largest:%ld,cache_free:%ld,cache_largest:%ld napa_used=%ld napa_size=%ld", what,
               MR::getCurrentStageName(), kib(napa, true), kib(napa, false), kib(gddr, true), kib(gddr, false), kib(cache, true), kib(cache, false),
               napa ? (long)(((long)napa->mSize - napa->getTotalFreeSize()) / 1024) : -1L, napa ? (long)(napa->mSize / 1024) : -1L);
    }
}
