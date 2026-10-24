#include "Game/AudioLib/AudAnmSoundObject.hpp"
#include "Game/AudioLib/AudSoundId.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioWait.hpp"
#include "Game/System/ResourceHolder.hpp"
#include "Game/Util/HashUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/SoundUtil.hpp"
#include <JSystem/JAudio2/JAISound.hpp>
#include <cstring>

struct SoundList {
    union SoundFlags {
        u32 _0;
        u8 _4[4];
    };

    const char* name;
    u32 _4;

    SoundFlags _8;
    const char* _C;

    u32 _10;
    u32 _14;
};

#ifdef TARGET_PC
// The mode byte (bits 0-1: sound, level sound, system SE, system level SE;
// bits 2-3: underwater variant) is the first byte of a u32 written as a
// number: its most significant byte on the big-endian console.  Read as
// _4[0] here it was the least significant one, always 0, so every level
// sound (the turn skid, sliding, the tornado spin wind...) was restarted as
// a one-shot sound each frame.
#define SOUND_FLAGS_BYTE(rEntry) (static_cast< u8 >((rEntry)._8._0 >> 24))
#else
#define SOUND_FLAGS_BYTE(rEntry) ((rEntry)._8._4[0])
#endif

SoundList soundlist[] = {
    {
        "\x90\xba\x8f\xac\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PV_JUMP_S,    // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x92\x86\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PV_JUMP_M,    // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x91\xe5\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PV_JUMP_L,    // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x8c\xe3\x83\x57\x83\x83\x83\x93\x83\x76",   // name
        SE_PV_JUMP_TURN,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x90\xba\x95\x9d\x83\x57\x83\x83\x83\x93\x83\x76",   // name
        SE_PV_JUMP_LONG,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x90\xba\x95\xa8\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PV_JUMP_JOY,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x8d\x82\x94\xf2\x82\xd1\x8d\x9e\x82\xdd",   // name
        SE_PV_HIGH_DIVE,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x90\xba\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76",  // name
        SE_PV_HIP_DROP,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e",   // name
        SE_PV_HIP_DROP_LAND,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x90\xba\x92\x85\x92\x6e\x92\xe2\x8e\x7e",  // name
        SE_PV_LAND,    // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\xba\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93\x92\x85\x92\x6e",  // name
        SE_PV_LAND_COOL,       // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57",  // name
        SE_PV_DAMAGE_S,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x91\xe5\x83\x5f\x83\x81\x81\x5b\x83\x57",  // name
        SE_PV_DAMAGE_L,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x8d\xc5\x8f\x49\x83\x5f\x83\x81\x81\x5b\x83\x57",   // name
        SE_PV_LAST_DAMAGE,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x90\xba\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57",  // name
        SE_PV_BURN,      // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57",   // name
        SE_PV_ELEC_DAMAGE,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x90\xba\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9",       // name
        SE_PV_ELEC_DAMAGE_RECOVER,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\xba\x93\x7c\x82\xea",    // name
        SE_PV_DOWN,  // 0x4
        0,           // 0x8
        0,           // 0xC
        0,           // 0x10
        0,           // 0x14
    },
    {
        "\x90\xba\x97\x8e\x89\xba\x8e\x80\x96\x53",    // name
        SE_PV_FALL_DIE,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x82\xb5\x82\xd1\x82\xea",      // name
        SE_PV_DAMAGE_S,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57",  // name
        SE_PV_FREEZE,    // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x82\xc2\x82\xd4\x82\xea\x83\x5f\x83\x81\x81\x5b\x83\x57",    // name
        SE_PV_DIE_ROCK_CRASH,  // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x83\x58\x83\x73\x83\x93",  // name
        SE_PV_SPIN,  // 0x4
        0,           // 0x8
        0,           // 0xC
        0,           // 0x10
        0,           // 0x14
    },
    {
        "\x90\xba\x83\x58\x83\x73\x83\x93\x83\x4c\x83\x83\x83\x93\x83\x5a\x83\x8b",  // name
        SE_PV_SPIN_CANCEL,     // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x83\x70\x83\x93\x83\x60",   // name
        SE_PV_PUNCH,  // 0x4
        0,            // 0x8
        0,            // 0xC
        0,            // 0x10
        0,            // 0x14
    },
    {
        "\x90\xba\x93\xa5\x82\xdd",     // name
        SE_PV_STOMP,  // 0x4
        0,            // 0x8
        0,            // 0xC
        0,            // 0x10
        0,            // 0x14
    },
    {
        "\x90\xba\x8f\x52\x82\xe8",    // name
        SE_PV_KICK,  // 0x4
        0,           // 0x8
        0,           // 0xC
        0,           // 0x10
        0,           // 0x14
    },
    {
        "\x90\xba\x83\x67\x83\x8b\x83\x6c\x81\x5b\x83\x68",     // name
        SE_PV_TWIST_START,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x90\xba\x95\xc7\x94\xbd\x8e\xcb",   // name
        SE_PV_GUARD,  // 0x4
        0,            // 0x8
        0,            // 0xC
        0,            // 0x10
        0,            // 0x14
    },
    {
        "\x90\xba\x93\x8a\x82\xb0",     // name
        SE_PV_THROW,  // 0x4
        0,            // 0x8
        0,            // 0xC
        0,            // 0x10
        0,            // 0x14
    },
    {
        "\x90\xba\x95\xc7\x89\x9f\x82\xb5",     // name
        SE_PV_LIFT_UP,  // 0x4
        0,              // 0x8
        0,              // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x90\xba\x95\xc7\x91\xcc\x93\x96\x82\xbd\x82\xe8",       // name
        SE_PV_WALL_HIT_BODY,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x90\xba\x83\x58\x83\x73\x83\x93\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76",  // name
        SE_PV_PUNCH,           // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x8c\x79\x82\xa2\x97\xcd\x82\xdd",  // name
        SE_PV_CATCH,   // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\xba\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8",         // name
        SE_PV_CLIFF_FALL_HANG,  // 0x4
        0,                      // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x90\xba\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9",  // name
        SE_PV_CLIFF_CLIMB,   // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\xba\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86",  // name
        SE_PV_BURN_RUN,    // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x90\xba\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9",  // name
        SE_PV_BURN_RECOVER,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\xba\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9",    // name
        SE_PV_FREEZE_RECOVER,  // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x90\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57",       // name
        SE_PV_NEEDLE_DAMAGE,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x90\xba\x90\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86",         // name
        SE_PV_NEEDLE_DAMAGE_RUN,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x90\xba\x90\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9",           // name
        SE_PV_NEEDLE_DAMAGE_RECOVER,  // 0x4
        0,                            // 0x8
        0,                            // 0xC
        0,                            // 0x10
        0,                            // 0x14
    },
    {
        "\x90\xba\x90\x85\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57",      // name
        SE_PV_DAMAGE_S_WATER,  // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x90\x85\x92\x86\x8d\xc5\x8f\x49\x83\x5f\x83\x81\x81\x5b\x83\x57",     // name
        SE_PV_LAST_DAMAGE_WATER,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x90\xba\x97\xe2\x90\x85\x83\x5f\x83\x81\x81\x5b\x83\x57",         // name
        SE_PV_COLD_WATER_DAMAGE,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x90\xba\x83\x58\x83\x50\x83\x4c\x83\x88\x8a\x4a\x8e\x6e",  // name
        SE_PV_BURY_HEAD,   // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x90\xba\x83\x58\x83\x50\x83\x4c\x83\x88\x8f\x49\x97\xb9",         // name
        SE_PV_BURY_HEAD_RECOVER,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x90\xba\x91\xab\x96\x84\x82\xdc\x82\xe8\x8a\x4a\x8e\x6e",  // name
        SE_PV_BURY_FOOT,   // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x90\xba\x91\xab\x96\x84\x82\xdc\x82\xe8\x8f\x49\x97\xb9",         // name
        SE_PV_BURY_FOOT_RECOVER,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x90\xba\x82\xa0\x82\xad\x82\xd1",  // name
        SE_PV_YAWN,  // 0x4
        0,           // 0x8
        0,           // 0xC
        0,           // 0x10
        0,           // 0x14
    },
    {
        "\x90\xba\x82\xa2\x82\xd1\x82\xab\x82\x50",   // name
        SE_PV_SLEEP_1,  // 0x4
        0,              // 0x8
        0,              // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x90\xba\x82\xa2\x82\xd1\x82\xab\x82\x51",   // name
        SE_PV_SLEEP_2,  // 0x4
        0,              // 0x8
        0,              // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x90\xba\x95\xac\x90\x85\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PV_UPSET,       // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x90\xba\x83\x89\x83\x93\x83\x6a\x83\x93\x83\x4f\x83\x4c\x83\x62\x83\x4e",  // name
        SE_PV_KICK,            // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x90\xba\x8d\xbb\x92\x45\x8f\x6f",     // name
        SE_PV_LIFT_UP,  // 0x4
        0,              // 0x8
        0,              // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x90\xba\x93\x8a\x82\xb0\x82\xe7\x82\xea",  // name
        SE_PV_THROWN,  // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\xba\x8d\x51\x82\xc4",     // name
        SE_PV_UPSET,  // 0x4
        0,            // 0x8
        0,            // 0xC
        0,            // 0x10
        0,            // 0x14
    },
    {
        "\x90\xba\x82\xb5\x82\xe1\x82\xaa\x82\xde",  // name
        SE_PV_SQUAT,   // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\xba\x8d\xbb\x92\xbe\x82\xdd",     // name
        SE_PV_DIE_MUD,  // 0x4
        0,              // 0x8
        0,              // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x90\xba\x8d\xbb\x92\xbe\x82\xdd\x8e\x80\x96\x53",  // name
        SE_PV_MUD_SINK,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x90\xba\x8f\xc0\x92\xbe\x82\xdd",     // name
        SE_PV_DIE_MUD,  // 0x4
        0,              // 0x8
        0,              // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x90\xba\x8f\xc0\x92\xbe\x82\xdd\x8e\x80\x96\x53",  // name
        SE_PV_MUD_SINK,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x91\xab\x89\xb9\x8d\xb6",          // name
        SE_PM_FOOTNOTE_L,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x91\xab\x89\xb9\x89\x45",          // name
        SE_PM_FOOTNOTE_R,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x57\x83\x83\x83\x93\x83\x76\x93\xa5\x90\xd8",    // name
        SE_PM_JUMP,        // 0x4
        0x4000000,         // 0x8
        "\x90\x85\x92\xb5\x82\xcb\x83\x57\x83\x83\x83\x93\x83\x76",  // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x92\x85\x92\x6e",        // name
        SE_PM_LAND,    // 0x4
        0x4000000,     // 0x8
        "\x90\x85\x92\xb5\x82\xcb\x92\x85\x92\x6e",  // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x8f\x64\x82\xa2\x92\x85\x92\x6e",        // name
        SE_PM_LAND_HEAVY,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x92\xcd\x82\xdd",          // name
        SE_PM_GRAB_OBJ,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x82\xd4\x82\xe7\x82\xb3\x82\xaa\x82\xe8",  // name
        SE_PM_HAND,    // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x89\xf1\x93\x5d",   // name
        SE_PM_PRE_HIPDROP,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e",  // name
        SE_PM_HIPDROP,     // 0x4
        0x4000000,         // 0x8
        "\x90\x85\x92\xb5\x82\xcb\x92\x85\x92\x6e",      // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x58\x83\x73\x83\x93\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x89\xf1\x93\x5d",    // name
        SE_PM_SPIN_HIP_DROP_TURN,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x83\x58\x83\x73\x83\x93\x90\x4b\x83\x68\x83\x8d\x83\x62\x83\x76\x97\x8e\x89\xba",       // name
        SE_PM_LV_SPIN_HIP_DROP_FALL,  // 0x4
        0x1000000,                    // 0x8
        0,                            // 0xC
        0,                            // 0x10
        0,                            // 0x14
    },
    {
        "\x92\x86\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_JUMP_M,  // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x91\xe5\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_JUMP_L,  // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x8c\xe3\x83\x57\x83\x83\x83\x93\x83\x76",     // name
        SE_PM_JUMP_TURN,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x95\x9d\x83\x57\x83\x83\x83\x93\x83\x76",     // name
        SE_PM_JUMP_LONG,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x83\x67\x83\x8b\x83\x6c\x81\x5b\x83\x68\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_TWIST_JUMP,      // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76",   // name
        SE_PM_SPIN_ATTACK,  // 0x4
        0x4000000,          // 0x8
        "\x90\x85\x92\xb5\x82\xcb\x83\x57\x83\x83\x83\x93\x83\x76",   // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x83\x67\x83\x89\x83\x93\x83\x7c\x83\x8a\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76\x8f\xac",  // name
        SE_OJ_TRAMPOLINE_BOUND_S,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x83\x67\x83\x89\x83\x93\x83\x7c\x83\x8a\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76\x91\xe5",  // name
        SE_OJ_TRAMPOLINE_BOUND_L,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76",  // name
        SE_PM_SLIP_UP,     // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x5f\x83\x81\x81\x5b\x83\x57",      // name
        SE_PM_DAMAGE_S,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57",  // name
        SE_PM_BURN,    // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57",         // name
        SE_PM_NEEDLE_DAMAGE,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57",     // name
        SE_PM_ELEC_DAMAGE,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x95\xc7\x94\xbd\x8e\xcb",             // name
        SE_PM_SPIN_HIT_WALL,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x76\x83\x8c\x83\x58\x83\x5f\x83\x81\x81\x5b\x83\x57",      // name
        SE_PM_DAMAGE_STOMPED,  // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57",      // name
        SE_PM_ICE_DAMAGE,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x93\x7c\x82\xea",            // name
        SE_PM_FALLDOWN_S,  // 0x4
        0x4000000,         // 0x8
        "\x90\x85\x92\xb5\x82\xcb\x8d\xb6\x91\xab",      // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x90\x81\x82\xc1\x94\xf2\x82\xd1\x93\x7c\x82\xea",    // name
        SE_PM_FALLDOWN_M,  // 0x4
        0x4000000,         // 0x8
        "\x90\x85\x92\xb5\x82\xcb\x92\x85\x92\x6e",      // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x8d\xe2\x8a\x8a\x82\xe8",                 // name
        SE_PM_LV_SLIP_SLIP_CODE,  // 0x4
        0x9000000,                // 0x8
        "\x90\x85\x96\xca\x8a\x8a\x82\xe8",               // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x83\x8b\x83\x43\x81\x5b\x83\x57\x8a\x8a\x82\xe8",            // name
        SE_PM_LV_LUIGI_WALK_SLIP,  // 0x4
        0x9000000,                 // 0x8
        "\x90\x85\x96\xca\x8a\x8a\x82\xe8",                // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x83\x70\x83\x93\x83\x60\x95\x97\x90\xd8\x82\xe8",     // name
        SE_PM_PUNCH_SHOOT,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57\x95\x9c\x8b\x41\x83\x6f\x83\x45\x83\x93\x83\x68",  // name
        SE_PM_BURN_JUMP,           // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57\x89\x8a\x8f\xe3\x92\x86",  // name
        SE_PM_LV_BURNING,    // 0x4
        0x1000000,           // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x95\x58\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9",          // name
        SE_PM_ICE_DAMAGE_RECOVER,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x88\xf8\x82\xab\x96\xdf\x82\xb5\x8a\xee\x96\x7b",           // name
        SE_PM_LV_PULL_BACK_BASE,  // 0x4
        0x1000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x88\xf8\x82\xab\x96\xdf\x82\xb5\x95\x82\x97\x56",          // name
        SE_PM_LV_PULL_BACK_FLY,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x88\xf8\x82\xab\x96\xdf\x82\xb5\x96\x41\x94\x6a\x97\xf4",        // name
        SE_OJ_GCAPTURE_RELEASE,  // 0x4
        0,                       // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea",             // name
        SE_PM_LV_FLIP_DAMAGE_TURN,  // 0x4
        0x1000000,                  // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x8c\x8b\x8a\x45\x83\x71\x83\x62\x83\x67",                // name
        SE_PM_KAMECK_BARRIER_BOUND,  // 0x4
        0,                           // 0x8
        0,                           // 0xC
        0,                           // 0x10
        0,                           // 0x14
    },
    {
        "\x95\xc7\x8f\xd5\x93\xcb",             // name
        SE_PM_WALL_HIT_BODY,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x93\x8a\x82\xb0\x82\xe7\x82\xea",       // name
        SE_PM_FLIP_AWAY,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x83\x77\x83\x8a\x83\x52\x83\x76\x83\x5e\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_HELI_JUMP,         // 0x4
        0,                       // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x91\xd8\x8b\xf3\x8a\x4a\x8e\x6e",                 // name
        SE_PM_HELI_JUMP_AIR_START,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x91\xd8\x8b\xf3\x92\x86",                // name
        SE_PM_LV_HELI_JUMP_AIR,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x83\x7a\x83\x62\x83\x70\x81\x5b\x92\xb5\x82\xcb\x95\xd4\x82\xe8",  // name
        SE_PM_HOPPER_BOUND,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76\x97\xad\x82\xdf",    // name
        SE_PM_LV_HOPPER_PRE_JUMP,  // 0x4
        0x1000000,                 // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_HOPPER_JUMP,   // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x83\x65\x83\x8c\x83\x54\x95\x82\x97\x56",               // name
        SE_PM_LV_TERESA_MARIO_FLY,  // 0x4
        0x1000000,                  // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x83\x65\x83\x8c\x83\x54\x93\xa5\x82\xf1\x92\xa3\x82\xe8",   // name
        SE_PM_LV_AIR_WALK,  // 0x4
        0x1000000,          // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x83\x65\x83\x8c\x83\x54\x95\xc7\x94\xbd\x8e\xcb",            // name
        SE_PM_TERESA_MARIO_BOUND,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x83\x65\x83\x8c\x83\x54\x8f\xc1\x82\xa6\x82\xe9",             // name
        SE_PM_TERESA_MARIO_VANISH,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x83\x65\x83\x8c\x83\x54\x8c\xbb\x82\xea\x82\xe9",             // name
        SE_PM_TERESA_MARIO_APPEAR,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x83\x65\x83\x8c\x83\x54\x95\x97\x82\xc9\x8f\xe6\x82\xe9",            // name
        SE_PM_LV_T_MARIO_RIDE_WIND,  // 0x4
        0x1000000,                   // 0x8
        0,                           // 0xC
        0,                           // 0x10
        0,                           // 0x14
    },
    {
        "\x83\x6e\x83\x60\x94\xf2\x8d\x73\x92\x86",            // name
        SE_PM_LV_BEE_MARIO_FLY,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x83\x58\x83\x50\x83\x4c\x83\x88\x8a\x4a\x8e\x6e",    // name
        SE_PM_BURY_START,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x58\x83\x50\x83\x4c\x83\x88\x8f\x49\x97\xb9",  // name
        SE_PM_BURY_END,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x83\x58\x83\x50\x83\x4c\x83\x88\x8f\x49\x97\xb9\x83\x58\x83\x73\x83\x93",  // name
        SE_PM_BURY_END_SPIN,   // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x83\x58\x83\x50\x81\x5b\x83\x67\x91\xab",  // name
        SE_PM_SKATE,   // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x83\x58\x83\x50\x81\x5b\x83\x67\x8a\x8a\x82\xe8",       // name
        SE_PM_LV_SKATE_SLIP,  // 0x4
        0x1000000,            // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_SKATE_JUMP,    // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e",    // name
        SE_PM_SKATE_LAND,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x58\x83\x73\x83\x93",  // name
        SE_PM_SKATE_SPIN,  // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x6e\x83\x60\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab",     // name
        SE_PM_BEE_WALL_LAND,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x6e\x83\x60\x95\xc7\x95\xe0\x82\xab\x8d\xb6",         // name
        SE_PM_BEE_WALL_WALK_L,  // 0x4
        0,                      // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x83\x6e\x83\x60\x95\xc7\x95\xe0\x82\xab\x89\x45",         // name
        SE_PM_BEE_WALL_WALK_R,  // 0x4
        0,                      // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x83\x6e\x83\x60\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76",     // name
        SE_PM_BEE_WALL_JUMP,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x8d\xbb\x92\xbe\x82\xdd",            // name
        SE_PM_LV_SAND_SINK,  // 0x4
        0x1000000,           // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x8d\xbb\x92\x45\x8f\x6f",        // name
        SE_PM_SAND_OUT,  // 0x4
        0,               // 0x8
        0,               // 0xC
        0,               // 0x10
        0,               // 0x14
    },
    {
        "\x8d\xbb\x8b\xad\x90\xa7\x92\xbe\x82\xdd",              // name
        SE_PM_LV_SAND_SINK_FORCE,  // 0x4
        0x1000000,                 // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x8d\xbb\x8e\x80\x96\x53",           // name
        SE_PM_LV_SAND_DIE,  // 0x4
        0x1000000,          // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x8f\xc0\x8b\xad\x90\xa7\x92\xbe\x82\xdd",             // name
        SE_PM_LV_MUD_SINK_FORCE,  // 0x4
        0x1000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x8f\xc0\x8e\x80\x96\x53",          // name
        SE_PM_LV_MUD_DIE,  // 0x4
        0x1000000,         // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x93\xc5\x8f\xc0\x92\x45\x8f\x6f",            // name
        SE_PM_POISON_MUD_OUT,  // 0x4
        0,                     // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x93\xc5\x8f\xc0\x83\x5f\x83\x81\x81\x5b\x83\x57",           // name
        SE_PM_POISON_MUD_DAMAGE,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x83\x5f\x81\x5b\x83\x4e\x83\x7d\x83\x5e\x81\x5b\x92\xbe\x82\xdd",       // name
        SE_PM_LV_DARK_MATTER_IN,  // 0x4
        0x1000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x98\x66\x90\xaf\x8a\xd1\x92\xca\x92\x86",            // name
        SE_PM_LV_WARP_STRAIGHT,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x98\x66\x90\xaf\x8a\xd1\x92\xca\x8f\x49\x97\xb9",           // name
        SE_PM_WARP_STRAIGHT_END,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x83\x74\x81\x5b\x91\xd8\x8b\xf3\x92\x86",           // name
        SE_PM_LV_FOO_FLY_WAIT,  // 0x4
        0x1000000,              // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x83\x74\x81\x5b\x89\xc1\x91\xac",       // name
        SE_PM_FOO_ACCEL,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x83\x74\x81\x5b\x94\xf2\x8d\x73\x92\x86",         // name
        SE_PM_LV_FOO_FLYING,  // 0x4
        0x1000000,            // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x74\x81\x5b\x83\x75\x83\x8c\x81\x5b\x83\x4c",   // name
        SE_PM_FOO_BRAKE,  // 0x4
        0,                // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x83\x8f\x81\x5b\x83\x76\x83\x7c\x83\x62\x83\x68\x93\xfc\x82\xe8",  // name
        SE_PM_WARP_POD_IN,   // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x83\x8f\x81\x5b\x83\x76\x83\x7c\x83\x62\x83\x68\x8f\x6f",    // name
        SE_PM_WARP_POD_OUT,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x83\x8f\x81\x5b\x83\x76\x83\x7c\x83\x62\x83\x68\x88\xda\x93\xae",      // name
        SE_PM_LV_WARP_POD_MOVE,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x83\x58\x83\x79\x83\x56\x83\x83\x83\x8b\x83\x5f\x83\x62\x83\x56\x83\x85\x8b\xad",   // name
        SE_PM_RACE_START_DASH_L,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x83\x58\x83\x79\x83\x56\x83\x83\x83\x8b\x83\x5f\x83\x62\x83\x56\x83\x85\x8e\xe3",   // name
        SE_PM_RACE_START_DASH_S,  // 0x4
        0,                        // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x83\x5f\x83\x62\x83\x56\x83\x85\x89\xc1\x91\xac\x8b\xad\x90\xac\x8c\xf7",  // name
        SE_SY_GET_DASH_RING,   // 0x4
        0x2000000,             // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x83\x75\x83\x89\x83\x62\x83\x4e\x83\x7a\x81\x5b\x83\x8b\x8b\x7a\x82\xa2\x8d\x9e\x82\xdc\x82\xea",  // name
        SE_PM_BLACK_HOLE_IN,         // 0x4
        0,                           // 0x8
        0,                           // 0xC
        0,                           // 0x10
        0,                           // 0x14
    },
    {
        "\x90\x85\x95\xe0\x8d\x73\x93\xcb\x93\xfc",        // name
        SE_PM_WALK_TO_SWIM,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\x85\x95\xe0\x8d\x73\x92\x45\x8f\x6f",        // name
        SE_PM_SWIM_TO_WALK,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\x85\x96\xca\x83\x45\x83\x47\x83\x43\x83\x67",       // name
        SE_PM_WAIT_ON_WATER,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x90\x85\x92\x86\x83\x45\x83\x47\x83\x43\x83\x67",          // name
        SE_PM_WAIT_UNDER_WATER,  // 0x4
        0,                       // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x90\x85\x97\x8e\x89\xba\x93\xcb\x93\xfc",         // name
        SE_PM_DIVE_TO_WATER,  // 0x4
        0,                    // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x90\x85\x83\x57\x83\x83\x83\x93\x83\x76\x92\x45\x8f\x6f",       // name
        SE_PM_JUMP_FROM_WATER,  // 0x4
        0,                      // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x90\x85\x96\xca\x88\xea\x91\x7e\x82\xab",               // name
        SE_PM_SWIM_ACCEL_ON_WATER,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\x85\x92\x86\x88\xea\x91\x7e\x82\xab",               // name
        SE_PM_SWIM_ACCEL_IN_WATER,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\x85\x96\xca\x83\x6f\x83\x5e\x91\xab",            // name
        SE_PM_LV_SWIM_ON_WATER,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x90\x85\x92\x86\x83\x6f\x83\x5e\x91\xab",            // name
        SE_PM_LV_SWIM_IN_WATER,  // 0x4
        0x1000000,               // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x90\x85\x96\xca\x90\xf6\x82\xe8",                // name
        SE_PM_DIVE_WATER_ROLLING,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x90\x85\x92\x86\x90\xf6\x82\xe8",                // name
        SE_PM_DIVE_FAST_IN_WATER,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x90\x85\x96\xca\x83\x58\x83\x73\x83\x93\x8a\x4a\x8e\x6e",           // name
        SE_PM_TORNADE_ON_WATER_ST,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\x85\x96\xca\x83\x58\x83\x73\x83\x93",               // name
        SE_PM_LV_TORNADE_ON_WATER,  // 0x4
        0x1000000,                  // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\x85\x92\x86\x83\x58\x83\x73\x83\x93\x8a\x4a\x8e\x6e",           // name
        SE_PM_TORNADE_IN_WATER_ST,  // 0x4
        0,                          // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\x85\x92\x86\x83\x58\x83\x73\x83\x93",               // name
        SE_PM_LV_TORNADE_IN_WATER,  // 0x4
        0x1000000,                  // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x90\x85\x96\xca\x83\x5f\x83\x81\x81\x5b\x83\x57",         // name
        SE_PM_DAMAGE_ON_WATER,  // 0x4
        0,                      // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x90\x85\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57",         // name
        SE_PM_DAMAGE_IN_WATER,  // 0x4
        0,                      // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x8b\x54\x83\x57\x83\x46\x83\x62\x83\x67\x89\x6a\x82\xac",          // name
        SE_OJ_LV_TURTLE_JET_SWIM,  // 0x4
        0x1000000,                 // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x90\x85\x92\xea\x90\xda\x90\x47",                 // name
        SE_PM_LV_TURTLE_SEA_SMOKE,  // 0x4
        0x1000000,                  // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x8b\x54\x95\xc7\x83\x71\x83\x62\x83\x67",              // name
        SE_OJ_TURTLE_JET_BOUND_W,  // 0x4
        0,                         // 0x8
        0,                         // 0xC
        0,                         // 0x10
        0,                         // 0x14
    },
    {
        "\x8b\x54\x83\x75\x83\x8c\x81\x5b\x83\x4c",        // name
        SE_PM_TURTLE_BRAKE,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x8b\x54\x89\xc1\x91\xac",            // name
        SE_PM_TURTLE_ACCEL,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x8d\xb6\x91\xab",        // name
        SE_PM_FOOTNOTE_L_W,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x89\x45\x91\xab",        // name
        SE_PM_FOOTNOTE_R_W,  // 0x4
        0,                   // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x8d\xb6\x91\xab\x8f\xac",          // name
        SE_PM_FOOTNOTE_SUB_L_W,  // 0x4
        0,                       // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x89\x45\x91\xab\x8f\xac",          // name
        SE_PM_FOOTNOTE_SUB_R_W,  // 0x4
        0,                       // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x83\x57\x83\x83\x83\x93\x83\x76",  // name
        SE_PM_JUMP_W,      // 0x4
        0,                 // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x92\x85\x92\x6e",  // name
        SE_PM_LAND_W,  // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\x85\x92\xb5\x82\xcb\x8e\xe8",    // name
        SE_PM_HAND_W,  // 0x4
        0,             // 0x8
        0,             // 0xC
        0,             // 0x10
        0,             // 0x14
    },
    {
        "\x90\x85\x92\x65\x82\xa9\x82\xea",         // name
        SE_PM_WATER_BOUND,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x90\x85\x96\xca\x8a\x8a\x82\xe8",           // name
        SE_PM_LV_SLIP_WATER,  // 0x4
        0x1000000,            // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x67\x83\x8b\x83\x6c\x81\x5b\x83\x68\x95\x97",       // name
        SE_PM_LV_TWIST_WIND,  // 0x4
        0x1000000,            // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x58\x83\x8a\x83\x62\x83\x76",     // name
        SE_PM_LV_SLIP,  // 0x4
        0x9000000,      // 0x8
        "\x90\x85\x96\xca\x8a\x8a\x82\xe8",     // 0xC
        0,              // 0x10
        0,              // 0x14
    },
    {
        "\x8d\xc5\x8c\xe3\x82\xcc\x88\xea\x8c\x82",       // name
        SE_PM_LAST_DAMAGE,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x8b\xf3\x92\x86\x82\xd3\x82\xf1\x82\xce\x82\xe8",     // name
        SE_PM_LV_AIR_WALK,  // 0x4
        0x1000000,          // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x83\x58\x83\x73\x83\x93\x8b\x96\x89\xc2",       // name
        SE_PM_SPIN_ENABLE,  // 0x4
        0,                  // 0x8
        0,                  // 0xC
        0,                  // 0x10
        0,                  // 0x14
    },
    {
        "\x83\x58\x83\x73\x83\x93\x89\xf1\x95\x9c\x8f\x49\x97\xb9",        // name
        SE_PM_SPIN_RECOVER_END,  // 0x4
        0,                       // 0x8
        0,                       // 0xC
        0,                       // 0x10
        0,                       // 0x14
    },
    {
        "\x83\x67\x83\x8b\x83\x6c\x81\x5b\x83\x68\x94\xf2\x8d\x73",         // name
        SE_PM_LV_TORNADE_FLYING,  // 0x4
        0x1000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x83\x89\x83\x43\x83\x74\x8c\x78\x8d\x90",         // name
        SE_SY_LV_LIFE_ALERT,  // 0x4
        0x3000000,            // 0x8
        0,                    // 0xC
        0,                    // 0x10
        0,                    // 0x14
    },
    {
        "\x83\x89\x83\x43\x83\x74\x89\xf1\x95\x9c",        // name
        SE_SY_LIFE_RECOVER,  // 0x4
        0x2000000,           // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x90\x85\x92\x86\x83\x89\x83\x43\x83\x74\x8c\xb8\x8f\xad",      // name
        SE_SY_WATER_LIFE_DEC,  // 0x4
        0x2000000,             // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x8e\x5f\x91\x66\x8c\xb8\x8f\xad\x8c\x78\x8d\x90",      // name
        SE_SY_OXYGEN_ALERT,  // 0x4
        0x2000000,           // 0x8
        0,                   // 0xC
        0,                   // 0x10
        0,                   // 0x14
    },
    {
        "\x96\xb3\x8e\x5f\x91\x66\x8c\x78\x8d\x90",             // name
        SE_SY_OXYGEN_ZERO_ALERT,  // 0x4
        0x2000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x8e\x5f\x91\x66\x89\xf1\x95\x9c",            // name
        SE_SY_INC_OXYGEN_ONE,  // 0x4
        0x2000000,             // 0x8
        0,                     // 0xC
        0,                     // 0x10
        0,                     // 0x14
    },
    {
        "\x8e\x5f\x91\x66\x8a\xae\x91\x53\x89\xf1\x95\x9c",         // name
        SE_SY_INC_OXYGEN_FULL,  // 0x4
        0x2000000,              // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "\x90\x85\x96\xca\x8e\x5f\x91\x66\x89\xf1\x95\x9c",           // name
        SE_SY_LV_RECOVER_OXYGEN,  // 0x4
        0x3000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x90\x85\x96\xca\x8e\x5f\x91\x66\x8a\xae\x91\x53\x89\xf1\x95\x9c",         // name
        SE_SY_RECOVER_OXYGEN_FULL,  // 0x4
        0x2000000,                  // 0x8
        0,                          // 0xC
        0,                          // 0x10
        0,                          // 0x14
    },
    {
        "\x96\xb3\x8e\x5f\x91\x66\x83\x5f\x83\x81\x81\x5b\x83\x57",  // name
        SE_SY_NO_OXYGEN,   // 0x4
        0x2000000,         // 0x8
        0,                 // 0xC
        0,                 // 0x10
        0,                 // 0x14
    },
    {
        "\x83\x6e\x83\x60\x91\xcc\x97\xcd\x89\xf1\x95\x9c",               // name
        SE_SY_LV_INC_AIR_WALK_TIMER,  // 0x4
        0x3000000,                    // 0x8
        0,                            // 0xC
        0,                            // 0x10
        0,                            // 0x14
    },
    {
        "\x83\x6e\x83\x60\x91\xcc\x97\xcd\x8a\xae\x91\x53\x89\xf1\x95\x9c",          // name
        SE_SY_INC_AIR_WALK_TIMER_F,  // 0x4
        0x2000000,                   // 0x8
        0,                           // 0xC
        0,                           // 0x10
        0,                           // 0x14
    },
    {
        "\x83\x6e\x83\x60\x91\xcc\x97\xcd\x90\xd8\x82\xea",           // name
        SE_SY_NO_AIR_WALK_TIMER,  // 0x4
        0x2000000,                // 0x8
        0,                        // 0xC
        0,                        // 0x10
        0,                        // 0x14
    },
    {
        "\x95\xcf\x90\x67\x89\xf0\x8f\x9c",       // name
        SE_SY_MORPH_END,  // 0x4
        0x2000000,        // 0x8
        0,                // 0xC
        0,                // 0x10
        0,                // 0x14
    },
    {
        "\x8e\xf4\x82\xa2\x89\xf0\x8f\x9c",             // name
        SE_SY_MORPH_TO_NORMAL,  // 0x4
        0x2000000,              // 0x8
        0,                      // 0xC
        0,                      // 0x10
        0,                      // 0x14
    },
    {
        "",  // name
        0,   // 0x4
        0,   // 0x8
        0,   // 0xC
        0,   // 0x10
        0,   // 0x14
    },
};

