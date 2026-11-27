// Galaxy 3.0 multiplayer: shared world state in Play Together (DESIGN.md section 4 "World", section 8 "Host migration").
// Placement registry (stable object keys), switch bridge with a mirror bitmap kept on every console, shared enemy defeats,
// purple coins and star chips. The pure logic (registry, policy tables, mirror, codecs) is in MpWorldLogic.cpp.
//
// Active only when mp_active() && mp_mode() == MP_MODE_TOGETHER. Every hook returns at once otherwise.
#include "Game/MP/MpWorld.hpp"
#include "Game/MP/MpWorldLogic.hpp"
#include "Game/MP/MpWorldGuard.hpp"   // MP3 security: plausibility / rate limits of a guest's world events
#include "Game/MP/MpFlow.hpp"
#include "Game/MP/MpGame.hpp"
#include "port/mp_core_game.h"         // smallfix: mp_core_report_bad
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <new>

#include "Game/Demo/DemoActionKeeper.hpp"
#include "Game/Demo/DemoCastGroupHolder.hpp"
#include "Game/Demo/DemoDirector.hpp"
#include "Game/Demo/DemoExecutor.hpp"
#include "Game/LiveActor/AllLiveActorGroup.hpp"
#include "Game/LiveActor/HitSensor.hpp"
#include "Game/LiveActor/HitSensorInfo.hpp"
#include "Game/LiveActor/HitSensorKeeper.hpp"
#include "Game/LiveActor/LiveActor.hpp"
#include "Game/Map/StageSwitch.hpp"
#include "Game/MapObj/ChipBase.hpp"
#include "Game/MapObj/ChipGroup.hpp"
#include "Game/MapObj/ChipHolder.hpp"
#include "Game/MapObj/Coin.hpp"
#include "Game/MapObj/PurpleCoinHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/JMapIdInfo.hpp"
#include "port/mp_prof.h"
extern "C" int64_t port_host_time_ns(void);
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/SceneUtil.hpp"

using namespace MpWorldLogic;

namespace {
    enum { kCarryPerTurn = 8, kCarryMaxLiveTurns = 3600, kCarryBlockTurns = 1800, kKillRetryTurns = 180, kMapCheckTurns = 60, kRepairGapTurns = 120, kLiveWarmupTurns = 10,
           kLocalInfoSize = 1024 };

    // optfix item 16: the registry, the mirror and the local-writer table (67 KB) exist only for a Together session (see LazyMem).
    struct WorldMem {
        Registry reg;
        SwitchMirror mirror;
        ObjState objState;                       // worldsync: scratch for the object state map (packed on demand, never per frame)
        ObjCarry carry;                          // hostreload: kills / purple coins / chips of this scene serial, survives the scene's own reload
        const void* localInfo[kLocalInfoSize];   // SwitchIdInfo* written by local-policy writers (open addressing)
    };
    WorldMem* allocWorldMem() {
        void* p = calloc(1, sizeof(WorldMem));   // (zeroed: what the statics were)
        return p ? new (p) WorldMem() : nullptr;
    }
    void freeWorldMem(WorldMem* m) { m->~WorldMem(); free(m); }
    LazyMem< WorldMem > s_mem;
    Registry& reg_() { return s_mem.get()->reg; }          // (only where s_mem.get() is known: s_placing / s_placed / after ensureMem)
    SwitchMirror& mir_() { return s_mem.get()->mirror; }
    PendingQueue s_pending;
    uint32_t s_mirrorStage;                 // hash of stage name + scenario the mirror belongs to (0 = not known yet)

    int s_localInfoCount;
    bool s_localInfoFull;

    bool s_placing;                         // between beginPlacement and placementDone
    PoolRanges s_pool;                      // actors of lazily created shared pools in this placement (kept out of the keys)
    bool s_placed;                          // registry finalized for s_placedSerial
    bool s_inPlacementDone;                 // scene objects may be written although the scene is not live yet
    uint16_t s_placedSerial;
    bool s_carryPass;                       // hostreload: the host is putting its carry back, a few bits per live turn
    int s_carryDone;
    uint32_t s_carryDropTurn;               // turn of the last dropCarry (a blocked carry that no placement un-blocks is armed again after kCarryBlockTurns)
    bool s_restarting;                      // group restart wipe/reload in progress for s_restartSerial: ignore/suppress pre-restart object events
    uint16_t s_restartSerial;
    int s_zoneCount;                        // MR::getZoneNum() of the placed scene
    bool s_applying;                        // a remote event is being applied
    bool s_lastChipRemote;                  // chipsync: the last chip taken on this console came from another one (applyChip) or from its own player (onChip)
    uint32_t s_turn;                        // own turn counter (update() runs once per logic turn)
    uint32_t s_liveTurns;                   // turns spent live in this scene

    // Registry CRC agreement. Guests compare with the host's; the host keeps one result per guest slot.
    MpWorldGuard::SenderLimits s_limits;     // host: per guest, per event type ceilings
    MpWorldGuard::ReplyGate s_replyGate;     // host: REGCRC answers (amplification)
    uint32_t s_refused[MP_MAX_PLAYERS];      // events refused per sender (the request to the core's bad-message path is in the report)
    MpWorldLogic::PeerCrc s_hostCrcState;   // the host's CRC (guest); bound to the placement: dropped at beginPlacement
    uint32_t s_guestBad[MP_MAX_PLAYERS];     // host: turns a guest's CRC has disagreed with ours
    bool s_guestBadLogged[MP_MAX_PLAYERS];
    bool s_guestCrcKnown[MP_MAX_PLAYERS];
    uint16_t s_guestCrcSerial[MP_MAX_PLAYERS];
    uint32_t s_guestCrc[MP_MAX_PLAYERS];
    bool s_guestNeedsReply[MP_MAX_PLAYERS];

    // Switch map check (guests) and periodic CRC (host).
    int s_mapMismatch;
    RepairBudget s_repairBudget;            // guest: switch map repair requests (refills 1 per 10 s; full again when the maps agree)
    uint32_t s_lastRepairTurn;
    int s_objMismatch;                      // worldsync: summaries in a row that said this console lacks kills / coins / chips
    RepairBudget s_objBudget;               // guest: object state repair requests
    uint32_t s_lastObjRepairTurn;
    bool s_repairPending;                   // the next full map is authoritative (answers our repair request)
    uint32_t s_lastMapCheckTurn;
    int64_t s_lastMapCheckNs;               // (perf) the host's 1 s CRC is timed by the clock, not by turns
    uint16_t s_epoch;
    bool s_epochKnown;
    DropStats s_drops;                      // per scene drop counters (worldsync); printed and reset at endScene
    char s_sceneName[40];                   // "<stage>:<scenario>" of the placed scene, for the summary line
    bool s_wasActive;                       // tracks active->inactive->active reconnect edge
    bool s_needReconnectRepair;             // true after an active->inactive drop until reconnect repair runs
    bool s_regCrcSent;                      // placementDone sent initial REGCRC while active

    bool worldOn() { return mp_active() && mp_mode() == MP_MODE_TOGETHER; }
    bool worldHeld() { return MpGame::heldMode() == MP_MODE_TOGETHER; }
    uint16_t worldSerialNow() { return mp_active() ? mp_scene_serial() : MpGameInternal::serial(); }
    // The kinds this session shares (the object state map packs and applies only those)
    uint8_t shareMask() {
        const MpSettings* st = mp_settings();
        return (uint8_t)((st->sharedKills ? SHARE_KILL : 0) | ((st->flags & 2) ? SHARE_PURPLE : 0) | ((st->flags & 4) ? SHARE_CHIP : 0));
    }
    // hostreload: remember a defeat / coin / chip of this scene (index of the placed registry) so a reload of the same scene can put it back
    void noteCarry(int kind, int idx) {
        if (s_mem.get() && s_placed && reg_().ready()) carryNote(&s_mem.get()->carry, s_placedSerial, reg_().crc(), kind, idx);
    }
    // hostreload: players other than this console that are present and in the scene of this serial (their last pose carries it)
    int othersInScene() {
        int n = 0;
        for (int s = 0; s < MP_MAX_PLAYERS; s++) {
            if (s == mp_local_slot() || !mp_slot_present(s)) continue;
            MpPose p;
            if (MpPuppets::lastPose(s, &p) && p.sceneSerial == mp_scene_serial()) n++;
        }
        return n;
    }
    bool actorDeadNow(const void* a) { return a != nullptr && MR::isDead(static_cast< const LiveActor* >(a)); }

    // The block of a Together session: made at a scene start (or by the first world message), freed at the first scene end / start
    // after the session. False: no memory (said once): this session has no object / switch sync, nothing else changes.
    bool ensureMem() {
        if (s_mem.get()) return true;
        const bool wasFailed = s_mem.failed();
        if (!s_mem.acquire(allocWorldMem) && !wasFailed) mp_log("world: no memory for the registry (%u bytes): object and switch sync off", (unsigned)sizeof(WorldMem));
        return s_mem.get() != nullptr;
    }
    void releaseMem() {
        s_mem.release(freeWorldMem);
        s_mirrorStage = 0;
        s_localInfoCount = 0;
        s_localInfoFull = false;
    }

    bool isHostSlot(int slot) { return slot >= 0 && slot == mp_host_slot(); }

