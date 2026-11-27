// Galaxy 3.0 multiplayer: saves, game side (owner: save). See decomp/include/Game/MP/MpSaveGame.hpp and DESIGN.md section 1.
//
// The game keeps the whole GameData.bin in SaveDataHandler::_18 (read once at boot; every save writes the whole image to
// <save root>/title/00010000/524d4750/data/GameData.bin through the NAND layer). Switching the save root therefore means: no save
// sequence running, port_nand_set_root(new root) (host-heap string, nand.cpp), then the image is rebuilt exactly as the game builds
// it (SaveDataHandler::resetSaveData + initializeAllFileInSaveData from a reset UserFile) and the new root's file is copied in the
// way SaveDataHandleSequence::exePreLoadDone does (copySaveDataEachFile + restoreSysConfigFile), after a stricter host-side check
// (mp_save_validate: version, count, size, length, checksum, table bounds). Synchronous: no NAND error dialogs (a broken co-op
// file is kept aside and a fresh image is used), no wait on the save sequence's nerves. A self-check compares the loaded blocks
// with the file and the NAND root with the expected root; on a mismatch the previous root is restored.
// Fallback if a run-time switch ever proves unsafe: relaunch with an argument (vita/vgx-game/settings_runtime.cpp::Relaunch, i.e.
// sceAppMgrLoadExec) and let port_boot set the multiplayer root; not needed so far.
#include <errno.h>
#include "Game/MP/MpSaveGame.hpp"
#include <cstdlib>

#include "Game/MP/MpGame.hpp"
#include "Game/MP/MpMessages.hpp"
#include "Game/MP/MpSaveGate.hpp"
#include "Game/System/GalaxyStatusAccessor.hpp"
#include "Game/System/GameDataHolder.hpp"
#include "Game/System/GameSequenceDirector.hpp"
#include "Game/System/GameSequenceFunction.hpp"
#include "Game/System/ScenarioDataParser.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/SaveDataHandleSequence.hpp"
#include "Game/System/SaveDataHandler.hpp"
#include "Game/System/SysConfigFile.hpp"
#include "Game/System/UserFile.hpp"
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/JMapInfo.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "port/heap_routing.h"
#include "../../../../platform/src/mp/mp_savecheck.h"
#include "../../../../platform/src/mp/mp_savefile.h"
#include "../../../../platform/src/mp/mp_config.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>
#include "port/mp_test_env.h"

extern "C" int port_nand_get_root(char* out, int cap);       // platform/src/nand/nand.cpp
extern "C" int port_nand_get_boot_root(char* out, int cap);  // the single-player root port_boot set
extern "C" void port_nand_set_root(const char* root);
extern "C" void port_host_job_wait(const char* what, void (*fn)(void*), void* arg);  // platform/src/dvd/dvd.cpp
extern "C" bool port_nand_all_atomic(void);   // smallfix: the "Safe saves" setting (SMG_ATOMIC_SAVES), single-player writes included
extern "C" void port_nand_set_atomic_writes(bool on);  // savefix: the game's GameData.bin writes through the atomic writer (nand.cpp)

namespace {

    uint32_t sLastAppliedCrc = 0;      // glitchfix S2: crc of the snapshot applied last, and the session it was applied in
    uint32_t sLastAppliedSession = 0;
    // fix2: the image was reloaded from disk (root switch, root reload, keepsafe reload): what was applied is gone from RAM, so the
    // identical-snapshot skip must forget it, else a re-offered snapshot with the same crc is ignored and the progress never comes back
    void forgetAppliedSnapshot() { sLastAppliedCrc = 0; sLastAppliedSession = 0; }
    // the save gate's flags and rules: decomp/include/Game/MP/MpSaveGate.hpp (host-tested, tests/mp/savegate_test.cpp)
    MpSaveGate::State sG;
    const char kDataDir[] = "/title/00010000/524d4750/data";
    const int kBackupsKept = 10;
    const int kBackupListMax = 16;
    const unsigned kImageBytes = MP_SAVE_MAX_FILE;  // the game's _14/_18 buffers are 0x10000

    bool sPathsReady = false;
    // savefix (review finding 7): a root switch failed after it had changed the NAND root or the image, and the way back failed
    // too (or could not be verified): the image may be half-built or belong to another root. Every save (the game's, ours) is
    // refused until a switch / reload succeeds and passes its self-check.
    bool sUnsafe = false;
    // savefix (review finding 1): the image's generation: +1 at every load of a root's file into the image (switch, reload,
    // restore). The file select remembers the generation it cached its files from (fileSelectOpened).
    unsigned sImageGen = 1;
    bool sFsOpen = false;
    unsigned sFsGen = 0;
    unsigned sFsRetry = 0;
    int sSessionSlot = 1;
    bool sSnapshotMario = true;
    // review: a session ended while the in-RAM image holds progress nobody chose to write (a guest's multiplayer image carries
    // the host's progress in all six files; a host without a save choice): the game's own saves stay suppressed until the image
    // is reloaded from disk (leaveSessionRoot at the file select / discardSessionProgress). Without this a save prompt after
    // the session (a star demo the session ended in, the reconnect window) wrote the host's progress over all six co-op files.
    char sSpRoot[256];
    char sSpGameData[320];     // smallfix: the single-player GameData.bin of the "Play on my file" session
    bool sSpBakBefore = true;  // smallfix: its .mpbak existed before the session (true when unknown)
    char sMpRoot[256];
    char sBackupDir[256];
    char sRecoveryDir[256];

    // Slow host file calls run on the host-job thread while the game thread sleeps (fsync on the SD card), like nand.cpp.
    template < class F >
    void hostJob(const char* what, F& f) {
        port_host_job_wait(what, [](void* p) { (*static_cast< F* >(p))(); }, &f);
    }

    // Two 64 KiB work images from the host heap for the duration of one operation (nothing per frame).
    struct Scratch {
        unsigned char* a;
        unsigned char* b;
        Scratch() {
            a = static_cast< unsigned char* >(port_host_alloc(kImageBytes, 32));
            b = static_cast< unsigned char* >(port_host_alloc(kImageBytes, 32));
            if (a) memset(a, 0, kImageBytes);
            if (b) memset(b, 0, kImageBytes);
        }
        ~Scratch() {
            if (a) port_host_free(a);
            if (b) port_host_free(b);
        }
        bool ok() const { return a != nullptr && b != nullptr; }
        Scratch(const Scratch&) = delete;
        Scratch& operator=(const Scratch&) = delete;
    };

    // fix2: the background recovery writer did not finish within its (generous) time: every path that needs the card is refused ("the file
    // can't be used right now": preparePaths false) until the writer is idle again. Before, the game thread waited for it with no limit.
    bool sWriterHung = false;
    bool preparePaths() {
        if (sWriterHung) {
            if (!mp_save_async_idle()) return false;
            sWriterHung = false;
            mp_log("save recovery writer finished late: the card is usable again");
        }
        if (sPathsReady) return true;
        char boot[256];
        if (port_nand_get_boot_root(boot, sizeof(boot)) <= 0) return false;
        snprintf(sSpRoot, sizeof(sSpRoot), "%s", boot);
        // ux0:data/smg/saves -> ux0:data/smg/mp/saves; <run-dir>/saves -> <run-dir>/mp/saves.
        char parent[256];
        snprintf(parent, sizeof(parent), "%s", boot);
        size_t n = strlen(parent);
        while (n > 1 && parent[n - 1] == '/') parent[--n] = 0;
        char* slash = strrchr(parent, '/');
        if (slash != nullptr) {
            *slash = 0;
            snprintf(sMpRoot, sizeof(sMpRoot), "%s/mp/saves", parent);
            snprintf(sBackupDir, sizeof(sBackupDir), "%s/mp/backups", parent);
            snprintf(sRecoveryDir, sizeof(sRecoveryDir), "%s/mp/recovery", parent);
        } else {
            snprintf(sMpRoot, sizeof(sMpRoot), "mp/saves");
            snprintf(sBackupDir, sizeof(sBackupDir), "mp/backups");
            snprintf(sRecoveryDir, sizeof(sRecoveryDir), "mp/recovery");
        }
        sPathsReady = true;
        return true;
    }

    void dataDirOf(const char* root, char (&out)[320]) { snprintf(out, sizeof(out), "%s%s", root, kDataDir); }
    void gameDataOf(const char* root, char (&out)[320]) { snprintf(out, sizeof(out), "%s%s/GameData.bin", root, kDataDir); }

    SaveDataHandleSequence* sequence() {
        GameSystem* sys = SingletonHolder< GameSystem >::get();
        if (sys == nullptr || sys->mSequenceDirector == nullptr) return nullptr;
        SaveDataHandleSequence* seq = sys->mSequenceDirector->mSaveDataHandleSequence;
        if (seq == nullptr || !seq->isInitializedGameDataHolder() || seq->mSaveDataHandler == nullptr) return nullptr;
        return seq;
    }

    // The save sequence (and with it every NAND request of the save files) is idle.
    SaveDataHandleSequence* idleSequence(const char* what) {
        SaveDataHandleSequence* seq = sequence();
        if (seq == nullptr) {
            mp_log("save %s refused: save system not ready", what);
            return nullptr;
        }
        if (seq->isActive()) return nullptr;  // retry next frame (no log: called every frame while waiting)
        return seq;
    }

    unsigned char* liveImage(SaveDataHandleSequence* seq) { return seq->mSaveDataHandler->_18; }

    // Read and fully validate a GameData.bin. Returns MP_SAVE_OK, -1 missing, -2 I/O error, or an MpSaveCheck code.
    int readGameData(const char* path, unsigned char* buf, unsigned* outLen) {
        int rc = 0;
        uint32_t len = 0;
        auto job = [&] { rc = mp_save_read_file(path, buf, kImageBytes, &len); };
        hostJob("MpSaveRead", job);
        *outLen = len;
        if (rc == -ENOENT) return -1;
        if (rc != 0) return rc == -EFBIG ? MP_SAVE_ERR_SIZE : -2;
        return mp_save_validate(buf, len, true, true);
    }

    int slotOfName(const char* name) {
        // "mario3" / "luigi3" / "config3"
        size_t n = strlen(name);
        if (n == 0) return -1;
        int d = name[n - 1] - '0';
        return d >= 1 && d <= MP_SAVE_SLOTS ? d : -1;
    }

    bool validSlot(int slot) { return slot >= 1 && slot <= MP_SAVE_SLOTS; }

    // Blank user-file blocks (what a never-created file serialises to): the slots of a fresh co-op GameData.bin.
    unsigned char sBlankGame[MP_SAVE_GAME_BLOCK];
    unsigned char sBlankConfig[MP_SAVE_CONFIG_BLOCK];
    unsigned char sKeepGame[MP_SAVE_GAME_BLOCK];
    unsigned char sKeepConfig[MP_SAVE_CONFIG_BLOCK];
    bool sBlankReady = false;

