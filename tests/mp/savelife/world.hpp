// savelife harness (savefix): the fake console's state (game objects, net bridge, faults, image provenance, log).
#pragma once
#include "fakegame.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const int kKillExit = 77;

struct Faults {
    int ops = 0;              // mutating file-system calls so far (this process)
    int killAt = 0;           // 0 off; else the op number at which the process dies
    bool killAfter = false;   // die after the op (else before it)
    bool tear = false;        // a write the power cut lands in reaches the card half-done
    int readEio = 0;          // reads of a file whose path contains readEioMatch fail (this many opens)
    char readEioMatch[64] = "";
    int statErr = 0;          // stat() of a path containing statErrMatch fails with statErrno (this many calls; a card that cannot say whether a file is there)
    char statErrMatch[64] = "";
    int statErrno = 5;        // EIO
    long cardFree = -1;       // bytes left on the card (-1: unlimited)
    int shortWrite = 0;
    int renameFail = 0;
    bool renameNoReplace = false;
    int hostAllocFail = 0;    // port_host_alloc (MpSaveGame's Scratch)
    int hostAllocAfter = 0;   // ... after this many good ones
    int mallocFail = 0;       // malloc / calloc >= 32 KiB (mp_savefile's work buffers)
    int copySkip = 0;         // the game's block copy leaves one block out
    int fopenFail = 0;
    char fopenFailMatch[64] = "";
    int writeCorrupt = 0;     // a write reaches the card with one byte changed (and reports success)
    int renameLie = 0;        // a rename reports success and does nothing
    int eioFd[64] = {};
    int eioCount = 0;
    long fsyncs = 0;          // fsync calls since the last clear (a script counts what a writer syncs)
    void clear() { *this = Faults(); }
};

struct Net {
    bool active = false;
    int localSlot = 0, hostSlot = 0;
    uint16_t epoch = 0;
    int phase = 0;            // MpUiPhase
    uint8_t snap[0x2000];
    uint32_t snapLen = 0;
};

struct World {
    // game objects
    alignas(32) u8 image[0x10000];
    SaveDataHandler handler;
    UserFile* user = nullptr;
    SysConfigFile sys;
    SaveDataHandleSequence seq;
    GameSequenceDirector dir;
    GameSystemSceneController scene;
    GameSystem system;
    GameDataHolder holder;
    u8 blankGame[0xF80];
    u8 blankConfig[0x60];
    u8 sysconf[0x80];
    int busyFrames = 0;
    uint32_t turn = 0;
    int toasts = 0, lastToast = -1, leaveCalls = 0, offers = 0, restores = 0;   // (savesus S2: restores = restoreUserFile calls)
    Net net;
    Faults faults;
    // image provenance: the root whose file the image was last loaded from (or reset on)
    char imageRoot[256] = "";
    bool imageCopied = false;
    unsigned imageLoads = 0;
    void (*killHook)(const char* what, const char* path) = nullptr;

    void init() {
        handler._18 = image;
        seq.mSaveDataHandler = &handler;
        seq.mSysConfigFile = &sys;
        dir.mSaveDataHandleSequence = &seq;
        system.mSequenceDirector = &dir;
        system.mSceneController = &scene;
    }
    void noteImageReset();
    void noteImageCopied();
    void onKill(const char* what, const char* path) {
        if (killHook) killHook(what, path);
    }
    uint64_t rngState = 1;
    uint64_t rng() {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 7;
        rngState ^= rngState << 17;
        return rngState;
    }
    // log ring
    char logs[64][200];
    int logN = 0;
    bool echo = false;
    void log(const char* line) {
        snprintf(logs[logN % 64], sizeof logs[0], "%s", line);
        logN++;
        if (echo) fprintf(stderr, "  | %s\n", line);
    }
    void dumpLog(FILE* f) {
        const int from = logN > 64 ? logN - 64 : 0;
        for (int i = from; i < logN; i++) fprintf(f, "  | %s\n", logs[i % 64]);
    }
};
extern World gW;