    // worldsync: every event that is not applied is counted and (rate limited) logged with type, key and reason.
    void drop(uint8_t msgType, DropReason why, const MpObjKey* key = nullptr, unsigned evSerial = 0) {
        const DropType dt = dropTypeOfMsg(msgType);
        if (!s_drops.note(dt, why, s_turn)) return;
        if (key) mp_log("world drop type=%s key=%u/%04x/%u/%u reason=%s serial=%u/%u", dropTypeName(dt), (unsigned)key->zone, (unsigned)key->nameHash,
                        (unsigned)key->lid, (unsigned)key->sub, dropReasonName(why), evSerial, (unsigned)mp_scene_serial());
        else mp_log("world drop type=%s reason=%s serial=%u/%u msg=%02x", dropTypeName(dt), dropReasonName(why), evSerial, (unsigned)mp_scene_serial(), msgType);
    }
    void dropIdx(DropType dt, DropReason why, int idx) {   // a queued item: the key comes from the registry (still valid while queued)
        if (!s_drops.note(dt, why, s_turn)) return;
        if (s_mem.get() && idx >= 0 && idx < reg_().count()) {
            const MpObjKey& k = reg_().at(idx).key;
            mp_log("world drop type=%s key=%u/%04x/%u/%u reason=%s", dropTypeName(dt), (unsigned)k.zone, (unsigned)k.nameHash, (unsigned)k.lid, (unsigned)k.sub,
                   dropReasonName(why));
        } else mp_log("world drop type=%s idx=%d reason=%s", dropTypeName(dt), idx, dropReasonName(why));
    }
    DropType pendType(uint8_t kind) { return kind == PEND_KILL ? DT_KILL : (kind == PEND_PURPLE ? DT_PURPLE : DT_CHIP); }
    void pushPending(uint8_t kind, uint16_t idx, uint32_t deadline) {
        PendingQueue::Item ev;
        if (s_pending.push(kind, idx, deadline, &ev)) dropIdx(pendType(ev.kind), DR_QUEUE_FULL, ev.index);   // the oldest was dropped
    }

    // ---- local writer set ----------------------------------------------------------------------------------------------
    uint32_t infoHash(const void* p) {
        uintptr_t v = (uintptr_t)p;
        v ^= v >> 4;
        v *= 2654435761u;
        return (uint32_t)v & (kLocalInfoSize - 1);
    }
    void addLocalInfo(const void* info) {
        if (!info) return;
        if (s_localInfoCount >= kLocalInfoSize / 2) {
            if (!s_localInfoFull) mp_log("world local-switch table full");
            s_localInfoFull = true;
            return;
        }
        uint32_t h = infoHash(info);
        while (s_mem.get()->localInfo[h] && s_mem.get()->localInfo[h] != info) h = (h + 1) & (kLocalInfoSize - 1);
        if (!s_mem.get()->localInfo[h]) {
            s_mem.get()->localInfo[h] = info;
            s_localInfoCount++;
        }
    }
    bool isLocalInfo(const void* info) {
        if (s_localInfoCount == 0) return false;
        uint32_t h = infoHash(info);
        while (s_mem.get()->localInfo[h]) {
            if (s_mem.get()->localInfo[h] == info) return true;
            h = (h + 1) & (kLocalInfoSize - 1);
        }
        return false;
    }
    void clearLocalInfo() {
        if (s_mem.get()) memset(s_mem.get()->localInfo, 0, sizeof s_mem.get()->localInfo);
        s_localInfoCount = 0;
        s_localInfoFull = false;
    }

    // (slot, bit) of a switch id; false when not representable.
    bool switchSlotBit(const SwitchIdInfo& info, int* slot, int* bit) {
        if (!info.mIDInfo) return false;
        int s = info.mIsGlobal ? kGlobalSlot : zoneSlot(info.mIDInfo->mZoneID);
        s32 b = info.getSwitchNo();
        if (s < 0 || b < 0 || b >= kBits) return false;
        *slot = s;
        *bit = b;
        return true;
    }

    // ---- real switches -------------------------------------------------------------------------------------------------
    ZoneSwitch* realZone(int slot) {
        if (!MR::isExistSceneObj(SceneObj_StageSwitchContainer)) return nullptr;
        StageSwitchContainer* c = MR::getSceneObj< StageSwitchContainer >(SceneObj_StageSwitchContainer);
        if (!c) return nullptr;
        return slot == kGlobalSlot ? c->getGlobalSwitches() : c->getZoneSwitches(slot);
    }
    // Writes the bit directly: readers poll every frame, no callback runs, nothing is re-sent (loop guard).
    // Scene objects are touched only while the scene is live (or inside placementDone): never during a teardown.
    bool sceneWritable() { return s_placed && (s_inPlacementDone || MpGame::isGameSceneLive()); }
    void writeReal(int slot, int bit, bool on) {
        if (!sceneWritable() || s_placedSerial != mp_scene_serial()) return;
        ZoneSwitch* z = realZone(slot);
        if (z && z->get(bit) != on) z->set(bit, on);
    }
    void applyCb(void*, int slot, int bit, bool on) { writeReal(slot, bit, on); }
    // Mirror on-bits -> real switches (scene start after a reload, after a map).
    void restoreReal() {
        if (!s_placed) return;
        for (int s = 0; s < kZoneSlots; s++) {
            const BitSet128& b = mir_().onBits(s);
            if (!b.any()) continue;
            for (int i = 0; i < kBits; i++) {
                if (b.get(i) && !mir_().isGuarded(s, i) && !mir_().isLocal(s, i)) writeReal(s, i, true);
            }
        }
    }

    // The mirror follows the scene serial: a new serial forgets the old stage's bits.
    void syncMirrorSerial(uint16_t serial) {
        if (!mir_().valid() || mir_().serial() != serial) {
            mir_().reset(serial);
            s_mirrorStage = 0;
            s_mapMismatch = 0;
            s_repairBudget.reset();
            s_repairPending = false;
        }
    }

    bool regUsable();   // (defined with the object sync agreement below)

    // ---- sending -------------------------------------------------------------------------------------------------------
    void sendRegCrc(int toSlot, uint8_t flags = 0) {
        uint8_t buf[kRegCrcLenFlags];
        RegCrcMsg m = {s_placedSerial, reg_().crc(), flags};
        size_t n = writeRegCrc(buf, sizeof buf, m);
        if (toSlot < 0) mp_send(MP_MSG_REGCRC, buf, (uint16_t)n, true);
        else mp_send_to(toSlot, MP_MSG_REGCRC, buf, (uint16_t)n, true);
    }
    void sendMap(int toSlot, bool full) {
        uint8_t buf[kMapHeader + kMapEntry * kMapMaxZones];
        size_t n = writeMap(buf, sizeof buf, mp_scene_serial(), mir_(), full);
        if (!n) return;
        if (toSlot < 0) mp_send(MP_MSG_SWITCHMAP, buf, (uint16_t)n, true);
        else mp_send_to(toSlot, MP_MSG_SWITCHMAP, buf, (uint16_t)n, true);
        if (full) mp_log("world switchmap sent zones=%d crc=%08x to=%d", mir_().zonesWithBits(), mir_().crc(), toSlot);
    }
    // worldsync (host): the scene's kill / purple coin / chip state: full = every chunk that holds a bit, then the summary; else the summary
    // only (the 1 s check). Chunks are 138 bytes at most.
    void sendObjMap(int toSlot, bool full) {
        if (!mp_is_host() || !regUsable() || s_mem.get()->carry.blocked) return;   // (blocked: a group restart is under way, the registry still holds the pre-restart flags)
        ObjState& st = s_mem.get()->objState;
        packObjState(reg_(), &st, shareMask(), actorDeadNow);
        carryPrune(&s_mem.get()->carry, reg_(), actorDeadNow);   // an enemy that came back by design leaves the carry
        carryMerge(s_mem.get()->carry, s_placedSerial, reg_().crc(), shareMask(), reg_(), &st);   // hostreload: what the reload has not put back yet is still the host's state
        uint8_t buf[kObjMapHeader + kObjMapChunkBytes];
        int chunks = 0;
        if (full) {
            for (int kind = OM_KILL; kind <= OM_CHIP; kind++) {
                for (int start = 0; start < st.usedBytes; start += kObjMapChunkBytes) {
                    const int n = st.usedBytes - start < kObjMapChunkBytes ? st.usedBytes - start : (int)kObjMapChunkBytes;
                    if (!objChunkHasBits(st, kind, start, n)) continue;
                    const size_t len = writeObjChunk(buf, sizeof buf, s_placedSerial, reg_().crc(), kind, st, start, n);
                    if (!len) continue;
                    if (toSlot < 0) mp_send(MP_MSG_OBJMAP, buf, (uint16_t)len, true);
                    else mp_send_to(toSlot, MP_MSG_OBJMAP, buf, (uint16_t)len, true);
                    chunks++;
                }
            }
        }
        const size_t len = writeObjSummary(buf, sizeof buf, s_placedSerial, reg_().crc(), st);
        if (len) {
            if (toSlot < 0) mp_send(MP_MSG_OBJMAP, buf, (uint16_t)len, true);
            else mp_send_to(toSlot, MP_MSG_OBJMAP, buf, (uint16_t)len, true);
        }
        if (full) mp_log("world objmap sent kill=%u purple=%u chip=%u chunks=%d to=%d", (unsigned)st.count[0], (unsigned)st.count[1], (unsigned)st.count[2], chunks, toSlot);
    }
    void sendObj(uint8_t type, const MpObjKey& key) {
        uint8_t buf[kObjLen];
        ObjMsg m = {s_placedSerial, key};
        size_t n = writeObj(buf, sizeof buf, m);
        mp_send(type, buf, (uint16_t)n, true);
    }