    void captureBlankFromReset(const UserFile* file) {
        memset(sBlankGame, 0, sizeof(sBlankGame));
        memset(sBlankConfig, 0, sizeof(sBlankConfig));
        file->makeGameDataBinary(sBlankGame, sizeof(sBlankGame));
        file->makeConfigDataBinary(sBlankConfig, sizeof(sBlankConfig));
        sBlankReady = true;
    }

    // Without a root switch yet: serialise the live user file, reset it, take the blank blocks, load the live data back (all
    // chunks are present, so the reload restores every field the reset touched).
    bool ensureBlank(SaveDataHandleSequence* seq) {
        if (sBlankReady) return true;
        UserFile* file = seq->mCurrentUserFile;
        char gameName[16], configName[16];
        snprintf(gameName, sizeof(gameName), "%s", file->getGameDataName());
        snprintf(configName, sizeof(configName), "%s", file->getConfigDataName());
        memset(sKeepGame, 0, sizeof(sKeepGame));
        memset(sKeepConfig, 0, sizeof(sKeepConfig));
        file->makeGameDataBinary(sKeepGame, sizeof(sKeepGame));
        file->makeConfigDataBinary(sKeepConfig, sizeof(sKeepConfig));
        const bool mario = file->mIsPlayerMario;
        file->resetAllData();
        captureBlankFromReset(file);
        file->loadFromGameDataBinary(gameName, sKeepGame, sizeof(sKeepGame));
        file->loadFromConfigDataBinary(configName, sKeepConfig, sizeof(sKeepConfig));
        file->mIsPlayerMario = mario;
        return true;
    }

    MpSaveSnapshotView blankView() {
        MpSaveSnapshotView v;
        v.block[0] = sBlankGame;
        v.block[1] = sBlankGame;
        v.block[2] = sBlankConfig;
        v.len[0] = v.len[1] = MP_SAVE_GAME_BLOCK;
        v.len[2] = MP_SAVE_CONFIG_BLOCK;
        v.slot = 1;
        v.flags = 0;
        v.crc = 0;
        return v;
    }

    // A fresh co-op base: the live image's table and sysconf, every slot blank (never another file's progress).
    bool blankBase(SaveDataHandleSequence* seq, unsigned char* out) {
        if (!ensureBlank(seq)) return false;
        memcpy(out, seq->mSaveDataHandler->_18, kImageBytes);
        MpSaveSnapshotView v = blankView();
        return mp_save_fill_blank_base(out, kImageBytes, &v);
    }

    // Make the game's save image the given root's GameData.bin (or a fresh image). seq must be idle.
    bool loadRootIntoImage(SaveDataHandleSequence* seq, const char* root, bool isMp, Scratch& s) {
        char path[320];
        gameDataOf(root, path);
        bool changed = false;
        bool repairOk = true;   // savelife review: the repair's answer (false: it could not decide, e.g. a card read error on a side file)
        auto recover = [&] {
            if (isMp) {
                char dir[320];
                dataDirOf(root, dir);
                mp_save_mkdirs(dir);
            }
            // review: on the single-player root only a missing GameData.bin is repaired here: an existing file the stricter host check
            // dislikes is never swapped for an older .mpbak behind the player's back. (smallfix: the boot repair, nand.cpp port_nand_set_root,
            // does replace a TORN single-player file from a valid .mptmp / .mpbak, once, at start-up.)
            struct stat st;
            if (isMp || stat(path, &st) != 0) repairOk = mp_save_recover(path, &changed);
        };
        hostJob("MpSaveRecover", recover);
        if (changed) mp_log("save recovered %s after an interrupted write", path);

        unsigned len = 0;
        int check = readGameData(path, s.a, &len);
        if (check == -1) {
            // savefix: missing, but an interrupted replace left the previous / next version beside it that the repair could not
            // put back (a failing rename): that is not "no file yet": refuse like a read error (a fresh image would replace it)
            struct stat st;
            char side[340];
            // smallfix: only a .mptmp proves an interrupted replace; a lone .mpbak is a save deleted outside the game
            snprintf(side, sizeof(side), "%s.mptmp", path);
            bool beside = stat(side, &st) == 0 || errno != ENOENT;   // (a stat error: cannot tell: treated as there)
            if (beside) check = -2;
        }
        if (isMp && check > 0 && !repairOk) {
            // savelife review: the file is there and invalid, the repair could not decide (it changed nothing: a side file could not be read) and
            // a .mptmp / .mpbak is beside it. Setting the torn file aside and starting fresh would let the first save truncate the unreadable
            // .mptmp or move the good .mpbak away: refuse the switch like a read error; the next try repairs once the card reads again.
            struct stat st;
            char side[340];
            snprintf(side, sizeof(side), "%s.mptmp", path);
            bool beside = stat(side, &st) == 0 || errno != ENOENT;   // (a stat error: cannot tell: treated as there)
            snprintf(side, sizeof(side), "%s.mpbak", path);
            beside = beside || stat(side, &st) == 0 || errno != ENOENT;
            if (beside) check = -2;
        }
        const bool valid = check == MP_SAVE_OK;
        if (check == MP_SAVE_ERR_VERSION && mp_save_is_newer(s.a, len)) check = -2;   // savefix (keepsafe S3): a newer build's file: refuse
        if (check == -2 || (!isMp && check > 0)) {   // savefix (finding 6): a read error refuses on the multiplayer root too
            // review: never a fresh image on the single-player root (the next save would write it over every single-player
            // file, e.g. after a transient read error): refuse the switch, nothing has been changed yet
            mp_log("save root %s: GameData.bin unusable (%s), not switching", isMp ? "mp" : "sp",
                   check > 0 ? mp_save_check_name(check) : "read error");
            return false;
        }
        if (check > 0 || check == -2) {
            mp_log("save root %s: GameData.bin unusable (%s), using a fresh image", isMp ? "mp" : "sp",
                   check > 0 ? mp_save_check_name(check) : "read error");
            if (isMp && check > 0) {
                // Keep the broken co-op file aside (never the single-player one: that stays exactly as it is).
                auto aside = [&] { mp_save_keep_aside(path); };   // savefix (keepsafe S2): earlier ones kept too
                hostJob("MpSaveAside", aside);
            }
        }
        if (valid) memset(s.a + len, 0, kImageBytes - len);

        port_nand_set_root(root);
        sUnsafe = true;   // savefix: from here a failure leaves the image / root unverified (cleared below on success)

        unsigned char* image = liveImage(seq);
        seq->mCurrentUserFile->resetAllData();
        captureBlankFromReset(seq->mCurrentUserFile);
        SaveDataHandler::resetSaveData(image);
        SaveDataHandler::initializeAllFileInSaveData(image, seq->mSysConfigFile, seq->mCurrentUserFile);
        if (valid) {
            SaveDataHandler::copySaveDataEachFile(image, s.a);
            seq->restoreSysConfigFile(seq->mSysConfigFile);
        }

        // Self-check: the NAND root is the new one, the image is well formed and holds the file's blocks.
        char now[256];
        if (port_nand_get_root(now, sizeof(now)) < 0 || strcmp(now, root) != 0) {
            mp_log("save root self-check failed: nand root");
            return false;
        }
        if (mp_save_validate(image, kImageBytes, false, false) != MP_SAVE_OK) {
            mp_log("save root self-check failed: image table");
            return false;
        }
        if (valid) {
            for (int slot = 1; slot <= MP_SAVE_SLOTS; slot++) {
                static const char* const kinds[3] = {"mario", "luigi", "config"};
                for (int k = 0; k < 3; k++) {
                    char name[16];
                    snprintf(name, sizeof(name), "%s%d", kinds[k], slot);
                    uint32_t fo, fs, io, is;
                    if (!mp_save_find(s.a, name, &fo, &fs) || !mp_save_find(image, name, &io, &is) ||
                        memcmp(s.a + fo, image + io, fs < is ? fs : is) != 0) {
                        mp_log("save root self-check failed: block %s", name);
                        return false;
                    }
                }
            }
        }
        sUnsafe = false;
        sImageGen++;
        return true;
    }

    // savefix: the game's own GameData.bin write goes through the atomic writer while multiplayer code is involved: the
    // multiplayer root, a running session, a "Play on my file" session (nand.cpp; single player: the original in-place write)
    void updateAtomicWrites() { port_nand_set_atomic_writes(sG.onMpRoot || sG.sessionActive || sG.spSessionSlot > 0); }

    bool switchRoot(bool toMp) {
        if (!preparePaths()) {
            mp_log("save root refused: no boot save root");
            return false;
        }
        if (!sUnsafe && MpSaveGate::switchIsNoop(sG, toMp)) return true;   // (a stale held image / an unverified one is reloaded)
        SaveDataHandleSequence* seq = idleSequence("root");
        if (seq == nullptr) return false;
        Scratch s;
        if (!s.ok()) {
            mp_log("save root refused: out of memory");
            return false;
        }
        if (!loadRootIntoImage(seq, toMp ? sMpRoot : sSpRoot, toMp, s)) {
            // Back to where we were (a full reload of that root's file).
            Scratch r;
            const bool touched = sUnsafe;   // (the failed load got as far as the root / the image)
            const bool back = touched && r.ok() && loadRootIntoImage(seq, sG.onMpRoot ? sMpRoot : sSpRoot, sG.onMpRoot, r);
            if (touched && !back) sUnsafe = true;   // savefix (finding 7): every save refused until a switch succeeds
            mp_log("save root %s kept (switch failed, restore %s)%s", sG.onMpRoot ? "mp" : "sp",
                   !touched ? "not needed" : back ? "ok" : "FAILED", sUnsafe ? ": saves blocked" : "");
            updateAtomicWrites();
            return false;
        }
        sG.onMpRoot = toMp;
        MpSaveGate::imageReloaded(sG);
        forgetAppliedSnapshot();
        updateAtomicWrites();
        mp_log("save root %s", toMp ? "mp" : "sp");
        return true;
    }

    int policy() {
        return mp_save_policy(mp_active(), mp_local_slot(), mp_host_slot(), mp_host_epoch(), mp_save_suppressed());
    }