struct SoundSwapList {
    const char* name;

    u32 offset1;
    u32 offset2;
    u32 offset3;
};

SoundSwapList soundswaplist[] = {{"", 0, 0, 0}};

u32 Mario::initSoundTable(SoundList* pList, u32 globalTablePosition) {
    u32* pSwapOffset = reinterpret_cast< u32* >(soundswaplist) + globalTablePosition;
    SoundList* pEntry;
    u32 count = 0;
    s32 i = 0;
    while (true) {
        pEntry = pList + i;
        if (pEntry->name[0] == '\0') {
            break;
        }

        pEntry->_10 = 0;
        pEntry->_14 = pEntry->_4;

        if (globalTablePosition != 0) {
            s32 swapIndex = 0;
            while (true) {
                const SoundSwapList* pSwapEntry = soundswaplist + swapIndex;
                if (pSwapEntry->name[0] == '\0') {
                    break;
                }

                if (strcmp(pEntry->name, pSwapEntry->name) == 0) {
                    u32 soundID = pSwapOffset[swapIndex * 4];
                    if (soundID != 0) {
                        pEntry->_14 = soundID;
                    }
                    break;
                }

                swapIndex++;
            }
        }

        count++;
        i++;
    }

    return count;
}

void Mario::initSound() {
    u32 count = initSoundTable(soundlist, 0);
    _96C = new HashSortTable(count);
    for (u32 i = 0; i < count; i++) {
        _96C->add(soundlist[i].name, i, false);
    }

    _96C->sort();
    _970 = nullptr;
}