    // ---- object sync agreement -----------------------------------------------------------------------------------------
    bool regUsable() { return s_placed && reg_().ready() && !reg_().overflowed() && s_placedSerial == mp_scene_serial(); }
    // May this console send object events? The host always (receivers judge); a guest when its registry equals the host's.
    bool objSyncLocal() {
        if (!regUsable()) return false;
        if (mp_is_host()) return true;
        return s_hostCrcState.agrees(s_placedSerial, reg_().crc());
    }
    // worldsync: why objSyncFrom(slot) is false (the drop reason), or -1 when the event may be judged now.
    int objSyncWhy(int slot) {
        if (!s_placed) return DR_NOT_PLACED;
        if (!reg_().ready() || reg_().overflowed()) return DR_REG_OFF;
        if (s_placedSerial != mp_scene_serial()) return DR_SERIAL;
        if (mp_is_host() && !isHostSlot(slot)) {
            if (slot < 0 || slot >= MP_MAX_PLAYERS || !s_guestCrcKnown[slot] || s_guestCrcSerial[slot] != s_placedSerial) return DR_NO_CRC;
            return s_guestCrc[slot] == reg_().crc() ? -1 : DR_CRC_MISMATCH;
        }
        if (mp_is_host()) return -1;
        if (!s_hostCrcState.known || s_hostCrcState.serial != s_placedSerial) return DR_NO_CRC;
        return s_hostCrcState.crc == reg_().crc() ? -1 : DR_CRC_MISMATCH;
    }
    // May an object event from `slot` be applied here?
    bool objSyncFrom(int slot) {
        if (!regUsable()) return false;
        if (mp_is_host() && !isHostSlot(slot)) {
            // From a guest: only when that guest reported the same registry for this scene.
            return slot >= 0 && slot < MP_MAX_PLAYERS && s_guestCrcKnown[slot] && s_guestCrcSerial[slot] == s_placedSerial &&
                   s_guestCrc[slot] == reg_().crc();
        }
        return objSyncLocal() || (mp_is_host() && isHostSlot(slot));
    }
    // Host, a guest's world event: over its ceiling -> dropped and counted. (The leader's own and a host-slot sender are never judged.)
    bool senderAllowed(int slot, MpWorldGuard::Kind k, const char* what) {
        if (!mp_is_host() || isHostSlot(slot)) return true;
        if (s_limits.allow(slot, k, s_turn)) return true;
        if (slot >= 0 && slot < MP_MAX_PLAYERS && s_refused[slot]++ < 5) mp_log("world refused %s from slot=%d: rate", what, slot);
        MpWorldGuard::reportRate(mp_is_host(), slot, [](int sl, const char* why) { mp_core_report_bad(sl, why); });   // smallfix
        return false;
    }
    float plausMax() {
        static float v = [] {
            float d = MpWorldGuard::kDefaultMaxDistance;
#if defined(TARGET_PC) && !defined(__vita__)
            if (const char* e = getenv("SMG_MP_PLAUS_DIST")) { const float f = (float)atof(e); if (f > 0.0f) d = f; }
#endif
            return d;
        }();
        return v;
    }
    // Host: the sender's last pose against the object (log every measured distance: the limit is tuned from these lines)
    bool senderNear(int slot, const char* what, const LiveActor* a) {
        if (!mp_is_host() || isHostSlot(slot) || a == nullptr) return true;
        MpPose p;
        if (!MpPuppets::lastPose(slot, &p)) return true;   // no pose yet: nothing to judge by
        const float o[3] = {a->mPosition.x, a->mPosition.y, a->mPosition.z};
        const float dx = p.pos[0] - o[0], dy = p.pos[1] - o[1], dz = p.pos[2] - o[2];
        mp_log("world plaus %s slot=%d dist=%.0f", what, slot, sqrtf(dx * dx + dy * dy + dz * dz));
        if (MpWorldGuard::nearEnough(p.pos, o, plausMax())) return true;
        if (slot >= 0 && slot < MP_MAX_PLAYERS && s_refused[slot]++ < 5) mp_log("world refused %s from slot=%d: far", what, slot);
        MpWorldGuard::reportFar(mp_is_host(), slot, p.pos, o, plausMax(), [](int sl, const char* why) { mp_core_report_bad(sl, why); });   // smallfix
        return false;
    }