    // The playing user file as three blocks: the playing character's game data and the config serialised now (live progress),
    // the other character's block as last stored in the image. Pointers into static buffers / the image.
    unsigned char sBlockGame[MP_SAVE_GAME_BLOCK];
    unsigned char sBlockConfig[MP_SAVE_CONFIG_BLOCK];
    bool liveBlocks(SaveDataHandleSequence* seq, MpSaveSnapshotView* v, int* slotOut, bool* marioOut) {
        UserFile* file = seq->mCurrentUserFile;
        const char* name = file->getGameDataName();
        const int slot = slotOfName(name);
        if (slot < 0) return false;
        const bool mario = strncmp(name, "mario", 5) == 0;
        memset(sBlockGame, 0, sizeof(sBlockGame));
        memset(sBlockConfig, 0, sizeof(sBlockConfig));
        file->makeGameDataBinary(sBlockGame, sizeof(sBlockGame));
        file->makeConfigDataBinary(sBlockConfig, sizeof(sBlockConfig));
        char other[16];
        snprintf(other, sizeof(other), "%s%d", mario ? "luigi" : "mario", slot);
        const unsigned char* image = seq->mSaveDataHandler->_18;
        uint32_t off = 0, size = 0;
        if (!mp_save_find(image, other, &off, &size) || size == 0) return false;
        if (size > MP_SAVE_GAME_BLOCK) size = MP_SAVE_GAME_BLOCK;
        v->block[0] = mario ? sBlockGame : image + off;
        v->len[0] = (uint16_t)(mario ? sizeof(sBlockGame) : size);
        v->block[1] = mario ? image + off : sBlockGame;
        v->len[1] = (uint16_t)(mario ? size : sizeof(sBlockGame));
        v->block[2] = sBlockConfig;
        v->len[2] = (uint16_t)sizeof(sBlockConfig);
        v->slot = (uint8_t)slot;
        v->flags = mario ? MP_SAVE_SNAP_FLAG_MARIO : 0;
        v->crc = 0;
        *slotOut = slot;
        *marioOut = mario;
        return true;
    }

    int viewStars(const MpSaveSnapshotView& v) {
        const int a = mp_save_block_stars(v.block[0], v.len[0]), b = mp_save_block_stars(v.block[1], v.len[1]);
        return (a < 0 ? 0 : a) + (b < 0 ? 0 : b);
    }

    // ---- the user-file validator's limits (snapval) ----------------------------------------------------------------------
    // Snapshots, recovery files and imported files go into the game's save parsers, which trust every size and count
    // (platform/src/mp/mp_savecheck.h). The validator needs what this build serializes (galaxies, flags, values: taken from our
    // own serialisation of the current user file) and the ScenarioData limits (power star and scenario bits per galaxy, the
    // story table's last progress). Built once, on first use; then a self-test: our own data and every file in the save image
    // must pass, or the limit checks are switched off (the structure checks, which keep the parsers in bounds, always stay).
    bool sCheckReady = false;
    unsigned char sCheckGame[MP_SAVE_GAME_BLOCK];
    unsigned char sCheckConfig[MP_SAVE_CONFIG_BLOCK];
    MpSaveLimits sCheckLimits;

    const char* checkBlockName(int block) { return block == 0 ? "mario" : block == 1 ? "luigi" : block == 2 ? "config" : "-"; }

    // The first file of the image (and our own serialisation) that fails the FULL check with `lim`; 0 when all pass.
    int checkSelfTest(SaveDataHandleSequence* seq, const MpSaveLimits* lim, char (&what)[48], MpSaveCheckResult* r) {
        int rc = mp_savecheck_game_block(sCheckGame, sizeof(sCheckGame), MP_SAVECHECK_FULL, lim, &r->at);
        if (rc == MP_SC_OK) rc = mp_savecheck_config_block(sCheckConfig, sizeof(sCheckConfig), MP_SAVECHECK_FULL, &r->at);
        if (rc != MP_SC_OK) {
            snprintf(what, sizeof(what), "own user file");
            return rc;
        }
        const unsigned char* image = seq->mSaveDataHandler->_18;
        for (int slot = 1; slot <= MP_SAVE_SLOTS; slot++) {
            MpSaveSnapshotView v;
            if (!mp_save_view_slot(image, slot, &v)) continue;
            rc = mp_savecheck_user_file(v.block, v.len, MP_SAVECHECK_FULL, lim, r);
            if (rc != MP_SC_OK) {
                snprintf(what, sizeof(what), "file %d %s", slot, checkBlockName(r->block));
                return rc;
            }
        }
        return MP_SC_OK;
    }

    void ensureCheckLimits(SaveDataHandleSequence* seq) {
        if (sCheckReady || seq == nullptr || seq->mCurrentUserFile == nullptr) return;
        sCheckReady = true;  // once per boot, whatever the outcome
        UserFile* file = seq->mCurrentUserFile;
        memset(sCheckGame, 0, sizeof(sCheckGame));
        memset(sCheckConfig, 0, sizeof(sCheckConfig));
        file->makeGameDataBinary(sCheckGame, sizeof(sCheckGame));
        file->makeConfigDataBinary(sCheckConfig, sizeof(sCheckConfig));
        if (!mp_savecheck_limits_from_block(sCheckGame, sizeof(sCheckGame), &sCheckLimits)) {
            mp_savecheck_install(nullptr);
            mp_log("save check limits unavailable (own data unreadable): structure and value checks only");
            return;
        }
        // ScenarioData: a scenario's PowerStarId is the mask of the stars it can give (getPowerStarNum counts the non-zero
        // ones); the star flags of a galaxy are bits 0..7 (star ids 1..8), the visited flags and coin records one per scenario.
        int masks = 0;
        for (ScenarioDataIter it = MR::makeBeginScenarioDataIter(); !it.isEnd(); it.goNext()) {
            const GalaxyStatusAccessor acc = it.makeAccessor();
            const ScenarioData* data = acc.mScenarioData;
            if (data == nullptr || data->mScenarioData == nullptr) continue;
            const int entries = data->mScenarioData->getNumEntries();
            u32 stars = 0;
            for (int no = 1; no <= entries; no++) {
                u32 id = 0;
                if (data->getValueU32("PowerStarId", no, &id)) stars |= id;
            }
            const u8 scenarios = entries >= 8 ? 0xFF : (u8)((1u << (entries > 0 ? entries : 0)) - 1);
            if (mp_savecheck_limits_set_galaxy(&sCheckLimits, (u16)MR::getHashCode(acc.getName()), (u8)(stars & 0xFF), scenarios)) masks++;
        }
        // The story table (GameDataHolder::isPassedStoryEvent): the largest "progress" is the end of the story.
        const JMapInfo* story = file->mGameDataHolder != nullptr ? file->mGameDataHolder->mMapInfo : nullptr;
        if (story != nullptr && story->mData != nullptr && story->getNumEntries() > 0) {
            u32 maxProgress = 0;
            bool ok = true;
            for (int i = 0; i < story->getNumEntries() && ok; i++) {
                u32 progress = 0;
                ok = story->getValue< u32 >(i, "progress", &progress);
                if (progress > maxProgress) maxProgress = progress;
            }
            if (ok && maxProgress <= 0xFF) {
                sCheckLimits.storyKnown = 1;
                sCheckLimits.maxStoryProgress = (uint16_t)maxProgress;
            }
        }
        char what[48];
        MpSaveCheckResult r = {MP_SC_OK, -1, 0};
        int rc = checkSelfTest(seq, &sCheckLimits, what, &r);
        if (rc == MP_SC_OK) {
            mp_savecheck_install(&sCheckLimits);
            mp_log("save check limits galaxies=%u gala=%u masks=%d flags=%u values=%u story=%d", (unsigned)sCheckLimits.galaxyCount,
                   (unsigned)sCheckLimits.galaCount, masks, (unsigned)sCheckLimits.flagCount, (unsigned)sCheckLimits.valueCount,
                   sCheckLimits.storyKnown ? (int)sCheckLimits.maxStoryProgress : -1);
            return;
        }
        // Our own data does not pass: the limits are wrong somewhere (never refuse a real file over it). Without limits.
        mp_savecheck_install(nullptr);
        mp_log("save check self-test failed: %s (%s at %u): limit checks off", what, mp_savecheck_name(rc), (unsigned)r.at);
        rc = checkSelfTest(seq, nullptr, what, &r);
        if (rc != MP_SC_OK) mp_log("save check self-test without limits failed too: %s (%s at %u)", what, mp_savecheck_name(rc), (unsigned)r.at);
    }

    // ---- session tracking and recovery checkpoints ----------------------------------------------------------------------
    const uint32_t kCheckpointTurns = 2 * 60 * 60;  // ~2 minutes of logic turns (60 per second)
    uint32_t sSessionId = 0;
    uint32_t sRecSeq = 0;
    uint32_t sTurns = 0;
    uint32_t sLastRecCrc = 0;
    int sRecIndex = -1;
    bool sAsyncUsed = false;
    int sPendingIndex = -1;
    unsigned char sRecBuf[MP_SAVE_REC_MAX];

    // fix2: wait (bounded) for the background writer; false = it is hung (sWriterHung: the card is refused until it ends)
    bool drainWriter(const char* where) {
        if (!sAsyncUsed || mp_save_async_wait()) return true;
        sWriterHung = true;
        mp_log("save recovery writer did not finish (%s): the file can't be used right now", where);
        return false;
    }

    bool writeCheckpoint(bool force, const char* why) {
        if (!sG.sessionSeen || sG.resolved || sUnsafe || !preparePaths()) return false;
        SaveDataHandleSequence* seq = sequence();
        if (seq == nullptr) return false;
        ensureCheckLimits(seq);  // snapval: the recovery file is checked like a snapshot when it is read back
        MpSaveSnapshotView v;
        int slot = 0;
        bool mario = true;
        if (!liveBlocks(seq, &v, &slot, &mario)) return false;
        MpSaveRecoveryInfo info;
        memset(&info, 0, sizeof(info));
        info.sessionId = sSessionId;
        info.seq = sRecSeq + 1;
        info.unixTime = (int64_t)time(nullptr);
        info.flags = (sG.originalHost ? MP_SAVE_REC_FLAG_ORIGINAL_HOST : 0) | (mario ? MP_SAVE_REC_FLAG_MARIO : 0);
        info.slot = (uint8_t)slot;
        if (sG.spSessionSlot > 0 && slot == sG.spSessionSlot) {   // savefix: only the chosen file's own progress has it as its source
            info.srcKind = MP_SAVE_SRC_SINGLE;
            info.srcSlot = (uint8_t)sG.spSessionSlot;
        } else if (sG.originalHost && sG.onMpRoot) {
            info.srcKind = MP_SAVE_SRC_COOP;  // the original host plays its own co-op file
            info.srcSlot = (uint8_t)slot;
        }
        info.stars = (uint16_t)viewStars(v);
        GameSystem* sys = SingletonHolder< GameSystem >::get();
        if (sys != nullptr && sys->mSceneController != nullptr) {
            const char* stage = MR::getCurrentStageName();
            int n = 0;
            for (; stage != nullptr && n < 32 && stage[n] >= 0x20 && stage[n] <= 0x7E; n++) info.galaxy[n] = stage[n];
            info.galaxy[n] = 0;
            const s32 scenario = MR::getCurrentScenarioNo();
            info.scenario = (uint8_t)(scenario >= 0 && scenario < 256 ? scenario : 0);
        }
        const uint32_t len = mp_save_recovery_pack(&info, &v, sRecBuf, sizeof(sRecBuf));
        MpSaveRecoveryInfo check;
        MpSaveSnapshotView packed;
        if (len == 0 || !mp_save_recovery_parse(sRecBuf, len, &check, &packed)) return false;
        if (!force && packed.crc == sLastRecCrc) return true;  // no progress since the last checkpoint
        if (sRecIndex < 0) {
            if (!drainWriter("checkpoint")) return false;   // savebug: an earlier session's checkpoint still being written must be listed
            auto job = [&] {
                mp_save_mkdirs(sRecoveryDir);
                sRecIndex = mp_save_recovery_next_index(sRecoveryDir);
            };
            hostJob("MpSaveRecIndex", job);
        }
        bool queued;
        {
            PortHostAllocScope scope;  // the writer thread is created on first use
            queued = mp_save_async_submit(sRecoveryDir, sRecIndex, sRecBuf, len, MP_SAVE_RECOVERY_KEEP);
        }
        if (!queued) return false;
        sAsyncUsed = true;
        sLastRecCrc = packed.crc;
        sRecSeq++;
        mp_log("save recovery checkpoint session=%d seq=%u bytes=%u stars=%u why=%s", sRecIndex, (unsigned)sRecSeq, (unsigned)len,
               (unsigned)info.stars, why);
        return true;
    }