void Mario::playSoundJ(const char* pSoundName, s32 timing) {
    u32 index;
    if (_96C->search(pSoundName, &index)) {
        switch (SOUND_FLAGS_BYTE(soundlist[index]) & 0x3) {
        case 0:
            MR::startSound(mActor, soundlist[index]._14, timing);
            break;

        case 2:
            MR::startSystemSE(soundlist[index]._14, timing);
            break;

        case 1:
            MR::startLevelSound(mActor, soundlist[index]._14, timing);
            break;

        case 3:
            MR::startSystemLevelSE(soundlist[index]._14, timing);
            break;
        }

        switch (SOUND_FLAGS_BYTE(soundlist[index]) & ~0x3) {
        case 0x4:
        case 0x8:
            if (mDrawStates.mIsUnderwater || mDrawStates._13) {
                playSoundJ(soundlist[index]._C, -1);
            }
            break;
        }
    }

    bool isFound = _96C->search("\x90\xba", pSoundName, &index);
    if (isFound) {
        MR::startSound(mActor, soundlist[index]._14, timing);
    }
}

void Mario::stopSoundJ(const char* pSoundName, u32 delay) {
    u32 index;
    if (_96C->search(pSoundName, &index)) {
        switch (SOUND_FLAGS_BYTE(soundlist[index]) & 0x3) {
        case 0:
            MR::stopSound(mActor, soundlist[index]._14, delay);
            break;

        case 1:
            break;

        case 2:
            MR::stopSystemSE(soundlist[index]._14, delay);
            break;

        case 3:
            break;
        }
    }

    if (_96C->search("\x90\xba", pSoundName, &index)) {
        JAISoundID soundID(soundlist[index]._14);
        MR::stopSound(mActor, soundID, delay);
    }
}