    // the mismatch line is a verdict, not a first impression: see MpWorldLogic::PeerCrc (a reload places the same serial twice)
    void checkHostCrc() {
        if (mp_is_host()) return;
        if (s_hostCrcState.tick(s_placed, s_placedSerial, reg_().crc()))
            mp_log("world regcrc mismatch -> object sync off (host=%08x mine=%08x)", s_hostCrcState.crc, reg_().crc());
    }
    void checkGuestCrcs() {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            const bool bad = mp_is_host() && s_placed && s_guestCrcKnown[i] && s_guestCrcSerial[i] == s_placedSerial && s_guestCrc[i] != reg_().crc();
            if (!bad) { s_guestBad[i] = 0; s_guestBadLogged[i] = false; continue; }
            if (s_guestBad[i] < MpWorldLogic::kMismatchGrace) s_guestBad[i]++;
            if (s_guestBad[i] >= MpWorldLogic::kMismatchGrace && !s_guestBadLogged[i]) {
                s_guestBadLogged[i] = true;
                mp_log("world regcrc mismatch -> object sync off slot=%d (guest=%08x mine=%08x)", i, s_guestCrc[i], reg_().crc());
            }
        }
    }

    // ---- actors --------------------------------------------------------------------------------------------------------
    int sensorCount(const LiveActor* a) { return a->mSensorKeeper ? a->mSensorKeeper->mSensorInfosSize : 0; }
    HitSensor* sensorAt(const LiveActor* a, int i) {
        HitSensorInfo* info = a->mSensorKeeper->getNthSensorInfo(i);
        return info ? info->mSensor : nullptr;
    }
    bool isChipName(const char* n) { return n && (strcmp(n, "BlueChip") == 0 || strcmp(n, "YellowChip") == 0); }
    // chipsync: a chip carried by another actor (Shellfish / TreasureBoxCracked create a YellowChip / BlueChip inside their own init, named in Shift-JIS) is no placement of its own: it is a
    // sub>0 child under the carrier's key, so it needs the chip flag by its own (identical on every console) name.
    bool isCarriedChipName(const char* n) {
        return n && (strcmp(n, "\x83\x43\x83\x47\x83\x8d\x81\x5b\x83\x60\x83\x62\x83\x76") == 0 || strcmp(n, "\x83\x75\x83\x8b\x81\x5b\x83\x60\x83\x62\x83\x76") == 0);
    }
    int s_regChipPlain = 0, s_regChipCarried = 0;
    bool isEnemyActor(LiveActor* a) {
        if (!a->mSensorKeeper) return false;
        HitSensor* body = a->getSensor("body");
        if (!body && sensorCount(a) > 0) body = sensorAt(a, 0);
        return body && MR::isSensorEnemy(body);
    }
    bool isPlayerSensor(const HitSensor* s) { return s && MR::isSensorPlayer(s); }
    // The enemy touches or holds the local player (or is held): applying a remote defeat now could strand the player.
    bool touchesPlayer(LiveActor* a) {
        if (!a->mSensorKeeper) return false;
        HitSensorKeeper* k = a->mSensorKeeper;
        if (isPlayerSensor(k->mTaking) || isPlayerSensor(k->mTaken)) return true;
        int n = sensorCount(a);
        for (int i = 0; i < n; i++) {
            HitSensor* s = sensorAt(a, i);
            if (!s || !s->mSensors) continue;
            for (int j = 0; j < s->mSensorCount; j++) {
                if (isPlayerSensor(s->mSensors[j])) return true;
            }
        }
        return false;
    }

    void logKey(const char* what, const MpObjKey& k, const char* src) {
        mp_log("world %s key=%u/%04x/%u/%u src=%s", what, (unsigned)k.zone, (unsigned)k.nameHash, (unsigned)k.lid, (unsigned)k.sub,
               src);
    }

    // Remote applies. Return true when done (applied or nothing to do), false to retry later.
    bool applyKill(int idx) {
        Entry& e = reg_().at(idx);
        LiveActor* a = (LiveActor*)e.actor;
        if (MR::isDead(a)) { e.flags |= ENTRY_KILLED; noteCarry(OM_KILL, idx); return true; }   // idempotent: already dead here
        if (touchesPlayer(a)) return false;
        s_applying = true;
        a->makeActorDead();                                  // never the class kill(): no items, no SW_DEAD, no demo
        s_applying = false;
        e.flags |= ENTRY_KILLED;                             // worldsync: part of the state map this console can hand on
        noteCarry(OM_KILL, idx);
        logKey("kill", e.key, "remote");
        return true;
    }
    bool applyPurple(int idx) {
        Entry& e = reg_().at(idx);
        if (e.flags & ENTRY_GOT) return true;                // each console counts a coin once
        Coin* c = (Coin*)(LiveActor*)e.actor;
        s_applying = true;
        bool done = c->mpRemoteGet();
        s_applying = false;
        if (!done) return false;                             // not appeared yet: retry until it does
        e.flags |= ENTRY_GOT;
        noteCarry(OM_PURPLE, idx);
        logKey("purple", e.key, "remote");
        return true;
    }
    bool applyChip(int idx) {
        Entry& e = reg_().at(idx);
        if (e.flags & ENTRY_GOT) return true;
        ChipBase* c = (ChipBase*)(LiveActor*)e.actor;
        s_lastChipRemote = true;
        s_applying = true;
        bool done = c->mpRemoteGet();
        s_applying = false;
        if (!done) return false;
        e.flags |= ENTRY_GOT;
        noteCarry(OM_CHIP, idx);
        logKey("chip", e.key, "remote");
        return true;
    }

    bool carryTry(void*, int kind, int idx) {
        if (kind == OM_KILL) return applyKill(idx);
        if (kind == OM_PURPLE) return applyPurple(idx);
        return applyChip(idx);
    }

    // ---- placement-time classification ---------------------------------------------------------------------------------
    void markLocalInfo(const SwitchIdInfo* info) {
        if (!info) return;
        addLocalInfo(info);
        int slot, bit;
        if (switchSlotBit(*info, &slot, &bit)) mir_().markLocal(slot, bit);
    }
    // Switches written by demo actions (DemoActionKeeper types 4/5 on, 12/13 off) stay local: each console runs its own demo.
    void markDemoActionSwitches() {
        if (!MR::isExistSceneObj(SceneObj_DemoDirector)) return;
        DemoDirector* d = MR::getSceneObj< DemoDirector >(SceneObj_DemoDirector);
        if (!d || !d->_18) return;
        for (s32 i = 0; i < d->_18->getObjNum(); i++) {
            DemoExecutor* ex = static_cast< DemoExecutor* >(d->_18->getCastGroup(i));   // as DemoFunction does
            if (!ex || !ex->mActionKeeper) continue;
            DemoActionKeeper* k = ex->mActionKeeper;
            for (s32 j = 0; j < k->mNumInfos; j++) {
                DemoActionInfo* info = k->mInfoArray[j];
                if (!info) continue;
                s32 t = info->mActionType;
                bool a = t == 4 || t == 12, b = t == 5 || t == 13;
                if (!a && !b) continue;
                for (s32 c = 0; c < info->mCastCount; c++) {
                    LiveActor* actor = info->mCastList[c];
                    if (!actor || !actor->mStageSwitchCtrl) continue;
                    markLocalInfo(a ? actor->mStageSwitchCtrl->mSW_A : actor->mStageSwitchCtrl->mSW_B);
                }
            }
        }
    }
    void markHolders() {
        if (MR::isExistSceneObj(SceneObj_PurpleCoinHolder)) {
            PurpleCoinHolder* h = MR::getSceneObj< PurpleCoinHolder >(SceneObj_PurpleCoinHolder);
            for (s32 i = 0; h && i < h->getObjNum(); i++) reg_().setFlagsForActor(h->getActor(i), ENTRY_PURPLE);
        }
        const int ids[2] = {SceneObj_BlueChipHolder, SceneObj_YellowChipHolder};
        for (int t = 0; t < 2; t++) {
            if (!MR::isExistSceneObj(ids[t])) continue;
            ChipHolder* h = MR::getSceneObj< ChipHolder >(ids[t]);
            for (s32 g = 0; h && h->mChipGroups && g < h->mNumChipGroups; g++) {
                ChipGroup* grp = h->mChipGroups[g];
                if (!grp) continue;
                s32 n = grp->mTotalCount < 5 ? grp->mTotalCount : 5;
                for (s32 c = 0; c < n; c++) {
                    if (grp->mChips[c].mChip) reg_().setFlagsForActor(static_cast< LiveActor* >(grp->mChips[c].mChip), ENTRY_CHIP);
                }
            }
        }
    }

    // ---- host migration ------------------------------------------------------------------------------------------------
    void checkEpoch() {
        uint16_t ep = mp_host_epoch();
        if (!s_epochKnown) {
            s_epochKnown = true;
            s_epoch = ep;
            return;
        }
        if (ep == s_epoch) return;
        s_epoch = ep;
        // Per-guest agreements were made with the old host's view; forget them. Repairs restart.
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            s_guestCrcKnown[i] = false;
            s_guestNeedsReply[i] = false;
        }
        s_mapMismatch = 0;
        s_repairBudget.reset();
        s_objMismatch = 0;
        s_objBudget.reset();
        s_repairPending = false;
        mp_log("world host change epoch=%u host=%d me_host=%d", (unsigned)ep, mp_host_slot(), (int)mp_is_host());
        if (!s_placed) return;
        s_regCrcSent = true;
        s_needReconnectRepair = false;
        if (mp_is_host()) {
            // The new host has the full mirror already: send it and the registry CRC at once.
            sendRegCrc(-1);
            sendMap(-1, true);
            sendObjMap(-1, true);
        } else {
            sendRegCrc(-1, kRegCrcRepair);   // to the new host: re-establishes object sync for this guest (and asks for its map: a plain repeat is not answered)
        }
    }
    // worldsync: one set bit of the host's object state map: idempotent (a purple coin / chip already counted is skipped; a kill is skipped by
    // applyKill when the enemy is dead here), the same queue as an event. HOST-AUTHORITATIVE CONVERGENCE (coordinator decision): the host packs a
    // defeat only while its actor is dead NOW (packObjState + actorDeadNow). An enemy that came back on a guest by design (Meramera::tryRecovery
    // -> resetAppear, decided per console) but is still dead on the host is killed AGAIN on the guest by the next map, within ~2-3 s (the 1 s summary,
    // two checks, then a repair request). When the HOST is the one whose enemy came back, it is not packed (it is alive), so the guests keep it
    // dead until the enemy is defeated again: the host console is the reference. No behaviour is added here beyond that.
    void applyObjBit(void*, int kind, int idx) {
        if (idx < 0 || idx >= reg_().count()) return;
        const uint8_t f = reg_().at(idx).flags;
        if (f & ENTRY_DUP) return;
        const bool now = MpGame::isGameSceneLive();
        if (kind == OM_KILL) {
            if (!(f & ENTRY_ENEMY) || !(shareMask() & SHARE_KILL)) return;   // (applyKill is idempotent: an enemy that is dead here is done; one that came back is killed again)
            if (!now || !applyKill(idx)) pushPending(PEND_KILL, (uint16_t)idx, now ? s_turn + kKillRetryTurns : 0);   // (not live: no deadline - the turns of an opening demo must not use up the retry time)
        } else if (kind == OM_PURPLE) {
            if (!(f & ENTRY_PURPLE) || (f & ENTRY_GOT) || !(shareMask() & SHARE_PURPLE)) return;
            if (!now || !applyPurple(idx)) pushPending(PEND_PURPLE, (uint16_t)idx, 0);
        } else if (kind == OM_CHIP) {
            if (!(f & ENTRY_CHIP) || (f & ENTRY_GOT) || !(shareMask() & SHARE_CHIP)) return;
            if (!now || !applyChip(idx)) pushPending(PEND_CHIP, (uint16_t)idx, 0);
        }
    }
    // Guest: the host's 1 s summary. Behind (the host has more, or another set of the same size) twice in a row => ask for the state again.
    void onObjSummary(const ObjMapMsg& om) {
        ObjState& st = s_mem.get()->objState;
        packObjState(reg_(), &st, shareMask(), actorDeadNow);
        if (!objStateBehind(om, st)) { s_objMismatch = 0; s_objBudget.reset(); return; }
        if (++s_objMismatch >= 2 && s_turn - s_lastObjRepairTurn >= kRepairGapTurns && s_objBudget.take(s_turn)) {
            s_lastObjRepairTurn = s_turn;
            s_objMismatch = 0;
            mp_log("world objmap behind host kill=%u/%u purple=%u/%u chip=%u/%u -> request", (unsigned)st.count[0], (unsigned)om.count[0], (unsigned)st.count[1],
                   (unsigned)om.count[1], (unsigned)st.count[2], (unsigned)om.count[2]);
            sendRegCrc(-1, kRegCrcRepair);   // the host answers a repair with the registry CRC, the switch map and the object state
        }
    }

    EarlyQueue s_early;                     // worldsync: object events that cannot be judged yet (survives scene boundaries; pruned by serial)
    enum { kEarlyTurns = 3600 };            // 60 s of world turns

    // One KILL / PURPLE / CHIP from `fromSlot`. True = done (applied, queued for the scene to run, or dropped and counted). False = it cannot
    // be judged yet (the scene is not placed, the next scene serial, the peer registry CRC not known): the caller keeps it in s_early.
    bool processObj(uint8_t type, int fromSlot, const ObjMsg& o) {
        const uint16_t core = mp_scene_serial();
        if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && o.serial == __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED)) { drop(type, DR_SERIAL, &o.key, o.serial); return true; }
        if (serialAhead(o.serial, core)) return false;                                  // the next scene: wait for it
        if (o.serial != core) { drop(type, DR_SERIAL, &o.key, o.serial); return true; }  // an old scene: gone
        const int why = objSyncWhy(fromSlot);
        if (why == DR_NOT_PLACED || why == DR_NO_CRC || why == DR_SERIAL) return false;
        if (why >= 0) { drop(type, (DropReason)why, &o.key, o.serial); return true; }
        int idx = reg_().findKey(o.key);
        if (idx < 0) { drop(type, DR_UNKNOWN_KEY, &o.key, o.serial); return true; }
        const MpWorldGuard::Kind gk = type == MP_MSG_KILL ? MpWorldGuard::K_KILL : (type == MP_MSG_PURPLE ? MpWorldGuard::K_PURPLE : MpWorldGuard::K_CHIP);
        if (!senderAllowed(fromSlot, gk, type == MP_MSG_KILL ? "KILL" : (type == MP_MSG_PURPLE ? "PURPLE" : "CHIP"))) { drop(type, DR_RATE, &o.key, o.serial); return true; }
        if (!senderNear(fromSlot, type == MP_MSG_KILL ? "KILL" : (type == MP_MSG_PURPLE ? "PURPLE" : "CHIP"), (const LiveActor*)reg_().at(idx).actor)) { drop(type, DR_FAR, &o.key, o.serial); return true; }
        if (!eventKindShared(type, shareMask())) { drop(type, DR_WRONG_KIND, &o.key, o.serial); return true; }   // the session does not share this kind (kills, purple coins, star chips)
        uint8_t f = reg_().at(idx).flags;
        bool now = MpGame::isGameSceneLive();   // not live: queue it, update() applies it once the scene runs
        if (type == MP_MSG_KILL) {
            if (!(f & ENTRY_ENEMY) || !mp_settings()->sharedKills) { drop(type, DR_WRONG_KIND, &o.key, o.serial); return true; }
            if (!now || !applyKill(idx)) pushPending(PEND_KILL, (uint16_t)idx, now ? s_turn + kKillRetryTurns : 0);   // (not live: no deadline - the turns of an opening demo must not use up the retry time)
        } else if (type == MP_MSG_PURPLE) {
            if (!(f & ENTRY_PURPLE)) { drop(type, DR_WRONG_KIND, &o.key, o.serial); return true; }
            if (!now || !applyPurple(idx)) pushPending(PEND_PURPLE, (uint16_t)idx, 0);
        } else {
            if (!(f & ENTRY_CHIP)) { drop(type, DR_WRONG_KIND, &o.key, o.serial); return true; }
            if (!now || !applyChip(idx)) pushPending(PEND_CHIP, (uint16_t)idx, 0);
        }
        return true;
    }
    void deferObj(uint8_t type, int fromSlot, const ObjMsg& o) {
        if (!serialWaitable(o.serial, mp_scene_serial())) { drop(type, DR_SERIAL, &o.key, o.serial); return; }   // only this scene's or the next one's may wait
        EarlyQueue::Item ev;
        if (s_early.push(type, (uint8_t)fromSlot, o.serial, o.key, s_turn + kEarlyTurns, &ev)) drop(ev.type, DR_QUEUE_FULL, &ev.key, ev.serial);
    }
}  // namespace