    // ---- savefix: launch guard (review finding 2: the two entry paths that played on the single-player root without a choice) --
    // The save layer itself refuses, whatever the menu does: once the session is PLAYING, the original host must have made a save
    // choice (prepareHostSave: a co-op file, a copy, or "Play on my file" with its backup), and a console that joined as a guest
    // must be on the multiplayer root. Otherwise, after a short grace (the menu's / desktop autopilot's choice), it leaves the room
    // with a message. Its saves are held by the gate meanwhile, so nothing is written either way.
    const uint32_t kLaunchGraceHost = 720;    // 12 s (SMG_MP_AUTO_SAVE retries for 600 turns)
    const uint32_t kLaunchGraceGuest = 120;
    bool sStartedAsGuest = false;
    uint32_t sUnsafeTurns = 0;
    bool sLaunchRefused = false;

    // The gate's environment (MpSaveGate::sync / suppressed): the side effects of a session edge.
    struct GateEnv {
        bool netActive() { return mp_active(); }
        bool reconnecting() {
            MpUiStatus st;
            mp_ui_status(&st);
            return st.phase == MP_UI_RECONNECTING;
        }
        bool localIsOriginalHost() { return mp_local_slot() == 0 && mp_host_epoch() == 0; }
        int policy() { return ::policy(); }
        void onResumed() {
            // fix2: the same session again after a lost link (the core's silent rejoin): its recovery file goes on
            if (mp_host_epoch() != 0) {
                sG.originalHost = false;
                MpSaveGate::sourceLost(sG);   // savefix (finding 5)
            }
            mp_log("save session resumed index=%d", sRecIndex);
        }
        void onStart() {
            sStartedAsGuest = !sG.originalHost;   // savefix: launch guard
            sUnsafeTurns = 0;
            sLaunchRefused = false;
            sSessionId = (uint32_t)time(nullptr) ^ (MpGame::turn() << 12);
            sRecIndex = -1;
            sRecSeq = 0;
            sTurns = 0;
            sLastRecCrc = 0;
            // (perf) the recovery file's index needs a listing of the recovery folders on the memory card (the game thread
            // waits for the host-job thread): done here, at the session's first turn, not by the first timer checkpoint two
            // minutes into play (a mid-game hitch of the card's directory reads)
            if (preparePaths()) {
                // savebug: a checkpoint of an earlier, undecided session may still be in the writer's queue: its file must exist before
                // the folder is listed, or this session would take the same index and overwrite it
                if (drainWriter("session start")) {
                    auto job = [&] {
                        mp_save_mkdirs(sRecoveryDir);
                        sRecIndex = mp_save_recovery_next_index(sRecoveryDir);
                    };
                    hostJob("MpSaveRecIndex", job);
                }
            }
            mp_log("save session start original_host=%d", sG.originalHost ? 1 : 0);
        }
        void onEnd() { writeCheckpoint(true, "end"); }   // lost connection, host gone, kicked or left: keep what is in RAM safe
        void onEnded(bool held) { mp_log("save session end%s", held ? " (saves held until the image is reloaded)" : ""); }
        void logNoChoice() {
            static uint32_t sWarnedSession = 0;
            if (sWarnedSession != sSessionId) {
                sWarnedSession = sSessionId;
                mp_log("save host has no save choice (single-player root, no sp session): saves held in RAM");
            }
        }
    };

    void syncSession() {
        GateEnv env;
        MpSaveGate::sync(sG, env);
    }

    void takeAsyncResult() {
        MpSaveAsyncResult r;
        if (sAsyncUsed && mp_save_async_take_result(&r)) {
            mp_log("save recovery written index=%d bytes=%u ms=%u ok=%d", r.index, (unsigned)r.bytes, (unsigned)(r.micros / 1000),
                   r.ok ? 1 : 0);
        }
    }

    // The session's leave decision is made: its recovery file goes (deleted after a save, moved to discarded otherwise).
    void finishSession(bool saved) {
        if (sAsyncUsed) {
            if (!mp_save_async_idle()) mp_log("save waiting for the recovery writer");
            if (!drainWriter("session end")) mp_log("save session end: the recovery file stays (writer hung)");   // a checkpoint write in flight (small, rare)
            takeAsyncResult();
        }
        if (sRecIndex >= 0 && preparePaths()) {
            const int index = sRecIndex;
            auto job = [&] {
                if (saved) mp_save_recovery_delete(sRecoveryDir, index);
                else mp_save_recovery_discard(sRecoveryDir, index, 10);
            };
            hostJob("MpSaveRecDone", job);
            mp_log("save recovery cleared index=%d %s", index, saved ? "saved" : "discarded");
        }
        if (sG.spSessionSlot > 0 && !sSpBakBefore && !port_nand_all_atomic()) {
            // smallfix (owner decision): "Play on my file" wrote through the safe writer, which left GameData.bin.mpbak in the single-player
            // folder. Safe saves is off and there was no .mpbak before the session: our own file, removed now the session is over.
            // Only when GameData.bin reads and validates and no .mptmp is beside it: after a failed last save the .mpbak may be the only good copy.
            auto rm = [&] {
                const char* why = "";
                if (mp_save_drop_session_backup(sSpGameData, &why)) mp_log("save removed %s.mpbak (made by the session; Safe saves is off)", sSpGameData);
                else mp_log("save kept %s.mpbak: %s", sSpGameData, why);
            };
            hostJob("MpSaveBakRemove", rm);
        }
        sG.resolved = true;
        sG.sessionSeen = false;
        sG.progressInRam = false;
        sG.spSessionSlot = 0;
        sG.coopChosen = false;
        sRecIndex = -1;
        updateAtomicWrites();
    }

    unsigned char sSnapOut[MP_SNAPSHOT_MAX];
    unsigned char sSnapIn[MP_SNAPSHOT_MAX];
    uint32_t sSnapshotSession = 0;  // snapval: sSessionId of the session whose snapshot is in the image (0: none yet)
}  // namespace

namespace MpSaveGame {
    bool enterSessionRoot() {
        const bool ok = switchRoot(true);
        if (ok) MpSaveGate::chooseCoop(sG);   // savefix: the co-op file is this session's choice (see MpSaveGate::hostWriteChosen)
        return ok;
    }

    bool leaveSessionRoot() {
        syncSession();
        if (sG.sessionSeen && !sG.sessionActive) {
            // Left without a decision (the ui did not ask): the recovery file stays, the next Multiplayer menu offers it.
            mp_log("save leave undecided: recovery kept index=%d", sRecIndex);
            sG.sessionSeen = false;
            sG.resolved = true;
            sRecIndex = -1;
            sG.spSessionSlot = 0;
            sG.coopChosen = false;
        }
        MpSaveGate::abandonChoice(sG);   // gamefix: no session ran on a "Play on my file" choice: it must not outlive the lobby
        if ((sG.heldAfterSession || sUnsafe) && !sG.onMpRoot) {
            // review: a host without a save choice played on the single-player root: its image holds the session's
            // progress; reload that root's file (switchRoot does nothing when the root does not change)
            SaveDataHandleSequence* seq = idleSequence("root");
            if (seq == nullptr) return false;
            Scratch s;
            if (!s.ok() || !loadRootIntoImage(seq, sSpRoot, false, s)) return false;
            mp_log("save root sp reloaded (session progress dropped)");
            updateAtomicWrites();
        } else if (!switchRoot(false)) {
            return false;
        }
        MpSaveGate::imageReloaded(sG);  // the session progress in RAM is gone with the reload
        forgetAppliedSnapshot();
        if (!sG.sessionActive) sG.coopChosen = false;   // savefix: off the co-op root, its choice is gone
        return true;
    }

    // gamefix: the host's "Play on my single-player file" (or any save choice) was not used: start failed, lobby closed, file select.
    void abandonHostChoice() { MpSaveGate::abandonChoice(sG); }

    bool heldAfterSession() { return sG.heldAfterSession; }

    // optfix: no session is running or waiting for its leave decision, no progress is held in RAM, no root switch is unverified and no
    // recovery write is in flight: nothing here needs the "multiplayer entered" flag any more (the hooks that guard the save do not read it).
    bool settled() {
        return !sG.sessionActive && !sG.sessionSeen && !sG.heldAfterSession && !sG.progressInRam && !sUnsafe && (!sAsyncUsed || mp_save_async_idle());
    }

    bool isSessionRoot() { return sG.onMpRoot; }

    void setSessionSlot(int slot) {
        if (validSlot(slot)) sSessionSlot = slot;
    }

    int sessionSlot() { return sSessionSlot; }

    bool snapshotIsMario() { return sSnapshotMario; }

    bool importSinglePlayerFile(int spSlot, int mpSlot) {
        if (!validSlot(spSlot) || !validSlot(mpSlot)) return false;
        if ((!sG.onMpRoot || (sG.heldAfterSession && !mp_active())) && !enterSessionRoot()) return false;   // (a stale image is reloaded)
        SaveDataHandleSequence* seq = idleSequence("import");
        if (seq == nullptr) return false;
        ensureCheckLimits(seq);  // snapval: the imported file is checked like a snapshot (mp_save_import_file)
        Scratch s;
        if (!s.ok()) return false;
        char spPath[320], mpPath[320];
        gameDataOf(sSpRoot, spPath);
        gameDataOf(sMpRoot, mpPath);
        // The single-player file is opened read-only (mp_save_import_file); the co-op base is the co-op file on disk, or, when
        // there is none yet, the in-memory co-op image (we are on the multiplayer root here).
        if (!blankBase(seq, s.b)) memcpy(s.b, liveImage(seq), kImageBytes);
        int rc = 0;
        auto job = [&] { rc = mp_save_import_file(spPath, spSlot, mpPath, mpSlot, s.b, kImageBytes, s.a); };
        hostJob("MpSaveImport", job);
        if (rc != MP_SAVE_OP_OK) {
            mp_log("save import sp=%d mp=%d failed: %s", spSlot, mpSlot, mp_save_op_name(rc));
            return false;
        }
        // The in-memory co-op image gets the same blocks (the other co-op slots stay as they are in memory).
        mp_save_copy_slot(s.a, mpSlot, liveImage(seq), mpSlot);
        MpSaveGate::chooseCoop(sG);   // savefix
        mp_log("save import sp=%d mp=%d ok", spSlot, mpSlot);
        return true;
    }

