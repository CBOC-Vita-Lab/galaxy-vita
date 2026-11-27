#pragma once

// (1.3 port) GXV_J3DNATIVE, the game's half: the bracket and the version counters the front end's material-list skip stands on
// (platform/vgx/src/vgx_j3native.inc has the protocol, the stages and the proof).
//
// Every material-list / differed-list application of the draw pass (DrawBufferShapeDrawer::drawPort) is sent as a bracket:
//   0x5A id verA verB ext flags (five u32), the call (CALL_DL, 9 bytes), 0x5B.
//     id     the list's guest address (a stable name of "this application")
//     verA   the list's own version (J3DDisplayListObj::mPortVer: bumped at every writer of its bytes: endDL, endPatch, the stored
//            differed list, newDisplayList, swapBuffer, setSingleDisplayList, single_To_Double), merged lists: both lists' versions
//     verB   the drawer's membership version: its own serial (add / remove / swap / resetLightSort) and the count / mask of its
//            visible packets (show / hide, the culled model)
//     ext    the scene / external serial: the sum of the global classes below that change what a list's replay leaves behind
//     flags  bit 0: a differed list; bits 8..31: the kinds of the animation that last made it differ (J3DMaterial::diff's flags: BTK
//            texture matrices, BRK colours, BPK, BTP texture numbers, MR:: colour utilities, fades), the census's "which kind bumped it"
// The front end skips nothing it cannot prove: it checks the memory write generation (exact), and otherwise the versions, which are
// claims of this side, behind probation, a periodic scrub and a hash cross-check (vgx_j3native.inc). The bytes are still written by
// the game: a skip changes what the front end parses, never what the game writes.
//
// Not bracketed (class fallback, sent as before): lists with a user list (environment maps), drawers with a light in front of a
// packet's differed list, anything while a display list is recorded or the particle gathering is on, any other renderer.
#ifdef TARGET_PC
#include <revolution.h>

enum {
    J3DPortVer_Scene,   // a scene's destruction / creation (the heaps the lists live in)
    J3DPortVer_Fog,     // the scene's fog (FogCtrl, GXSetFog writers)
    J3DPortVer_Fade,    // screen fades / tint (MR:: colour utilities on the scene)
    J3DPortVer_Light,   // light loads (ActorLightCtrl): the light memory is checked by the front end's hash; counted here
    J3DPortVer_Member,  // show / hide of a model, LOD switches
    J3DPortVer_Count
};
extern u32 gJ3DPortVer[J3DPortVer_Count];
inline void J3DPortVerBump(int k) { __atomic_fetch_add(&gJ3DPortVer[k], 1u, __ATOMIC_RELAXED); }
inline u32 J3DPortExtSerial() {
    return __atomic_load_n(&gJ3DPortVer[J3DPortVer_Scene], __ATOMIC_RELAXED) * 0x9E3779B1u +
           __atomic_load_n(&gJ3DPortVer[J3DPortVer_Fog], __ATOMIC_RELAXED) * 0x85EBCA6Bu +
           __atomic_load_n(&gJ3DPortVer[J3DPortVer_Fade], __ATOMIC_RELAXED) * 0xC2B2AE35u;
}
// (j3n2) the game's half of the census (GXV_J3DNATIVE >= 1; J3DPacket.cpp logs them every 300 pictures as "j3n game: ..."): who bumped
// which version, and how many of the writes left the bytes as they were (then, versions content-based, nothing is bumped)
enum {
    J3DPortCen_FogCalls,      // FogCtrl::update calls
    J3DPortCen_FogReal,       // of them with fog values that differ from the materials' (only these bump J3DPortVer_Fog)
    J3DPortCen_MemberBumps,   // show / hide
    J3DPortCen_EndDL,         // J3DDisplayListObj::endDL (a list made again in the other buffer)
    J3DPortCen_EndDLSame,     // of them the bytes of the current buffer: the swap undone, no bump
    J3DPortCen_EndPatch,      // endPatch (J3DMaterial::patch)
    J3DPortCen_EndPatchSame,  // of them unchanged: the swap undone, no bump
    J3DPortCen_DiffStored,    // endDiffDL storing changed bytes (GXV_J3D_DIFFKEEP: unchanged ones are kept without a bump)
    J3DPortCen_OtherBumps,    // newDisplayList / newSingleDisplayList / single_To_Double / setSingleDisplayList
    J3DPortCen_Count
};
extern u32 gJ3DPortCensus[J3DPortCen_Count];
inline void J3DPortCensusAdd(int k) { __atomic_fetch_add(&gJ3DPortCensus[k], 1u, __ATOMIC_RELAXED); }
void J3DPortSetDiffKinds(u32 kinds);  // J3DPacket.cpp: J3DMaterial::diff's flags, for the list it writes (0 after)
int J3DPortNativeMode();  // GXV_J3DNATIVE (default 0; 1 census, 2 verify, 3 on), read once (GXV_HOT_KNOBS); 0 when the FIFO is not the vgx front end's
extern "C" void port_gx_call_bracketed(const void* list, u32 nbytes, u32 id, u32 verA, u32 verB, u32 ext, u32 flags);  // GXDisplayList.c
#endif