namespace MpWorldSync {

bool applyingRemote() { return s_applying; }

void dropCarry(const char* why) {   // MpBubble: a group restart (everyone caught) on every console; also a RESTART this console could not act on
    if (!s_mem.get()) return;
    if (s_mem.get()->carry.valid) mp_log("world reload carry dropped (%s)", why ? why : "-");
    carryDrop(&s_mem.get()->carry);
    s_carryDropTurn = s_turn;
    s_carryPass = false;
}

void onRestartMessage() {   // MpBubble: RESTART from the host (or host's own group restart) for this serial, whether or not this console can act on it (G8)
    // (runs on the game thread: the scene-init thread may be inside placement; the flags read here are only ever set by that thread, and a stale
    // read means the console is treated as "loading", which clears more, never less)
    const uint16_t ser = mp_scene_serial();
    MpGame::dropHeldWorld();   // held pre-restart world messages (OBJMAP chunks, a SWITCH / SWITCHMAP) must not replay at the first live turn; every branch (the pool is empty whenever the scene is live)
    s_early.dropSerial(ser);   // events of the scene the others are leaving, kept for this serial while this console loaded
    s_pending.clear();         // nothing queued from the pre-restart scene may apply during the wipe or after reload
    const bool loading = s_placing || (s_placed && !MpGame::isGameSceneLive());
    if (!loading) {
        if (!__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) || __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED) != ser) {
            __atomic_store_n(&s_restartSerial, ser, __ATOMIC_RELAXED);
            __atomic_store_n(&s_restarting, true, __ATOMIC_RELEASE);
            if (s_mem.get()) dropCarry("restart message");
        }
        return;
    }
    if (!s_mem.get()) return;
    // this console placed the scene already but is not live yet: what it queued came from the scene the others are leaving; its carry must not
    // stay blocked for the whole scene. Clear both, ask the host again (the host answers once its own placement is done).
    carryClear(&s_mem.get()->carry);
    s_carryPass = false;
    mp_log("world restart while loading: pending, early and held world messages cleared, carry cleared, map re-requested");
    if (s_placed && !mp_is_host()) sendRegCrc(-1, kRegCrcRepair);
}

int sceneObjBegin() {   // SceneObjHolder::create: a scene object is made lazily, inside the row that asked for it
    if (!s_placing) return -1;
    AllLiveActorGroup* group = MR::getAllLiveActorGroup();
    return s_pool.begin(group ? group->getObjNum() : 0, true);
}

void sceneObjEnd(int token) {
    if (token == -1) return;
    AllLiveActorGroup* group = MR::getAllLiveActorGroup();
    s_pool.end(token, group ? group->getObjNum() : 0);
}

void beginPlacement() {
    s_pool.clear();
    s_placing = false;
    s_placed = false;
    if (s_mem.get()) reg_().clear();
    s_pending.clear();
    clearLocalInfo();
    s_regCrcSent = false;
    if (!worldHeld()) {
        releaseMem();   // (optfix: a single-player scene or a finished session holds none of the 67 KB)
        return;
    }
    s_mem.retryNext();   // fix2: a failed 67 KB allocation is tried again at every scene start (memory differs scene to scene)
    if (!ensureMem()) return;
    uint16_t serial = worldSerialNow();
    uint32_t stage = nameHash32(MR::getCurrentStageName()) ^ ((uint32_t)MR::getCurrentScenarioNo() * 0x9E3779B1u);
    if (stage == 0) stage = 1;
    syncMirrorSerial(serial);
    if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE)) {
        // beginPlacement() runs on the async scene-init thread: never touch s_early here (game thread already dropped it in onRestartMessage and holdEarly refused s_restartSerial while s_restarting was set)
        __atomic_store_n(&s_restarting, false, __ATOMIC_RELEASE);
    }
    carryArm(&s_mem.get()->carry);
    if (s_mem.get()->carry.valid && s_mem.get()->carry.serial != serial) carryClear(&s_mem.get()->carry);   // hostreload: another scene (a real MOVE)
    if (s_mirrorStage != 0 && s_mirrorStage != stage) {
        mir_().reset(serial);   // same serial but another stage: never carry bits over
        carryClear(&s_mem.get()->carry);
    }
    s_mirrorStage = stage;
    mir_().resetScene();        // keeps the on-bits of this serial (reload after a miss), clears masks and guards
    s_replyGate.newPlacement();
    s_hostCrcState.beginPlacement();   // the CRC of the placement before is no verdict on this one (same serial after a reload)
    for (int i = 0; i < MP_MAX_PLAYERS; i++) { s_guestBad[i] = 0; s_guestBadLogged[i] = false; }
    s_objMismatch = 0;
    s_objBudget.reset();
    s_repairPending = false;      // the map that answers our placement REGCRC is applied "on only"
    s_mapMismatch = 0;
    s_liveTurns = 0;
    s_regChipPlain = s_regChipCarried = 0;
    s_placing = true;
}

void registerPlacement(const JMapInfoIter& iter, NameObj* created, int before, int after) {
    if (!s_placing || !created) return;
    AllLiveActorGroup* group = MR::getAllLiveActorGroup();
    if (!group) return;
    int total = group->getObjNum();
    if (before < 0) before = 0;
    if (after > total) after = total;
    if (after <= before) return;

    const char* name = nullptr;
    if (!MR::getObjectName(&name, iter) || !name) return;
    s32 zone = MR::getPlacedZoneId(iter);
    if (zone < 0 || zone >= kGlobalSlot) return;
    s32 lid = -1;
    if (!MR::getJMapInfoLinkID(iter, &lid)) lid = 0xFFFF;

    MpObjKey key;
    key.zone = (uint8_t)zone;
    key.lid = (uint16_t)lid;
    key.nameHash = mp_name_hash15(name);
    // sub 0 is the created object itself; when it is not a LiveActor its children start at 1.
    int base = (static_cast< NameObj* >(group->getActor(before)) == created) ? 0 : 1;
    bool boss = isBossName(name);
    for (int i = before; i < after; i++) {
        if (s_pool.contains(i)) continue;   // a shared pool's actor (CoinHolder's Coins): no row's, never keyed
        int sub = s_pool.subFor(before, i, base);
        if (sub > 255) break;
        LiveActor* a = group->getActor(i);
        if (!a) continue;
        key.sub = (uint8_t)sub;
        uint8_t flags = 0;
        if (sub == 0 && !boss && isEnemyActor(a)) flags |= ENTRY_ENEMY;
        // chipsync: a Star Chip registers with its ChipGroup only in initAfterPlacement, i.e. AFTER placementDone: markHolders() found every group empty, so no chip
        // ever got ENTRY_CHIP and onChip / applyChip / the objmap's chip bits never matched one (and the chip's "body" sensor is an enemy-range sensor, so it was
        // flagged as a KILL-eligible enemy instead). The placement name says it is a chip; the key is the same (zone, link id, name) on every console.
        if (sub == 0 && isChipName(name)) {
            flags = (uint8_t)((flags & ~ENTRY_ENEMY) | ENTRY_CHIP);
            s_regChipPlain++;
        } else if (sub > 0 && isCarriedChipName(a->getName())) {
            flags = (uint8_t)((flags & ~ENTRY_ENEMY) | ENTRY_CHIP);
            s_regChipCarried++;
        }
        reg_().add(key, nameHash32(a->getName()), a, flags);
    }
}