    int compareImportProgress(int spSlot, int mpSlot) {
        if (!validSlot(spSlot) || !validSlot(mpSlot) || !preparePaths()) return 0;
        Scratch s;
        if (!s.ok()) return 0;
        char spPath[320], mpPath[320];
        gameDataOf(sSpRoot, spPath);
        gameDataOf(sMpRoot, mpPath);
        unsigned len = 0;
        if (readGameData(spPath, s.a, &len) != MP_SAVE_OK) return 0;
        if (readGameData(mpPath, s.b, &len) != MP_SAVE_OK) {
            SaveDataHandleSequence* seq = sequence();
            if (!sG.onMpRoot || seq == nullptr) return 0;  // no co-op file yet: nothing to lose
            memcpy(s.b, liveImage(seq), kImageBytes);
        }
        return mp_save_compare_progress(s.a, spSlot, s.b, mpSlot);
    }

    int slotState(bool coop, int slot, int* stars) {
        if (stars) *stars = -1;
        if (!validSlot(slot) || !preparePaths()) return SLOT_UNREADABLE;
        char path[320];
        gameDataOf(coop ? sMpRoot : sSpRoot, path);
        int rc = SLOT_UNREADABLE;
        auto job = [&] { rc = mp_save_slot_state_file(path, coop, slot, stars); };
        hostJob("MpSaveSlotState", job);
        return rc;
    }

    bool settingsReadOnly() { return mp_config_read_only() != MP_CONFIG_WRITABLE; }
    namespace { bool sLeftForMismatch = false; }
    bool takeLeftForMismatch() { const bool r = sLeftForMismatch; sLeftForMismatch = false; return r; }

    int slotStars(bool coop, int slot) {
        if (!validSlot(slot) || !preparePaths()) return -1;
        Scratch s;
        if (!s.ok()) return -1;
        char path[320];
        gameDataOf(coop ? sMpRoot : sSpRoot, path);
        unsigned len = 0;
        if (readGameData(path, s.a, &len) != MP_SAVE_OK) return -1;
        // flow (review 2 #3): a file that was never created (or does not parse) is not a file: "New" in the menus, never launched
        if (!coop && mp_save_slot_state(s.a, slot) != 1) return -1;   // (co-op files: see MpMenuSequence::coopStars)
        return mp_save_slot_stars(s.a, slot, nullptr, nullptr);
    }

    bool writeBackToSinglePlayer(int mpSlot, int spSlot) {
        if (!validSlot(spSlot) || !validSlot(mpSlot) || sUnsafe || !preparePaths()) return false;
        SaveDataHandleSequence* seq = idleSequence("writeback");
        if (seq == nullptr) return false;
        Scratch s;
        if (!s.ok()) return false;
        char spPath[320], mpPath[320], backup[320];
        gameDataOf(sSpRoot, spPath);
        gameDataOf(sMpRoot, mpPath);
        int rc = 0;
        auto job = [&] {
            rc = mp_save_writeback_file(mpPath, mpSlot, spPath, spSlot, sBackupDir, (int64_t)time(nullptr), kBackupsKept, backup,
                                        sizeof(backup), s.a);
        };
        hostJob("MpSaveWriteBack", job);
        if (rc != MP_SAVE_OP_OK) {
            mp_log("save writeback mp=%d sp=%d failed: %s backup=%s", mpSlot, spSlot, mp_save_op_name(rc), backup[0] ? backup : "-");
            return false;
        }
        // On the single-player root the in-memory image must match, or the next single-player save would write the old blocks.
        if (!sG.onMpRoot) mp_save_copy_slot(s.a, spSlot, liveImage(seq), spSlot);
        mp_log("save writeback mp=%d sp=%d backup=%s", mpSlot, spSlot, backup);
        return true;
    }

    bool backupSinglePlayerFile() {
        if (!preparePaths()) return false;
        char spPath[320], backup[320];
        gameDataOf(sSpRoot, spPath);
        bool backed = false;
        auto job = [&] { backed = mp_save_backup(spPath, sBackupDir, (int64_t)time(nullptr), kBackupsKept, backup, sizeof(backup)); };
        hostJob("MpSaveBackup", job);
        if (!backed) {
            mp_log("save backup sp failed");
            return false;
        }
        mp_log("save backup sp path=%s", backup);
        return true;
    }

    bool keepGuestProgress(int mpSlot) { return saveOnLeave(LEAVE_SAVE_COOP, mpSlot); }

    bool shouldOfferKeepProgress() {
        if (mp_active() && policy() == MP_SAVE_POLICY_SUPPRESS) sG.progressInRam = true;
        return sG.progressInRam && sG.onMpRoot;
    }

    bool discardSessionProgress() {
        if (!sG.onMpRoot) {
            sG.progressInRam = false;
            return true;
        }
        SaveDataHandleSequence* seq = idleSequence("discard");
        if (seq == nullptr) return false;
        Scratch s;
        if (!s.ok() || !loadRootIntoImage(seq, sMpRoot, true, s)) return false;
        sG.progressInRam = false;
        sG.heldAfterSession = false;
        if (sG.sessionSeen && !mp_active()) finishSession(false);
        mp_log("save session progress discarded");
        return true;
    }

    // The original host's saves are written only where the player chose: the multiplayer root (new / continue / copied co-op
    // file) or "Play on single-player file N" (backed up first). A session that reached play any other way (a path that skipped
    // the host's save choice) keeps its progress in RAM like a guest's, and the leave offer asks where to put it: the
    // single-player GameData.bin is never written without that choice.
    bool hostWriteChosen() { return MpSaveGate::hostWriteChosen(sG); }

    bool savesSuppressed() {
        GateEnv env;
        const bool held = MpSaveGate::suppressed(sG, env);   // self-syncing: see MpSaveGate.hpp
        updateAtomicWrites();
        if (sUnsafe) {   // savefix (finding 7): an unverified image is never written
            mp_log("save refused: the save image is not verified (failed root switch)");
            return true;
        }
        if (sFsOpen && !sG.sessionActive && (sFsGen != sImageGen || sG.onMpRoot || sG.heldAfterSession)) {
            // savefix (finding 1): the file select's files were cached from another image (a root switch since), or the
            // switch back to the single-player root has not happened yet: its create / copy / delete must not reach a file
            // the refused action (create / copy / delete) has already changed the image in RAM: no later save may write it; the
            // image counts as unverified until it is reloaded from disk (the file select's guard does that)
            sUnsafe = true;
            mp_log("save refused: the file select is not up to date with the save image (image to be reloaded)");
            return true;
        }
        return held;
    }

    bool savesAutoconfirm() { return mp_active() && policy() == MP_SAVE_POLICY_AUTOCONFIRM && hostWriteChosen(); }

    bool offerSnapshot() {
        // The current host sends the progress. After a migration the new host sends its RAM image (the original host's
        // progress it applied), never its own single-player data: it is on the multiplayer root with the snapshot applied.
        if (!mp_active() || !mp_is_host() || sUnsafe) return false;
        SaveDataHandleSequence* seq = sequence();
        if (seq == nullptr) return false;
        MpSaveSnapshotView v;
        int slot = 0;
        bool mario = true;
        if (!liveBlocks(seq, &v, &slot, &mario)) return false;
        const uint32_t len = mp_save_snapshot_pack(v.block[0], v.len[0], v.block[1], v.len[1], v.block[2], v.len[2], slot, v.flags,
                                                   sSnapOut, sizeof(sSnapOut));
        if (len == 0) return false;
        ensureCheckLimits(seq);
        MpSaveSnapshotView view;
        MpSaveCheckResult why;
        if (!mp_save_snapshot_parse_checked(sSnapOut, len, &view, &why)) {
            // snapval: our own progress fails the guests' check (should not happen: the self-test passed). Offered anyway: each
            // guest decides; the log says why they will refuse it.
            view.crc = mp_save_crc32(sSnapOut + MP_SAVE_SNAP_HEADER, len - MP_SAVE_SNAP_HEADER);
            mp_log("save snapshot offer fails the check: %s block=%s at=%u", mp_savecheck_name(why.err), checkBlockName(why.block),
                   (unsigned)why.at);
        }
        mp_snapshot_offer(sSnapOut, len);
        mp_log("save snapshot offer bytes=%u crc=%08x", (unsigned)len, (unsigned)view.crc);
        return true;
    }