void Mario::startBas(const char* pAnimName, bool arg2, f32 startFrame, f32 speed) {
    if (mActor->mSoundObject != nullptr) {
        ResourceHolder* pHolder = MR::getResourceHolder(mActor);
        const JAUSoundAnimation* pRes = nullptr;

        if (pAnimName != nullptr && pHolder->mBasResTable->isExistRes(pAnimName)) {
            pRes = static_cast< JAUSoundAnimation* >(pHolder->mBasResTable->getRes(pAnimName));
        }

        if (pRes != nullptr) {
            mActor->mSoundObject->startAnimation(pRes, arg2, startFrame, speed);
        } else {
            mActor->mSoundObject->removeAnimation();
        }

        _970 = pAnimName;
    }
}

bool Mario::isRunningBas(const char* pAnimName) const {
    if (_970 == nullptr) {
        return false;
    }

    if (strcmp(_970, pAnimName) == 0) {
        return true;
    }

    return false;
}

void Mario::skipBas(f32 frame) {
    mActor->mSoundObject->skip(frame);
}

void Mario::playSoundTeresaFlying() {
    if (getPlayerMode() != 6) {
        return;
    }

    s32 timing = 100;
    if (getCurrentStatus() == 0x1C) {
        if (mWait->_14 == 0) {
            timing = 100 - static_cast< s32 >(mWait->_16);
            if (timing < 0) {
                timing = 0;
            }
        } else {
            timing = 0;
        }
    }

    playSound("\x83\x65\x83\x8c\x83\x54\x95\x82\x97\x56", timing);
}

void Mario::playSoundTrampleCombo(u8 combo) {
    if (combo >= 7) {
        return;
    }

    MR::startSystemSE("SE_SY_TRAMPLE_COMBO", combo);
}

void Mario::setSeVersion(u32 version) {
    MR::setSeVersion(mActor, version);
}