void placementDone() {
    if (!s_placing) return;
    s_placing = false;
    markDemoActionSwitches();
    reg_().finalize();
    markHolders();
    s_placed = true;
    s_placedSerial = worldSerialNow();
    s_zoneCount = MR::getZoneNum();
    snprintf(s_sceneName, sizeof s_sceneName, "%s:%d", MR::getCurrentStageName() ? MR::getCurrentStageName() : "-", (int)MR::getCurrentScenarioNo());
    if (worldOn() && !s_epochKnown) {
        s_epochKnown = true;
        s_epoch = mp_host_epoch();
    }
    mp_log("world reg n=%d crc=%08x dups=%d overflow=%d serial=%u", reg_().count(), reg_().crc(), reg_().dupCount(),
           (int)reg_().overflowed(), (unsigned)s_placedSerial);
    mp_log("world reg chips plain=%d carried=%d", s_regChipPlain, s_regChipCarried);
    if (s_pool.overflowed()) mp_log("world pool ranges overflow n=%d", s_pool.count());
    s_inPlacementDone = true;
    restoreReal();                 // a reload after a miss gets this serial's synced switches back at once
    s_inPlacementDone = false;
    s_carryPass = false;
    s_carryDone = 0;
    if (!worldOn()) return;        // reconnecting during placement: update() sends REGCRC repair as soon as mp_active() returns
    if (mp_is_host() && regUsable() && !carryMayApply(true, othersInScene())) {
        if (s_mem.get()->carry.valid) mp_log("world reload carry dropped (no other player in this scene: a fresh scene)");
        carryClear(&s_mem.get()->carry);
    }
    if (mp_is_host() && regUsable() && s_mem.get()->carry.valid) {
        // hostreload: the host reloaded its own scene (same serial): what was defeated / collected here before, by anyone, is put back (the
        // guests did not reload and still have it; the host is the reference, so it must not show it alive or hand a joiner an empty state).
        // Not at once (the scene is not live and the pending queue holds 64): update() puts kCarryPerTurn bits back per live turn.
        if (carryBound(s_mem.get()->carry, s_placedSerial, reg_().crc())) {
            s_carryPass = true;
            mp_log("world reload carry armed kill=%u purple=%u chip=%u", (unsigned)s_mem.get()->carry.st.count[0], (unsigned)s_mem.get()->carry.st.count[1],
                   (unsigned)s_mem.get()->carry.st.count[2]);
        } else {
            mp_log("world reload carry dropped (registry changed crc=%08x was=%08x)", (unsigned)reg_().crc(), (unsigned)s_mem.get()->carry.regCrc);
            carryClear(&s_mem.get()->carry);
        }
    }
    s_regCrcSent = true;
    s_needReconnectRepair = false;
    sendRegCrc(-1);                // guest: to the host (also asks for the map); host: to everyone
    if (mp_is_host()) {
        for (int i = 0; i < MP_MAX_PLAYERS; i++) {
            if (!s_guestNeedsReply[i]) continue;
            s_guestNeedsReply[i] = false;
            sendMap(i, true);
            sendObjMap(i, true);
        }
    } else {
        checkHostCrc();
    }
}

void classifySwitch(const SwitchIdInfo* info, const char* switchName, const JMapInfoIter& iter) {
    if (!info || !s_placing) return;
    const char* name = nullptr;
    if (!MR::getObjectName(&name, iter) || !name) return;
    s32 arg0 = -1;
    bool hasArg0 = iter.getValue< s32 >("Obj_arg0", &arg0);
    if (switchPolicy(name, switchName, hasArg0, arg0) == SW_LOCAL) markLocalInfo(info);
}

namespace {
    void edgeSlotBit(int slot, int bit, bool on);
}

void onSwitchEdge(const SwitchIdInfo& info, bool on) {
    if (!s_placed || s_applying || !worldOn()) return;
    if (s_placedSerial != mp_scene_serial() || !MpGame::isGameSceneLive() || s_liveTurns < kLiveWarmupTurns) return;
    if (isLocalInfo(&info)) return;
    int slot, bit;
    if (!switchSlotBit(info, &slot, &bit)) return;
    edgeSlotBit(slot, bit, on);
}

namespace {
void edgeSlotBit(int slot, int bit, bool on) {
    bool tripped = false;
    if (mir_().oscEdge(slot, bit, s_turn, &tripped)) {
        if (tripped) mp_log("world osc-guard zone=%d bit=%d", slotWire(slot), bit);
        return;
    }
    mir_().setOn(slot, bit, on);
    uint8_t buf[kSwitchLen];
    SwitchMsg m = {s_placedSerial, (uint8_t)slotWire(slot), (uint8_t)bit, (uint8_t)(on ? 1 : 0)};
    size_t n = writeSwitch(buf, sizeof buf, m);
    mp_send(MP_MSG_SWITCH, buf, (uint16_t)n, true);
    mp_log("world switch zone=%d bit=%d on=%d src=local", slotWire(slot), bit, (int)on);
}

#if defined(TARGET_PC) && !defined(__vita__)
// MP3 (harness test support, desktop only): SMG_MP_TEST_SWITCH="<secs>:<bit>" turns on switch <bit> (0-127) of the main zone,
// "<secs>:g<bit>" the global switch <bit>, <secs> of live play after each scene start, through the same edge path as a switch
// the game turns on (PETARI_SWITCH writes the container bit directly, so the bridge never sees it). Once per scene.
void testSwitchTick() {
    static int sOn = -1, sBit = -1;
    static bool sGlobal = false;
    static uint32_t sTurns = 0, sDoneSerial = 0xFFFFFFFFu;
    if (sOn < 0) {
        const char* e = getenv("SMG_MP_TEST_SWITCH");
        sOn = 0;
        float secs = 0;
        int bit = -1;
        if (e && *e && (sscanf(e, "%f:g%d", &secs, &bit) == 2 || sscanf(e, "%f:%d", &secs, &bit) == 2) && bit >= 0 && bit < kBits) {
            sGlobal = strchr(e, 'g') != nullptr;
            sBit = bit;
            sTurns = (uint32_t)(secs * 60.0f);
            sOn = 1;
            mp_log("world test switch: %s bit %d after %u live turns", sGlobal ? "global" : "zone 0", sBit, (unsigned)sTurns);
        }
    }
    if (!sOn || s_liveTurns < sTurns || s_liveTurns < kLiveWarmupTurns || sDoneSerial == s_placedSerial) return;
    sDoneSerial = s_placedSerial;
    const int slot = sGlobal ? (int)kGlobalSlot : zoneSlot(0);
    if (slot < 0) return;
    ZoneSwitch* z = realZone(slot);
    if (!z || z->get(sBit)) return;
    z->set(sBit, true);
    mp_log("world test switch: on zone=%d bit=%d serial=%u", slotWire(slot), sBit, (unsigned)s_placedSerial);
    edgeSlotBit(slot, sBit, true);
}

// MP3 worldsync (harness test support, desktop only): SMG_MP_TEST_KILL="<secs>:<n>" defeats <n> live enemies of the registry <secs> of live play after
// each scene start, through the game's own makeActorDead (so the KILL goes out exactly as for a defeat by the player). Once per scene.
void testKillTick() {
    static int sOn = -1, sCount = 0;
    static uint32_t sTurns = 0, sDoneSerial = 0xFFFFFFFFu;
    if (sOn < 0) {
        sOn = 0;
        const char* e = getenv("SMG_MP_TEST_KILL");
        float secs = 0;
        int n = 0;
        if (e && *e && sscanf(e, "%f:%d", &secs, &n) == 2 && n > 0 && n <= 200) {
            sCount = n;
            sTurns = (uint32_t)(secs * 60.0f);
            sOn = 1;
            mp_log("world test kill: %d enemies after %u live turns", n, (unsigned)sTurns);
        }
    }
    if (!sOn || s_liveTurns < sTurns || s_liveTurns < kLiveWarmupTurns || sDoneSerial == s_placedSerial || !regUsable()) return;
    sDoneSerial = s_placedSerial;
    int killed = 0;
    for (int i = 0; i < reg_().count() && killed < sCount; i++) {
        Entry& e = reg_().at(i);
        if (!(e.flags & ENTRY_ENEMY) || (e.flags & ENTRY_DUP)) continue;
        LiveActor* a = (LiveActor*)e.actor;
        if (!a || MR::isDead(a)) continue;
        a->makeActorDead();
        killed++;
    }
    mp_log("world test kill: killed=%d serial=%u", killed, (unsigned)s_placedSerial);
}
// MP3 worldsync (harness test support, desktop only): SMG_MP_TEST_COLLECT="<secs>:<purple>:<chips>" collects up to <purple> Purple Coins and <chips> Star Chips
// of the registry <secs> of live play after each scene start, through the game's own pickup steps (Coin::mpRemoteGet / ChipBase::mpRemoteGet run
// noticeGetCoin / noticeGetChip exactly as an ItemGet does, which is what calls onPurpleCoin / onChip): to world sync it is an ordinary pickup (GOT flag,
// carry, PURPLE / CHIP sent to whoever is there). Retries every turn for 10 s (a coin or chip that has not appeared yet is skipped, not counted).
void testCollectTick() {
    static int sOn = -1, sWantP = 0, sWantC = 0;
    static uint32_t sTurns = 0, sDoneSerial = 0xFFFFFFFFu, sTries = 0, sTrySerial = 0xFFFFFFFFu;
    static int sGotP = 0, sGotC = 0;
    if (sOn < 0) {
        sOn = 0;
        const char* e = getenv("SMG_MP_TEST_COLLECT");
        float secs = 0;
        int np = 0, nc = 0;
        if (e && *e && sscanf(e, "%f:%d:%d", &secs, &np, &nc) == 3 && np >= 0 && nc >= 0 && np <= 200 && nc <= 200 && np + nc > 0) {
            sWantP = np;
            sWantC = nc;
            sTurns = (uint32_t)(secs * 60.0f);
            sOn = 1;
            mp_log("world test collect: %d purple coins, %d chips after %u live turns", np, nc, (unsigned)sTurns);
        }
    }
    if (!sOn || s_liveTurns < sTurns || s_liveTurns < kLiveWarmupTurns || sDoneSerial == s_placedSerial || !regUsable()) return;
    if (sTrySerial != s_placedSerial) { sTrySerial = s_placedSerial; sTries = 0; sGotP = sGotC = 0; }
    int regP = 0, regC = 0;
    for (int i = 0; i < reg_().count(); i++) {
        Entry& e = reg_().at(i);
        if (e.flags & ENTRY_DUP) continue;
        LiveActor* a = (LiveActor*)e.actor;
        if (!a) continue;
        if (e.flags & ENTRY_PURPLE) {
            regP++;
            if (sGotP < sWantP && !(e.flags & ENTRY_GOT) && ((Coin*)a)->mpRemoteGet()) sGotP++;
        } else if (e.flags & ENTRY_CHIP) {
            regC++;
            if (sGotC < sWantC && !(e.flags & ENTRY_GOT) && ((ChipBase*)a)->mpRemoteGet()) sGotC++;
        }
    }
    if (sTries++ == 0) {
        int groups[2] = {-1, -1}, chips[2] = {0, 0};   // (what the scene has, registered or not: -1 = no holder)
        const int ids[2] = {SceneObj_BlueChipHolder, SceneObj_YellowChipHolder};
        for (int t = 0; t < 2; t++) {
            if (!MR::isExistSceneObj(ids[t])) continue;
            ChipHolder* h = MR::getSceneObj< ChipHolder >(ids[t]);
            groups[t] = h ? (int)h->mNumChipGroups : 0;
            for (s32 g = 0; h && h->mChipGroups && g < h->mNumChipGroups; g++) {
                if (h->mChipGroups[g]) chips[t] += (int)h->mChipGroups[g]->mTotalCount;
            }
        }
        mp_log("world test collect: registry purple=%d chip=%d serial=%u; scene chip groups blue=%d (%d chips) yellow=%d (%d chips)", regP, regC, (unsigned)s_placedSerial,
               groups[0], chips[0], groups[1], chips[1]);
    }
    if ((sGotP >= sWantP && sGotC >= sWantC) || sTries > 600) {
        sDoneSerial = s_placedSerial;
        mp_log("world test collect: purple=%d chip=%d (registry purple=%d chip=%d) serial=%u", sGotP, sGotC, regP, regC, (unsigned)s_placedSerial);
    }
}
#endif
}  // namespace