    bool applySnapshotIfReady() {
        syncSession();
        // Every console that is not the original host takes the session progress (a host by migration already has it).
        if (!mp_active() || policy() == MP_SAVE_POLICY_AUTOCONFIRM) return false;
        const uint32_t ready = mp_snapshot_ready();
        if (ready == 0) return false;
        // Guests play on the multiplayer root only: never put session progress into the single-player image.
        if (!sG.onMpRoot && !enterSessionRoot()) return false;
        SaveDataHandleSequence* seq = idleSequence("snapshot");
        if (seq == nullptr) return false;  // a save sequence (suppressed) is finishing: next frame
        ensureCheckLimits(seq);
        const uint32_t len = ready <= sizeof(sSnapIn) ? mp_snapshot_get(sSnapIn, sizeof(sSnapIn)) : 0;
        MpSaveSnapshotView view;
        MpSaveCheckResult why;
        why.err = MP_SC_OK;
        if (len == 0 || len != ready || !mp_save_snapshot_parse_checked(sSnapIn, len, &view, &why)) {
            mp_snapshot_consumed();
            if (why.err == MP_SC_OK) {
                mp_log("save snapshot rejected bytes=%u", (unsigned)ready);
                return false;
            }
            // MP3 snapval: the blocks would make the game's save parsers read or write out of bounds, or hold progress the game
            // cannot have (a modified host). Nothing is written. With the session's earlier snapshot in the image, play goes on
            // with that; without any, this console cannot share the session's progress: leave politely.
            mp_log("save snapshot rejected: %s block=%s at=%u bytes=%u", mp_savecheck_name(why.err), checkBlockName(why.block),
                   (unsigned)why.at, (unsigned)ready);
            if (sSnapshotSession != 0 && sSnapshotSession == sSessionId) {
                mp_log("save snapshot rejected: keeping the session's previous progress");
            } else {
                mp_log("save snapshot rejected: leaving the session");
                MpUi::toast(MP_T_HOST_DATA_BAD, -1, 0);
                sLeftForMismatch = true;   // uxfix #2: the menus say it in a window when the session ends
                mp_ui_leave();
            }
            return false;
        }
        if (sLastAppliedSession != 0 && sLastAppliedSession == sSessionId && view.crc == sLastAppliedCrc) {
            // glitchfix S2: the very image already applied in this session (a host that offers it on every save, or a resend): applying it again
            // would rewrite the live user file mid-level for nothing. A different image (the host's progress moved on) is applied as before.
            mp_snapshot_consumed();
            mp_log("save snapshot identical to the applied one: ignored crc=%08x", (unsigned)view.crc);
            return false;
        }
        // The progress goes into EVERY file of the in-RAM multiplayer image (guests never save during a session; the leave offer
        // writes only the slot the player picks, to disk). Whichever file the guest starts then holds the session's progress,
        // whether it pressed Play This File before or after this arrived: with one slot only, a guest starting another file
        // got a blank file and the new-file prologue was queued before the host's MOVE could redirect it (crash in
        // MR::startPrologue). The slot number in the payload is information only.
        unsigned char* image = liveImage(seq);
        const int current = seq->mCurrentUserFile ? slotOfName(seq->mCurrentUserFile->getGameDataName()) : -1;
        const int slot = validSlot(current) ? current : sSessionSlot;
        int applied = 0;
        for (int i = 1; i <= MP_SAVE_SLOTS; i++) {
            if (mp_save_snapshot_apply(&view, image, i)) applied++;
        }
        if (applied != MP_SAVE_SLOTS) {
            mp_log("save snapshot rejected: image (%d of %d files)", applied, MP_SAVE_SLOTS);
            if (applied == 0) {
                mp_snapshot_consumed();
                return false;
            }
        }
        sSessionSlot = slot;
        sSnapshotMario = (view.flags & MP_SAVE_SNAP_FLAG_MARIO) != 0;
        sG.progressInRam = true;
        sSnapshotSession = sSessionId;
        MpSaveGate::sourceLost(sG);   // savefix (finding 5): another console's progress: no single-player source any more
        seq->restoreUserFile(seq->mCurrentUserFile, slot, sSnapshotMario);
        sLastAppliedCrc = view.crc;
        sLastAppliedSession = sSessionId;
        mp_snapshot_consumed();
        mp_log("save snapshot applied slot=%d files=%d", slot, applied);
        return true;
    }
    bool snapshotAppliedThisSession() {
        if (!mp_active()) return false;
        if (policy() == MP_SAVE_POLICY_AUTOCONFIRM) return true;   // the original host plays its own progress
        return sG.progressInRam;
    }

    // ---- leaving: "Save your progress?" -----------------------------------------------------------------------------------
    void beginSinglePlayerSession(int spSlot) {
        if (!validSlot(spSlot)) return;
        MpSaveGate::beginSinglePlayer(sG, spSlot);
        if (preparePaths()) {   // smallfix: was there a .mpbak before this session's writes? (then it is not ours to remove at the end)
            gameDataOf(sSpRoot, sSpGameData);
            char bak[330];
            snprintf(bak, sizeof(bak), "%s.mpbak", sSpGameData);
            struct stat st;
            auto probe = [&] { sSpBakBefore = stat(bak, &st) == 0; };
            hostJob("MpSaveBakProbe", probe);
        } else {
            sSpBakBefore = true;   // (unknown: nothing is removed)
        }
        updateAtomicWrites();
        mp_log("save session on single-player file sp=%d", spSlot);
    }

    bool leaveDecisionPending() {
        syncSession();
        return sG.sessionSeen;
    }

    // gamefix: the single-player GameData.bin on disk: stars of slot (-1: unreadable / missing) and its last write (unix seconds, 0 unknown)
    static int singlePlayerFileInfo(int slot, long long* mtime) {
        *mtime = 0;
        if (!validSlot(slot) || !preparePaths()) return -1;
        Scratch s;
        if (!s.ok()) return -1;
        char path[320];
        gameDataOf(sSpRoot, path);
        unsigned len = 0;
        if (readGameData(path, s.a, &len) != MP_SAVE_OK) return -1;
        struct stat st;
        if (stat(path, &st) == 0) *mtime = (long long)st.st_mtime;
        return mp_save_slot_stars(s.a, slot, nullptr, nullptr);
    }

    bool offerSaveOnLeave(LeaveOffer* out) {
        syncSession();
        if (out == nullptr) return false;
        memset(out, 0, sizeof(*out));
        for (int i = 0; i < 6; i++) out->coopStars[i] = -1;
        SaveDataHandleSequence* seq = sequence();
        if (!sG.sessionSeen || seq == nullptr || !preparePaths()) return false;
        MpSaveSnapshotView v;
        int slot = 0;
        bool mario = true;
        if (!liveBlocks(seq, &v, &slot, &mario)) return false;
        out->offer = true;
        out->coop = true;
        out->single = sG.spSessionSlot > 0 && slot == sG.spSessionSlot;   // savefix: only while file N itself is in play
        out->singleSlot = out->single ? sG.spSessionSlot : 0;
        out->singleStars = -1;
        if (sG.spSessionSlot > 0) {
            long long mt;
            out->singleStars = singlePlayerFileInfo(sG.spSessionSlot, &mt);
        }
        out->sessionSlot = slot;
        out->stars = viewStars(v);
        Scratch s;
        if (s.ok()) {
            char mpPath[320];
            gameDataOf(sMpRoot, mpPath);
            unsigned len = 0;
            if (readGameData(mpPath, s.a, &len) == MP_SAVE_OK) {
                ensureBlank(seq);
                const MpSaveSnapshotView blank = blankView();
                for (int i = 1; i <= MP_SAVE_SLOTS; i++) {
                    out->coopUsed[i - 1] = !(sBlankReady && mp_save_slot_matches(s.a, i, &blank));
                    out->coopStars[i - 1] = out->coopUsed[i - 1] ? mp_save_slot_stars(s.a, i, nullptr, nullptr) : -1;
                }
            }
        }
        mp_log("save leave offer stars=%d single=%d", out->stars, out->single ? out->singleSlot : 0);
        return true;
    }

    bool saveOnLeave(int choice, int slot) {
        syncSession();
        if (!preparePaths()) return false;
        if (choice == LEAVE_DONT_SAVE) {
            mp_log("save leave discard");
            finishSession(false);
            return true;
        }
        if (sUnsafe) {
            mp_log("save leave refused: the save image is not verified");
            return false;
        }
        SaveDataHandleSequence* seq = idleSequence("leave");
        if (seq == nullptr) return false;
        UserFile* file = seq->mCurrentUserFile;
        int from = slotOfName(file->getGameDataName());
        if (from < 0) return false;
        seq->mSaveDataHandler->storeUserFile(file);  // the live progress into the image, as a save would
        unsigned char* image = liveImage(seq);
        MpSaveSnapshotView v;
        if (!mp_save_view_slot(image, from, &v)) return false;
        const int stars = viewStars(v);
        Scratch s;
        if (!s.ok()) return false;
        if (choice == LEAVE_SAVE_COOP) {
            if (!validSlot(slot) || !blankBase(seq, s.b)) return false;
            char mpPath[320];
            gameDataOf(sMpRoot, mpPath);
            int rc = 0;
            char mpDir[320];
            dataDirOf(sMpRoot, mpDir);
            auto job = [&] { mp_save_mkdirs(mpDir); rc = mp_save_put_blocks(&v, mpPath, slot, s.b, kImageBytes, s.a); };
            hostJob("MpSaveLeave", job);
            if (rc != MP_SAVE_OP_OK) {
                mp_log("save leave coop mp=%d failed: %s", slot, mp_save_op_name(rc));
                return false;
            }
            if (sG.onMpRoot) mp_save_copy_slot(s.a, slot, image, slot);
            mp_log("save leave coop mp=%d ok stars=%d", slot, stars);
        } else if (choice == LEAVE_SAVE_SINGLE) {
            const int sp = sG.spSessionSlot;
            if (sp <= 0) return false;
            if (from != sp) {
                // savefix (sv_mine_leave run): the user file in memory is not file N's (back at the title, another file loaded):
                // writing it would put another file's data (or a blank one) into single-player file N
                mp_log("save leave sp=%d refused: the file in play is file %d", sp, from);
                return false;
            }
            char spPath[320], backup[320];
            gameDataOf(sSpRoot, spPath);
            int rc = 0;
            backup[0] = 0;
            auto job = [&] {
                if (!mp_save_backup(spPath, sBackupDir, (int64_t)time(nullptr), kBackupsKept, backup, sizeof(backup))) {
                    rc = MP_SAVE_OP_BACKUP;
                    return;
                }
                rc = mp_save_put_blocks(&v, spPath, sp, nullptr, 0, s.a);
            };
            hostJob("MpSaveLeaveSp", job);
            if (rc != MP_SAVE_OP_OK) {
                mp_log("save leave sp=%d failed: %s backup=%s", sp, mp_save_op_name(rc), backup[0] ? backup : "-");
                return false;
            }
            if (!sG.onMpRoot) mp_save_copy_slot(s.a, sp, image, sp);
            mp_log("save leave sp=%d backup=%s stars=%d", sp, backup, stars);
        } else {
            return false;
        }
        finishSession(true);
        return true;
    }

