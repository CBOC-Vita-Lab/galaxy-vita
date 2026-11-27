// Galaxy 3.0 multiplayer: the layout groups' registry (memfix). See MpUiLayouts.hpp. Pure code.
#include "Game/MP/MpUiLayouts.hpp"

#include <string.h>

namespace MpUiLayouts {
namespace {
const Group* sGroups[kMaxGroups];
bool sMade[kMaxGroups];
bool sPermanent[kMaxGroups];
int sCount = 0;
Kind sKind = KIND_NONE;

bool Wants(const Group& g, Kind k) { return k != KIND_NONE && (g.kinds & (1u << k)) != 0; }
}  // namespace

bool add(const Group* g) {
    if (!g || !g->create || !g->forget || sCount >= kMaxGroups) return false;
    for (int i = 0; i < sCount; i++)
        if (sGroups[i] == g) return false;
    sGroups[sCount] = g;
    sMade[sCount] = false;
    sPermanent[sCount] = false;
    ++sCount;
    return true;
}
int groupCount() { return sCount; }
const Group* group(int i) { return i >= 0 && i < sCount ? sGroups[i] : nullptr; }
bool created(int i) { return i >= 0 && i < sCount && sMade[i]; }

void beginScene(Kind kind) { sKind = kind; }
Kind sceneKind() { return sKind; }
void setSceneKind(Kind kind) { sKind = kind; }

uint32_t bytesNeeded() {
    uint32_t n = 0;
    for (int i = 0; i < sCount; i++)
        if (!sMade[i] && Wants(*sGroups[i], sKind)) n += sGroups[i]->bytes;
    return n;
}

bool ready() {
    if (sKind == KIND_NONE) return false;
    for (int i = 0; i < sCount; i++)
        if (!sMade[i] && Wants(*sGroups[i], sKind)) return false;
    return true;
}

int archivesWanted(Kind kind, const char** out, int cap) {
    int n = 0;
    for (int i = 0; i < sCount; i++) {
        if (sMade[i] || !Wants(*sGroups[i], kind)) continue;
        for (int a = 0; a < sGroups[i]->archiveCount && sGroups[i]->archives; a++) {
            const char* name = sGroups[i]->archives[a];
            bool dup = false;
            for (int k = 0; k < n && !dup; k++) dup = strcmp(out[k], name) == 0;
            if (!dup && n < cap) out[n++] = name;
        }
    }
    return n;
}

Result ensure(uint32_t freeBytes, uint32_t reserve, int* layouts, ArchiveReadyFn ready, const char** missing) {
    if (layouts) *layouts = 0;
    if (missing) *missing = nullptr;
    if (sKind == KIND_NONE) return R_NO_KIND;
    const uint32_t need = bytesNeeded();
    if (need == 0) return R_READY;
    if (ready) {
        for (int i = 0; i < sCount; i++) {
            if (sMade[i] || !Wants(*sGroups[i], sKind)) continue;
            for (int a = 0; a < sGroups[i]->archiveCount && sGroups[i]->archives; a++) {
                if (!ready(sGroups[i]->archives[a])) {
                    if (missing) *missing = sGroups[i]->archives[a];
                    return R_NO_ARCHIVE;
                }
            }
        }
    }
    if (freeBytes < need || freeBytes - need < reserve) return R_NO_MEMORY;
    for (int i = 0; i < sCount; i++) {
        if (sMade[i] || !Wants(*sGroups[i], sKind)) continue;
        sGroups[i]->create();
        sMade[i] = true;
        if (layouts) *layouts += sGroups[i]->layouts;
    }
    return R_CREATED;
}

Result ensureAll(int* layouts) {
    if (layouts) *layouts = 0;
    bool any = false;
    for (int i = 0; i < sCount; i++) {
        if (sMade[i]) continue;
        sGroups[i]->create();
        sMade[i] = true;
        sPermanent[i] = true;
        any = true;
        if (layouts) *layouts += sGroups[i]->layouts;
    }
    return any ? R_CREATED : R_READY;
}

void endScene() {
    for (int i = 0; i < sCount; i++) {
        if (!sMade[i] || sPermanent[i]) continue;
        sGroups[i]->forget();
        sMade[i] = false;
    }
    sKind = KIND_NONE;
}

}  // namespace MpUiLayouts