void onActorDead(LiveActor* actor) {
    if (!s_placed || s_applying || !actor || MR::isDead(actor)) return;
    if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && s_placedSerial == __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED)) return;
    if (!worldOn() || !MpGame::isGameSceneLive() || !mp_settings()->sharedKills || !objSyncLocal()) return;
    int idx = reg_().findActor(actor);
    if (idx < 0) return;
    const Entry& e = reg_().at(idx);
    if (!(e.flags & ENTRY_ENEMY) || (e.flags & ENTRY_DUP)) return;
    reg_().at(idx).flags |= ENTRY_KILLED;   // worldsync: this console's own defeat is part of the state it hands to a joiner (if it becomes host)
    noteCarry(OM_KILL, idx);
    sendObj(MP_MSG_KILL, e.key);
    logKey("kill", e.key, "local");
}

void onPurpleCoin(LiveActor* coin) {
    if (!s_placed || s_applying || !coin) return;
    if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && s_placedSerial == __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED)) return;
    int idx = reg_().findActor(coin);
    if (idx < 0) return;
    Entry& e = reg_().at(idx);
    if (!(e.flags & ENTRY_PURPLE) || (e.flags & ENTRY_DUP)) return;
    e.flags |= ENTRY_GOT;
    noteCarry(OM_PURPLE, idx);
    if (!worldOn() || !MpGame::isGameSceneLive() || !(mp_settings()->flags & 2) || !objSyncLocal()) return;
    sendObj(MP_MSG_PURPLE, e.key);
    logKey("purple", e.key, "local");
}

void onChip(LiveActor* chip) {
    if (!s_placed || s_applying || !chip) return;
    if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && s_placedSerial == __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED)) return;
    int idx = reg_().findActor(chip);
    if (idx < 0) return;
    Entry& e = reg_().at(idx);
    if (!(e.flags & ENTRY_CHIP) || (e.flags & ENTRY_DUP)) return;
    s_lastChipRemote = false;
    e.flags |= ENTRY_GOT;
    noteCarry(OM_CHIP, idx);
    if (!worldOn() || !MpGame::isGameSceneLive() || !(mp_settings()->flags & 4) || !objSyncLocal()) return;
    sendObj(MP_MSG_CHIP, e.key);
    logKey("chip", e.key, "local");
}

// chipsync: one "[mp] world chip group done type=<0 blue|1 yellow> id=<group arg> src=local|remote appear=0|1" line per chip group and scene when the group is complete on
// this console (appear=0), and again when its completion demo has ended and the group's switch A (the launch star / reward) is on (appear=1). src: the last chip came from
// this console's player or from another console. Read only; a handful of groups, one flag test each per turn.
static void chipGroupTick() {
    static uint16_t sSerial = 0xFFFF;
    static uint8_t sDone[2], sAppear[2];   // bit per group index
    if (sSerial != s_placedSerial) { sSerial = (uint16_t)s_placedSerial; sDone[0] = sDone[1] = sAppear[0] = sAppear[1] = 0; }
    const int ids[2] = {SceneObj_BlueChipHolder, SceneObj_YellowChipHolder};
    for (int t = 0; t < 2; t++) {
        if (!MR::isExistSceneObj(ids[t])) continue;
        ChipHolder* h = MR::getSceneObj< ChipHolder >(ids[t]);
        for (s32 g = 0; h && h->mChipGroups && g < h->mNumChipGroups && g < 8; g++) {
            ChipGroup* grp = h->mChipGroups[g];
            if (!grp || grp->mTotalCount <= 0) continue;
            if (!(sDone[t] & (1u << g)) && grp->isComplete()) {
                sDone[t] |= (uint8_t)(1u << g);
                mp_log("world chip group done type=%d id=%d src=%s appear=0", t, (int)grp->mStageSwitchArg, s_lastChipRemote ? "remote" : "local");
            }
            if ((sDone[t] & (1u << g)) && !(sAppear[t] & (1u << g)) && grp->mSwitchCtrl && grp->mSwitchCtrl->mSW_A && grp->mSwitchCtrl->isOnSwitchA()) {
                sAppear[t] |= (uint8_t)(1u << g);
                mp_log("world chip group done type=%d id=%d src=%s appear=1", t, (int)grp->mStageSwitchArg, s_lastChipRemote ? "remote" : "local");
            }
        }
    }
}

void update() {
    s_turn++;
    if (!worldOn()) {
        if (s_wasActive) {
            s_wasActive = false;
            s_needReconnectRepair = true;
        }
        return;
    }
    s_wasActive = true;
    MP_PROF_SCOPE(MP_PROF_WORLD);
    checkEpoch();
    if ((s_needReconnectRepair || !s_regCrcSent) && s_placed && s_placedSerial == mp_scene_serial()) {
        s_needReconnectRepair = false;
        s_regCrcSent = true;
        s_pending.clear();
        s_early.clear();
        s_repairBudget.reset();
        s_objBudget.reset();
        s_mapMismatch = 0;
        s_objMismatch = 0;
        s_repairPending = true;
        if (!mp_is_host()) {
            mp_log("world reconnect repair serial=%u", (unsigned)s_placedSerial);
            sendRegCrc(-1, kRegCrcRepair);
        } else {
            sendRegCrc(-1);
        }
    }
    if (s_drops.summaryDue(s_turn)) {   // a console stuck not-placed / CRC-mismatched: one summary per 30 s, not a line per drop
        char line[200];
        s_drops.format(line, sizeof line, s_sceneName[0] ? s_sceneName : "-");
        mp_log("%s (running)", line);
    }
    if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && mp_scene_serial() != __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED)) __atomic_store_n(&s_restarting, false, __ATOMIC_RELEASE);
    if (s_mem.get() && s_mem.get()->carry.blocked && s_turn - s_carryDropTurn > (uint32_t)kCarryBlockTurns) {
        __atomic_store_n(&s_restarting, false, __ATOMIC_RELEASE);
        carryArm(&s_mem.get()->carry);   // (no placement followed the group restart: do not stay blocked)
        mp_log("world reload carry unblocked (no placement after the restart)");
        if (mp_is_host() && s_placed && s_placedSerial == mp_scene_serial()) {
            for (int i = 0; i < MP_MAX_PLAYERS; i++) {
                if (!s_guestNeedsReply[i]) continue;
                s_guestNeedsReply[i] = false;
                sendMap(i, true);
                sendObjMap(i, true);
            }
        }
    }
    if (!s_placed || s_placedSerial != mp_scene_serial()) return;
    checkHostCrc();
    checkGuestCrcs();
    bool live = MpGame::isGameSceneLive();
    if (live && ++s_liveTurns == 1) restoreReal();   // bits that arrived between placement and the Action state
#if defined(TARGET_PC) && !defined(__vita__)
    if (live) testSwitchTick();   // MP3 (harness): SMG_MP_TEST_SWITCH, desktop test hook; off unless set
    if (live) testKillTick();     // MP3 worldsync (harness): SMG_MP_TEST_KILL, desktop test hook; off unless set
    if (live) testCollectTick();  // MP3 worldsync (harness): SMG_MP_TEST_COLLECT, desktop test hook; off unless set
#endif
    if (live) chipGroupTick();    // chipsync: the group-complete lines
    if (s_pending.count() > 0 && live) {
        s_pending.run(s_turn, [](const PendingQueue::Item& it) -> bool {
            if (it.index >= reg_().count()) return true;
            switch (it.kind) {
            case PEND_KILL: return applyKill(it.index);
            case PEND_PURPLE: return applyPurple(it.index);
            case PEND_CHIP: return applyChip(it.index);
            default: return true;
            }
        }, [](const PendingQueue::Item& it) { dropIdx(pendType(it.kind), DR_EXPIRED, it.index); });
    }
    if (s_carryPass && live && mp_is_host() && regUsable()) {
        ObjCarry& cr = s_mem.get()->carry;
        carryPrune(&cr, reg_(), actorDeadNow);
        int left = 0;
        s_carryDone += carryPass(&cr, reg_(), shareMask(), kCarryPerTurn, carryTry, nullptr, &left);
        if (left == 0 || s_liveTurns > (uint32_t)kCarryMaxLiveTurns) {
            s_carryPass = false;
            if (left > 0) mp_log("world reload carry gave up with %d bits outstanding", left);
            if (s_carryDone > 0) {
                mp_log("world reload carry applied n=%d (kill/purple/chip back after the host's own reload)", s_carryDone);
                sendObjMap(-1, true);   // everyone gets the state again (the guests that did not reload skip what they have)
            }
        }
    }
    if (s_early.count() > 0 && live) {
        s_early.run(s_turn, [](const EarlyQueue::Item& it) -> int {
            ObjMsg o;
            o.serial = it.serial;
            o.key = it.key;
            return processObj(it.type, it.fromSlot, o) ? EarlyQueue::EQ_DONE : EarlyQueue::EQ_KEEP;
        }, [](const EarlyQueue::Item& it) { drop(it.type, DR_EXPIRED, &it.key, it.serial); });
    }
    // Host: 1 s mirror CRC so guests notice a missed edge.
    // (a skipped cutscene runs many turns per frame: by turns this was ~10 CRC messages a second to every guest)
    const int64_t nowNs = port_host_time_ns();
    if (mp_is_host() && live && !__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && (nowNs - s_lastMapCheckNs >= (int64_t)kMapCheckTurns * 16666667 || nowNs < s_lastMapCheckNs)) {
        s_lastMapCheckNs = nowNs;
        s_lastMapCheckTurn = s_turn;
        sendMap(-1, false);
        sendObjMap(-1, false);   // worldsync: the object state summary rides the same 1 s check
    }
}