    // ---- desktop autopilot: the host's save choice the menu would make (SMG_MP_AUTO_SAVE=coop | copy:N) ------------------
    // The core's SMG_MP_AUTO=host starts a session without the lobby's Start, so without this the host keeps its progress in
    // RAM (savesSuppressed). coop: play on the co-op file the file select then picks; copy:N: single-player file N copied into
    // co-op file N first (the menu's "Copy"); savefix: mine:N = "Play on my file N" (the single-player root, a backup first, as
    // the menu's prepareHostSave). SMG_MP_TEST_SAVE=S (savefix, tests): the original host asks the game for a save every S seconds
    // of session play (the game's own save sequence, auto-confirmed in a session). Desktop only; nothing without the env.
    void autoSaveChoiceStep() {
#if defined(TARGET_PC) && !defined(__vita__)
        static int sTestSaveTurns = -1;
        static uint32_t sTestSaveNext = 0;
        if (sTestSaveTurns < 0) {
            const char* t = getenv("SMG_MP_TEST_SAVE");
            sTestSaveTurns = t && atoi(t) > 0 ? atoi(t) * 60 : 0;
        }
        if (sTestSaveTurns > 0 && sG.sessionActive && sG.originalHost && hostWriteChosen() && MpGame::isGameSceneLive()) {
            if (sTestSaveNext == 0) sTestSaveNext = MpGame::turn() + (uint32_t)sTestSaveTurns;
            SaveDataHandleSequence* seq = sequence();
            if (MpGame::turn() >= sTestSaveNext && seq != nullptr && !seq->isActive()) {
                sTestSaveNext = MpGame::turn() + (uint32_t)sTestSaveTurns;
                mp_log("save test: game save requested");
                GameSequenceFunction::startGameDataSaveSequence(false, false);
            }
        }
        static int sTries = -1;   // -1 unread, 0 idle/done, >0 frames left to retry
        static char sChoice[16];
        if (sTries < 0) {
            const char* e = MP_TEST_GETENV("SMG_MP_AUTO_SAVE");
            sTries = 0;
            if (e && *e) {
                snprintf(sChoice, sizeof(sChoice), "%s", e);
                sTries = 600;
            }
        }
        if (sTries <= 0 || !sG.sessionActive) return;
        if (policy() != MP_SAVE_POLICY_AUTOCONFIRM || hostWriteChosen()) {
            sTries = 0;
            return;
        }
        bool ok = false;
        if (!strncmp(sChoice, "coop", 4)) {
            ok = enterSessionRoot();
        } else if (!strncmp(sChoice, "copy", 4)) {
            const char* c = strchr(sChoice, ':');
            const int n = c ? atoi(c + 1) : 1;
            ok = validSlot(n) && importSinglePlayerFile(n, n) && isSessionRoot();
        } else if (!strncmp(sChoice, "mine", 4)) {
            const char* c = strchr(sChoice, ':');
            const int n = c ? atoi(c + 1) : 1;
            ok = validSlot(n) && ((!sG.onMpRoot && !sG.heldAfterSession) || leaveSessionRoot()) && backupSinglePlayerFile();
            if (ok) beginSinglePlayerSession(n);
        } else {
            sTries = 0;
            mp_log("save auto choice '%s' unknown (coop | copy:N | mine:N)", sChoice);
            return;
        }
        if (ok || --sTries == 0) {
            sTries = 0;
            mp_log("save auto choice %s %s", sChoice, ok ? "ready" : "FAILED");
        }
#endif
    }

    // ---- savefix: desktop test autopilot for the leave decision and the recovery offer (scenarios; nothing without the env) --
    //   SMG_MP_TEST_LEAVE=S          leave the session after S seconds of play (mp_ui_leave, as the session menu's Leave)
    //   SMG_MP_AUTO_LEAVE=coop:N | single | discard   the answer to "Save your progress?" once the session is over
    //   SMG_MP_AUTO_RECOVERY=keep:N | single | discard  the answer to the recovery offer (recoveryPending) at the file select
    // Logs the same decisions as the menus ("ui: recovery offered stars=%d" is the menu's own line, repeated here).
    static void autoDecisionStep() {
#if defined(TARGET_PC) && !defined(__vita__)
        static int sRead = 0;
        static uint32_t sLeaveTurns = 0, sLeaveAt = 0;
        static char sLeave[16], sRec[16];
        static bool sRecDone = false;
        if (!sRead) {
            sRead = 1;
            const char* t = getenv("SMG_MP_TEST_LEAVE");
            sLeaveTurns = t && atoi(t) > 0 ? (uint32_t)atoi(t) * 60 : 0;
            const char* l = MP_TEST_GETENV("SMG_MP_AUTO_LEAVE");
            snprintf(sLeave, sizeof(sLeave), "%s", l ? l : "");
            const char* r = MP_TEST_GETENV("SMG_MP_AUTO_RECOVERY");
            snprintf(sRec, sizeof(sRec), "%s", r ? r : "");
        }
        if (sLeaveTurns > 0 && sG.sessionActive) {
            if (sLeaveAt == 0) sLeaveAt = MpGame::turn() + sLeaveTurns;
            if (MpGame::turn() >= sLeaveAt) {
                sLeaveTurns = 0;
                mp_log("save test: leaving the session");
                mp_ui_leave();
            }
        }
        if (sLeave[0] && !mp_active() && sG.sessionSeen && !sG.resolved) {
            const SaveDataHandleSequence* seq = sequence();
            if (seq != nullptr && !seq->isActive()) {
                bool ok = false;
                if (!strncmp(sLeave, "coop:", 5)) ok = saveOnLeave(LEAVE_SAVE_COOP, atoi(sLeave + 5));
                else if (!strcmp(sLeave, "single")) ok = saveOnLeave(LEAVE_SAVE_SINGLE, 0);
                else ok = saveOnLeave(LEAVE_DONT_SAVE, 0);
                mp_log("save test: leave decision %s %s", sLeave, ok ? "ok" : "FAILED");
                sLeave[0] = 0;
            }
        }
        if (sRec[0] && !sRecDone && !mp_active() && !sG.sessionSeen && sFsOpen) {
            sRecDone = true;
            RecoveryInfo info;
            if (!recoveryPending(&info)) {
                mp_log("save test: no recovery offered");
                return;
            }
            mp_log("ui: recovery offered stars=%d", info.stars);
            bool ok = false;
            if (!strncmp(sRec, "keep:", 5)) ok = recoveryKeep(atoi(sRec + 5), LEAVE_SAVE_COOP);
            else if (!strcmp(sRec, "single")) ok = recoveryKeep(0, LEAVE_SAVE_SINGLE);
            else ok = recoveryDiscard();
            mp_log("save test: recovery %s %s", sRec, ok ? "ok" : "FAILED");
        }
#endif
    }

    // ---- savefix: launch guard --------------------------------------------------------------------------------------------
    bool launchReady() {
        syncSession();
        if (!sG.sessionActive) return false;
        if (sG.originalHost) return MpSaveGate::hostWriteChosen(sG) && !sG.heldAfterSession;
        return !sStartedAsGuest || sG.onMpRoot;   // (a host by migration stays on the root it chose; its saves are held)
    }

    // savefix, revised (coordinator's two-console run: a guest that joined from the game by autopilot was ejected 3 s after joining):
    // the save layer never takes a player out of the room. A console that joins outside the menu's launch (autopilot, drop-in,
    // Quick Match, a rejoin) is legitimate. Its writes are already refused where they must be: a guest's saves are suppressed for
    // the whole session and held after it until the image is reloaded (MpSaveGate; the hook syncs itself), the first host
    // snapshot moves it to the multiplayer root (applySnapshotIfReady), and an original host without a save choice keeps its
    // progress in RAM (logNoChoice) until the leave offer, which writes only a co-op file. The menu's launch still refuses to
    // start on a save nobody chose (MpMenuSequence watchSession). This only logs, once per session.
    static void launchGuard() {
        MpUiStatus st;
        mp_ui_status(&st);
        if (sLaunchRefused || st.phase != MP_UI_PLAYING || launchReady()) {
            sUnsafeTurns = 0;
            return;
        }
        if (++sUnsafeTurns < (sG.originalHost ? kLaunchGraceHost : kLaunchGraceGuest)) return;
        sLaunchRefused = true;   // (logged once per session)
        mp_log("save launch check: %s: saves held in RAM (no file is written)",
               sG.originalHost ? "the host made no save choice" : "the guest is not on the multiplayer root yet");
    }

    // ---- recovery -------------------------------------------------------------------------------------------------------
    void tick() {
        syncSession();
        autoSaveChoiceStep();
        autoDecisionStep();
        takeAsyncResult();
        if (!sG.sessionActive) return;
        if (mp_host_epoch() != 0) {
            sG.originalHost = false;
            MpSaveGate::sourceLost(sG);   // savefix (finding 5): after a migration the session has no single-player source
        }
        updateAtomicWrites();
        launchGuard();   // savefix
        if (++sTurns >= kCheckpointTurns) {
            sTurns = 0;
            writeCheckpoint(false, "timer");
        }
    }

    bool writeRecoveryCheckpoint() {
        syncSession();
        sTurns = 0;
        return writeCheckpoint(true, "star");
    }

    bool recoveryPending(RecoveryInfo* out) {
        syncSession();
        sPendingIndex = -1;
        if (!preparePaths()) return false;
        ensureCheckLimits(sequence());  // snapval: recovery files are checked like snapshots (mp_save_recovery_parse)
        int indices[64];
        int count = 0;
        auto list = [&] { count = mp_save_recovery_list(sRecoveryDir, indices, 64); };
        hostJob("MpSaveRecList", list);
        Scratch s;
        if (!s.ok()) return false;
        for (int i = count - 1; i >= 0; i--) {
            const int index = indices[i];
            if (sG.sessionSeen && index == sRecIndex) continue;  // this session's own file: its leave decision is pending
            uint32_t len = 0;
            bool ok = false;
            auto read = [&] { ok = mp_save_recovery_read(sRecoveryDir, index, s.a, kImageBytes, &len); };
            hostJob("MpSaveRecRead", read);
            MpSaveRecoveryInfo info;
            MpSaveSnapshotView blocks;
            if (!ok || !mp_save_recovery_parse(s.a, len, &info, &blocks)) {
                mp_log("save recovery unreadable index=%d", index);
                continue;
            }
            // Already in its file (the original host's auto-save, a single-player save)? Then nothing would be lost.
            if (info.srcKind != MP_SAVE_SRC_NONE) {
                char path[320];
                gameDataOf(info.srcKind == MP_SAVE_SRC_COOP ? sMpRoot : sSpRoot, path);
                unsigned flen = 0;
                if (readGameData(path, s.b, &flen) == MP_SAVE_OK && mp_save_slot_matches(s.b, info.srcSlot, &blocks)) {
                    auto drop = [&] { mp_save_recovery_delete(sRecoveryDir, index); };
                    hostJob("MpSaveRecDrop", drop);
                    mp_log("save recovery already saved index=%d", index);
                    continue;
                }
            }
            // savebug: a single-player file's checkpoint that a later save has overtaken is not offered: it holds a never-created slot, or
            // FEWER stars than the file now and the file was saved after it (an earlier session whose decision was never made, then more play
            // on the same file). It is moved aside (recovery/discarded, ten kept), never deleted, and the player's file is not touched.
            if (info.srcKind == MP_SAVE_SRC_SINGLE) {
                long long mt = 0;
                const int fileStars = singlePlayerFileInfo(info.srcSlot, &mt);
                const bool newer = mt > 0 && info.unixTime > 0 && mt > info.unixTime;
                const bool blank = mp_save_view_state(&blocks) == 0;
                if (blank || (newer && fileStars >= 0 && (int)info.stars < fileStars)) {
                    auto aside = [&] { mp_save_recovery_discard(sRecoveryDir, index, 10); };
                    hostJob("MpSaveRecAside", aside);
                    mp_log("save recovery superseded index=%d stars=%u file=%d stars=%d%s", index, (unsigned)info.stars, (int)info.srcSlot, fileStars,
                           blank ? " (blank)" : "");
                    continue;
                }
            }
            sPendingIndex = index;
            if (out != nullptr) {
                memset(out, 0, sizeof(*out));
                out->index = index;
                out->sessionId = info.sessionId;
                out->unixTime = info.unixTime;
                snprintf(out->galaxy, sizeof(out->galaxy), "%s", info.galaxy);
                out->scenario = info.scenario;
                out->stars = info.stars;
                out->sessionSlot = info.slot;
                out->originalHost = (info.flags & MP_SAVE_REC_FLAG_ORIGINAL_HOST) != 0;
                out->mario = (info.flags & MP_SAVE_REC_FLAG_MARIO) != 0;
                out->sourceKind = info.srcKind;
                out->sourceSlot = info.srcSlot;
                out->single = info.srcKind == MP_SAVE_SRC_SINGLE;
                out->spStars = -1;
                if (out->single) {
                    long long mt;
                    out->spStars = singlePlayerFileInfo(info.srcSlot, &mt);
                    out->spNewer = mt > 0 && info.unixTime > 0 && mt > info.unixTime;   // the file was saved after this session's last checkpoint
                }
            }
            mp_log("save recovery pending index=%d stars=%u galaxy=%s", index, (unsigned)info.stars, info.galaxy);
            return true;
        }
        return false;
    }