void handle(const MpMsg& m) {
    if (!worldOn()) return;
    MP_PROF_SCOPE(MP_PROF_WORLD);
    checkEpoch();
    int zoneCount = s_placed ? s_zoneCount : kGlobalSlot;   // before placement, bounds only (kept in the mirror)
    uint16_t serial = mp_scene_serial();
    switch (m.type) {
    case MP_MSG_SWITCH: {
        SwitchMsg s;
        if (!readSwitch(m.data, m.len, zoneCount, &s)) { drop(m.type, DR_MALFORMED); return; }
        if (s.serial != serial) { drop(m.type, DR_SERIAL, nullptr, s.serial); return; }
        if (!senderAllowed(m.fromSlot, MpWorldGuard::K_SWITCH, "SWITCH")) { drop(m.type, DR_RATE); return; }
        if (!ensureMem()) { drop(m.type, DR_NO_MEM); return; }
        syncMirrorSerial(serial);
        int slot = zoneSlot(s.zone);
        mir_().setOn(slot, s.bit, s.on != 0);
        if (!mir_().isGuarded(slot, s.bit)) writeReal(slot, s.bit, s.on != 0);   // idempotent
        mp_log("world switch zone=%d bit=%d on=%d src=remote", (int)s.zone, (int)s.bit, (int)s.on);
        return;
    }
    case MP_MSG_SWITCHMAP: {
        if (!isHostSlot(m.fromSlot) || mp_is_host()) { drop(m.type, DR_NOT_HOST); return; }
        static MapMsg map;   // ~600 B: keep it off the stack
        if (!readMap(m.data, m.len, zoneCount, &map)) { drop(m.type, DR_MALFORMED); return; }
        if (map.serial != serial) { drop(m.type, DR_SERIAL, nullptr, map.serial); return; }
        if (!ensureMem()) { drop(m.type, DR_NO_MEM); return; }
        syncMirrorSerial(serial);
        if (mapIsFull(map)) {
            bool auth = s_repairPending;
            s_repairPending = false;
            int changed = applyMap(mir_(), map, auth, applyCb, nullptr);
            restoreReal();
            s_mapMismatch = 0;
            // the host repeats the map about once a second: log the first of each scene, repairs and real changes only
            static int s_mapLoggedSerial = -1;
            if (changed > 0 || auth || s_mapLoggedSerial != (int)serial) {
                s_mapLoggedSerial = (int)serial;
                mp_log("world switchmap applied zones=%d changed=%d mode=%s", (int)map.count, changed, auth ? "auth" : "on");
            }
            return;
        }
        // CRC only: compare after our own placement. Two checks in a row (2 s: in-flight edges have landed) => ask for a map.
        if (!s_placed || s_placedSerial != serial) return;
        if (map.crc == mir_().crc()) {
            s_mapMismatch = 0;
            s_repairBudget.reset();
            return;
        }
        if (++s_mapMismatch >= 2 && s_turn - s_lastRepairTurn >= kRepairGapTurns && s_repairBudget.take(s_turn)) {
            s_lastRepairTurn = s_turn;
            s_repairPending = true;
            s_mapMismatch = 0;
            mp_log("world switchmap mismatch host=%08x mine=%08x -> request", map.crc, mir_().crc());
            sendRegCrc(-1, kRegCrcRepair);   // REGCRC with the repair flag doubles as the map request
        }
        return;
    }
    case MP_MSG_REGCRC: {
        RegCrcMsg r;
        if (!readRegCrc(m.data, m.len, &r)) { drop(m.type, DR_MALFORMED); return; }
        if (r.serial != serial) { drop(m.type, DR_SERIAL, nullptr, r.serial); return; }
        if (mp_is_host()) {
            int slot = m.fromSlot;
            if (slot < 0 || slot >= MP_MAX_PLAYERS || isHostSlot(slot)) return;
            s_guestCrcKnown[slot] = true;
            s_guestCrcSerial[slot] = r.serial;
            s_guestCrc[slot] = r.crc;
            if (replyDeferred(s_placed && s_placedSerial == serial, s_mem.get() && s_mem.get()->carry.blocked)) {
                s_guestNeedsReply[slot] = true;   // answered at our placementDone (REGCRC goes to everyone there)
                return;
            }
            const bool repair = (r.flags & kRegCrcRepair) != 0;
            if (!(repair ? s_replyGate.allowRepair(slot, port_host_time_ns()) : s_replyGate.allow(slot, r.crc, port_host_time_ns()))) { drop(m.type, DR_RATE); return; }   // (a repeat: 6 bytes in, ~570 out)
            sendRegCrc(slot);
            sendMap(slot, true);
            sendObjMap(slot, true);
        } else {
            if (!isHostSlot(m.fromSlot)) return;
            s_hostCrcState.received(r.serial, r.crc);
        }
        return;
    }
    case MP_MSG_OBJMAP: {
        if (!isHostSlot(m.fromSlot) || mp_is_host()) { drop(m.type, DR_NOT_HOST); return; }
        ObjMapMsg om;
        if (!readObjMap(m.data, m.len, &om)) { drop(m.type, DR_MALFORMED); return; }
        if (om.serial != serial) { drop(m.type, DR_SERIAL, nullptr, om.serial); return; }
        if (!s_placed) { drop(m.type, DR_NOT_PLACED); return; }   // (no registry to apply to: the host's next summary / our REGCRC asks again)
        if (!regUsable()) { drop(m.type, DR_REG_OFF); return; }
        if (om.regCrc != reg_().crc()) { drop(m.type, DR_CRC_MISMATCH); return; }
        if (om.kind == OM_SUMMARY) { onObjSummary(om); return; }
        const int calls = applyObjChunk(om, reg_().count(), applyObjBit, nullptr);
        mp_log("world objmap applied kind=%d start=%d bits=%d", (int)om.kind, (int)om.start, calls);
        return;
    }
    case MP_MSG_KILL:
    case MP_MSG_PURPLE:
    case MP_MSG_CHIP: {
        ObjMsg o;
        if (!readObj(m.data, m.len, zoneCount, &o)) { drop(m.type, DR_MALFORMED); return; }
        if (!processObj(m.type, m.fromSlot, o)) deferObj(m.type, m.fromSlot, o);   // worldsync: not judgeable yet -> the early queue, never a silent drop
        return;
    }
    default:
        return;
    }
}

bool holdEarly(const MpMsg& m) {   // MpGame.cpp route(): a world message while the scene is not live; objects wait in the early queue (not the 24-entry pool)
    if (!worldOn() || (m.type != MP_MSG_KILL && m.type != MP_MSG_PURPLE && m.type != MP_MSG_CHIP)) return false;
    ObjMsg o;
    if (!readObj(m.data, m.len, s_placed ? s_zoneCount : (int)kGlobalSlot, &o)) { drop(m.type, DR_MALFORMED); return true; }
    if (__atomic_load_n(&s_restarting, __ATOMIC_ACQUIRE) && o.serial == __atomic_load_n(&s_restartSerial, __ATOMIC_RELAXED)) { drop(m.type, DR_SERIAL, &o.key, o.serial); return true; }
    deferObj(m.type, m.fromSlot, o);
    return true;
}

void noteHeldDrop(uint8_t msgType) {   // MpGame.cpp: the held pool (scene not live yet) refused a world message
    if (worldOn()) drop(msgType, DR_HELD_FULL);
}

void onPeerJoined(int slot) {
    if (slot >= 0 && slot < MP_MAX_PLAYERS) s_replyGate.resetSlot(slot);
}

void endScene() {
    if (worldHeld() && (s_placed || s_drops.total() > 0)) {
        char line[200];
        s_drops.format(line, sizeof line, s_sceneName[0] ? s_sceneName : "-");
        mp_log("%s", line);
    }
    s_drops.reset();
    if (!worldHeld()) {
        s_early.clear();   // the session is over; during a session (or reconnect) the next scene events wait here
        __atomic_store_n(&s_restarting, false, __ATOMIC_RELEASE);
        s_needReconnectRepair = false;
        s_regCrcSent = false;
        s_wasActive = false;
    }
    s_sceneName[0] = 0;
    s_limits.reset();
    s_replyGate.reset();
    for (int i = 0; i < MP_MAX_PLAYERS; i++) s_refused[i] = 0;
    s_placing = false;
    s_placed = false;
    s_inPlacementDone = false;
    s_applying = false;
    s_pending.clear();
    clearLocalInfo();
    if (s_mem.get()) {
        reg_().clear();
        mir_().resetScene();   // on-bits stay with their serial (a reload after a miss restores them)
        if (!worldHeld()) releaseMem();   // (optfix: the session is over)
    }
    s_liveTurns = 0;
}

}  // namespace MpWorldSync