    bool recoveryKeep(int slot, int choice, bool overwriteConfirmed) {
        if (sPendingIndex < 0 || !preparePaths()) return false;
        SaveDataHandleSequence* seq = idleSequence("recovery");
        if (seq == nullptr) return false;
        ensureCheckLimits(seq);
        Scratch s;
        if (!s.ok()) return false;
        const int index = sPendingIndex;
        uint32_t len = 0;
        bool ok = false;
        auto read = [&] { ok = mp_save_recovery_read(sRecoveryDir, index, s.a, kImageBytes, &len); };
        hostJob("MpSaveRecRead", read);
        MpSaveRecoveryInfo info;
        MpSaveSnapshotView blocks;
        if (!ok || !mp_save_recovery_parse(s.a, len, &info, &blocks)) return false;
        unsigned char* image = liveImage(seq);
        int rc = MP_SAVE_OP_ARGS;
        char backup[320];
        backup[0] = 0;
        if (choice == LEAVE_SAVE_COOP) {
            if (!validSlot(slot) || !blankBase(seq, s.b)) return false;
            char mpPath[320];
            gameDataOf(sMpRoot, mpPath);
            char mpDir[320];
            dataDirOf(sMpRoot, mpDir);
            // MP3 fix: a console that never made a Team File (a guest killed in its first session) has no mp/saves folder yet: make it, or the keep fails "write failed"
            auto job = [&] { mp_save_mkdirs(mpDir); rc = mp_save_put_blocks(&blocks, mpPath, slot, s.b, kImageBytes, nullptr); };
            hostJob("MpSaveRecKeep", job);
            if (rc == MP_SAVE_OP_OK && sG.onMpRoot) mp_save_snapshot_apply(&blocks, image, slot);
        } else if (choice == LEAVE_SAVE_SINGLE && info.srcKind == MP_SAVE_SRC_SINGLE) {
            slot = info.srcSlot;
            // savebug: a recovery file never replaces a single-player file with a blank slot, nor (without the player's answer to the
            // "more stars" question) with less progress or over a file saved after it was written
            const int state = mp_save_view_state(&blocks);
            if (state == 0) {
                mp_log("save recovery keep index=%d refused: the recovery file holds a never-created file", index);
                return false;
            }
            long long mt = 0;
            const int fileStars = singlePlayerFileInfo(slot, &mt);
            const bool newer = mt > 0 && info.unixTime > 0 && mt > info.unixTime;
            if (!overwriteConfirmed && (state < 0 || MpSaveGate::singleOverwriteWarning((int)info.stars, fileStars, newer))) {
                mp_log("save recovery keep index=%d refused: file %d has %d stars (recovery %u)%s%s, the player has not been asked", index, slot,
                       fileStars, (unsigned)info.stars, newer ? ", saved after it" : "", state < 0 ? ", recovery file unreadable as a file" : "");
                return false;
            }
            char spPath[320];
            gameDataOf(sSpRoot, spPath);
            auto job = [&] {
                rc = mp_save_backup(spPath, sBackupDir, (int64_t)time(nullptr), kBackupsKept, backup, sizeof(backup))
                         ? mp_save_put_blocks(&blocks, spPath, slot, nullptr, 0, nullptr)
                         : MP_SAVE_OP_BACKUP;
            };
            hostJob("MpSaveRecKeepSp", job);
            if (rc == MP_SAVE_OP_OK && !sG.onMpRoot) mp_save_snapshot_apply(&blocks, image, slot);
        }
        if (rc != MP_SAVE_OP_OK) {
            mp_log("save recovery keep index=%d failed: %s", index, mp_save_op_name(rc));
            return false;
        }
        auto drop = [&] { mp_save_recovery_delete(sRecoveryDir, index); };
        hostJob("MpSaveRecDrop", drop);
        sPendingIndex = -1;
        if (choice == LEAVE_SAVE_SINGLE) mp_log("save recovery kept index=%d sp=%d backup=%s", index, slot, backup);
        else mp_log("save recovery kept index=%d mp=%d", index, slot);
        return true;
    }

    // ---- savefix: the file select's guard (review finding 1) ----------------------------------------------------------
    unsigned fileSelectOpened() {
        syncSession();   // savefix: the title has no game-scene tick: a session that ended there is latched here
        sFsOpen = true;
        sFsGen = sImageGen;
        sFsRetry = 0;
        return sFsGen;
    }
    void fileSelectClosed() { sFsOpen = false; }
    int fileSelectGuard(unsigned cachedGen) {
        syncSession();   // (every frame of the file select: session edges are seen at the title too)
        autoDecisionStep();
        if (mp_active()) return FS_USABLE;   // (a session's launch: the Multiplayer menu holds the files)
        const bool unready = sUnsafe || sG.onMpRoot || sG.heldAfterSession;
        if (unready) {
            // the switch back to the single-player root: tried here too (every 30 frames), not only by the menu
            if (sFsRetry++ % 30 == 29 && leaveSessionRoot()) mp_log("save root: file select guard switched back to single-player");
            if (sUnsafe || sG.onMpRoot || sG.heldAfterSession) return FS_WAIT;
        }
        if (cachedGen != sImageGen) return FS_REBUILD;
        return FS_USABLE;
    }

    // ---- savefix: backups (review finding 3: no in-game restore) -------------------------------------------------------
    bool listBackups(BackupList* out) {
        if (out == nullptr) return false;
        out->count = 0;
        if (!preparePaths()) return false;
        MpSaveBackupInfo info[kBackupListMax];
        int n = 0;
        auto job = [&] { n = mp_save_backup_list(sBackupDir, info, kBackupListMax); };
        hostJob("MpSaveBackupList", job);
        for (int i = 0; i < n; i++) snprintf(out->name[out->count++], sizeof out->name[0], "%s", info[i].name);
        return true;
    }

    bool restoreBackup(const char* backupPath) {
        if (backupPath == nullptr || mp_active() || sG.sessionSeen || !preparePaths()) return false;
        SaveDataHandleSequence* seq = idleSequence("restore");
        if (seq == nullptr) return false;
        char spPath[320], before[320];
        gameDataOf(sSpRoot, spPath);
        before[0] = 0;
        int rc = 0;
        auto job = [&] {
            rc = mp_save_restore_backup(backupPath, spPath, sBackupDir, (int64_t)time(nullptr), kBackupsKept, before, sizeof(before));
        };
        hostJob("MpSaveRestore", job);
        if (rc != MP_SAVE_OP_OK) {
            mp_log("save restore %s failed: %s", backupPath, mp_save_op_name(rc));
            return false;
        }
        mp_log("save restore %s ok (the file it replaced: %s)", backupPath, before[0] ? before : "none");
        // The game's image must be the restored file, or its next save writes the old one back: reload when on the single-player
        // root (the file select then rebuilds: the generation changed).
        if (!sG.onMpRoot) {
            Scratch sc;
            if (!sc.ok() || !loadRootIntoImage(seq, sSpRoot, false, sc)) {
                sUnsafe = true;   // (saves blocked until a reload succeeds: the file select guard retries)
                mp_log("save restore: image reload failed, saves blocked until it succeeds");
            }
        }
        return true;
    }

    bool writeInFlight() { return mp_save_write_in_flight(); }

    bool recoveryDiscard() {
        if (sPendingIndex < 0 || !preparePaths()) return false;
        const int index = sPendingIndex;
        bool ok = false;
        auto job = [&] { ok = mp_save_recovery_discard(sRecoveryDir, index, 10); };
        hostJob("MpSaveRecDiscard", job);
        sPendingIndex = -1;
        mp_log("save recovery discarded index=%d ok=%d", index, ok ? 1 : 0);
        return ok;
    }
}  // namespace MpSaveGame

// MP3 keepfix (keepsafe review 4): keepsafe (import / restore / rescue / the repair at boot) replaces the multiplayer root's GameData.bin
// on the card. The game keeps that file's image in RAM and writes the WHOLE image at every save, so the image is reloaded before the call
// returns (the same load as restoreBackup's), or every save is refused until a reload succeeds. Registered by mp_port_boot
// (platform/src/mp/mp_boot.cpp) through mp_keepsafe_add_reload_hook / mp_keepsafe_set_guard. Single player: never called (keepsafe runs
// only from a multiplayer menu action and at boot, before the game has loaded any save).
extern "C" bool mp_savegame_keepsafe_may_replace(void) {
    // no session, nothing of a session held in RAM, no leave decision pending, no save write running, and (keepfix 2, review 2 #10) the
    // game's save sequence idle: one that is active but has not opened its file yet would write the old image over the new file
    if (mp_active() || sG.sessionActive || sG.sessionSeen || sG.progressInRam || sG.heldAfterSession || mp_save_write_in_flight()) return false;
    SaveDataHandleSequence* seq = sequence();
    return seq == nullptr || !seq->isActive();   // (no save system yet: nothing can write)
}

extern "C" void mp_savegame_keepsafe_reload(void) {
    if (!sPathsReady || !sG.onMpRoot) return;   // (the multiplayer file is not in RAM: the next switch to its root loads it from the card)
    SaveDataHandleSequence* seq = idleSequence("keepsafe");
    Scratch sc;
    if (seq == nullptr || !sc.ok() || !loadRootIntoImage(seq, sMpRoot, true, sc)) {
        sUnsafe = true;   // every save refused until a switch / reload succeeds (the file select guard retries)
        mp_log("save root mp: reload after keepsafe FAILED: saves blocked until a reload succeeds");
        return;
    }
    MpSaveGate::imageReloaded(sG);
    forgetAppliedSnapshot();
    mp_log("save root mp reloaded after keepsafe");
}

// glitchfix S1: read-only accessor for MpFlow.cpp (added at the end of the file; no existing function was changed)
namespace MpSaveGame {
    bool playingOnSinglePlayerFile() { return sG.spSessionSlot > 0; }
}  // namespace MpSaveGame
