#pragma once

#include "Game/Animation/XanimeResource.hpp"

XanimeBckTable4 quadAnimeTable[] = {
    {
        {"\x8a\xee\x96\x7b"},        // mParent
        "WalkSoft",      // fileName1
        0.00000000000f,  // 0x8
        "Walk",          // fileName2
        0.00000000000f,  // 0x10
        "Run",           // fileName3
        0.00000000000f,  // 0x18
        "Wait",          // fileName4
        1.00000000000f,  // 0x20
    },
    {
        {"\x90\x85\x89\x6a\x8a\xee\x96\x7b"},          // mParent
        "SwimWait",            // fileName1
        1.00000000000f,        // 0x8
        "SwimFlutterSurface",  // fileName2
        0.00000000000f,        // 0x10
        "SwimFlutter",         // fileName3
        0.00000000000f,        // 0x18
        "SwimDrift",           // fileName4
        0.00000000000f,        // 0x20
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x8a\xee\x96\x7b"},  // mParent
        "WalkSoft",      // fileName1
        0.00000000000f,  // 0x8
        "Walk",          // fileName2
        0.00000000000f,  // 0x10
        "TennisRun",     // fileName3
        0.00000000000f,  // 0x18
        "TennisWait",    // fileName4
        1.00000000000f,  // 0x20
    },
    {
        {"\x83\x58\x83\x89\x83\x43\x83\x5f\x81\x5b\x90\x4b"},    // mParent
        "SlideHipForWard",   // fileName1
        0.300000011921f,     // 0x8
        "SlideHipBackward",  // fileName2
        0.200000002980f,     // 0x10
        "SlideHipLeft",      // fileName3
        0.250000000000f,     // 0x18
        "SlideHipRight",     // fileName4
        0.250000000000f,     // 0x20
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x82\xa0\x82\xa8\x82\xde\x82\xaf"},  // mParent
        "SlideHipForWard",           // fileName1
        0.300000011921f,             // 0x8
        "SlideHipBackward",          // fileName2
        0.200000002980f,             // 0x10
        "SlideHipLeft",              // fileName3
        0.250000000000f,             // 0x18
        "SlideHipRight",             // fileName4
        0.250000000000f,             // 0x20
    },
    {
        {"\x83\x5e\x83\x7d\x83\x52\x83\x8d\x88\xda\x93\xae"},  // mParent
        "BallIdle",        // fileName1
        1.00000000000f,    // 0x8
        "BallWalkSoft",    // fileName2
        0.00000000000f,    // 0x10
        "BallWalk",        // fileName3
        0.00000000000f,    // 0x18
        "BallRun",         // fileName4
        0.00000000000f,    // 0x20
    },
    {
        {""},            // mParent
        "",              // fileName1
        0.00000000000f,  // 0x8
        nullptr,         // fileName2
        0.00000000000f,  // 0x10
        nullptr,         // fileName3
        0.00000000000f,  // 0x18
        nullptr,         // fileName4
        0.00000000000f,  // 0x20
    },
};

XanimeBckTable3 tripleAnimeTable[] = {
    {
        {"\x8d\xe2\x8d\xb6\x89\x45\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "Wait",              // fileName1
        1.00000000000f,      // 0x8
        "WaitSlopeL",        // fileName2
        0.00000000000f,      // 0x10
        "WaitSlopeR",        // fileName3
        0.00000000000f,      // 0x18
    },
    {
        {"\x8d\xe2\x91\x4f\x8c\xe3\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "Wait",              // fileName1
        1.00000000000f,      // 0x8
        "WaitSlopeD",        // fileName2
        0.00000000000f,      // 0x10
        "WaitSlopeU",        // fileName3
        0.00000000000f,      // 0x18
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93"},   // mParent
        "SurfRideLoop",   // fileName1
        1.00000000000f,   // 0x8
        "SurfRideLoopL",  // fileName2
        0.00000000000f,   // 0x10
        "SurfRideLoopR",  // fileName3
        0.00000000000f,   // 0x18
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x81\x69\x89\xc1\x91\xac\x81\x6a"},  // mParent
        "SurfRideDashLoop",      // fileName1
        1.00000000000f,          // 0x8
        "SurfRideDashLoopL",     // fileName2
        0.00000000000f,          // 0x10
        "SurfRideDashLoopR",     // fileName3
        0.00000000000f,          // 0x18
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x8c\x58\x82\xab\x8a\x4a\x8e\x6e"},  // mParent
        "SurfRide",              // fileName1
        1.00000000000f,          // 0x8
        "SurfRideL",             // fileName2
        0.00000000000f,          // 0x10
        "SurfRideR",             // fileName3
        0.00000000000f,          // 0x18
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x8c\x58\x82\xab\x8a\x4a\x8e\x6e\x81\x69\x89\xc1\x91\xac\x81\x6a"},  // mParent
        "SurfRideDash",                  // fileName1
        1.00000000000f,                  // 0x8
        "SurfRideDashL",                 // fileName2
        0.00000000000f,                  // 0x10
        "SurfRideDashR",                 // fileName3
        0.00000000000f,                  // 0x18
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x97\x8e\x89\xba"},  // mParent
        "SurfFall",          // fileName1
        1.00000000000f,      // 0x8
        "SurfFallL",         // fileName2
        0.00000000000f,      // 0x10
        "SurfFallR",         // fileName3
        0.00000000000f,      // 0x18
    },
    {
        {""},            // mParent
        "",              // fileName1
        0.00000000000f,  // 0x8
        nullptr,         // fileName2
        0.00000000000f,  // 0x10
        nullptr,         // fileName3
        0.00000000000f,  // 0x18
    },
};

XanimeBckTable2 doubleAnimeTable[] = {
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b"},  // mParent
        "SquatWait",       // fileName1
        1.00000000000f,    // 0x8
        "SquatWalk",       // fileName2
        0.00000000000f,    // 0x10
    },
    {
        {"\x82\xbb\x82\xcc\x8f\xea\x91\xab\x93\xa5\x82\xdd"},  // mParent
        "Run",             // fileName1
        0.750000000000f,   // 0x8
        "Wait",            // fileName2
        0.250000000000f,   // 0x10
    },
    {
        {"\x83\x5e\x83\x7d\x83\x52\x83\x8d\x82\xb5\x82\xe1\x82\xaa\x82\xdd"},  // mParent
        "BallSquat",           // fileName1
        1.00000000000f,        // 0x8
        "BallWalk",            // fileName2
        0.00000000000f,        // 0x10
    },
    {
        {""},            // mParent
        "",              // fileName1
        0.00000000000f,  // 0x8
        nullptr,         // fileName2
        0.00000000000f,  // 0x10
    },
};

XanimeBckTable1 singleAnimeTable[] = {
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "Jump",        // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        "Jump2",        // fileName
        0,              // animationHash
        0,              // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76""C"},  // mParent
        "JumpRoll",     // fileName
        0,              // animationHash
        0,              // fileHash
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "JumpTurn",          // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8b\xf3\x92\x86\x88\xea\x89\xf1\x93\x5d"},  // mParent
        "AirControl",    // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x74\x83\x8a\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "Rise",              // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x77\x83\x8a\x83\x52\x83\x76\x83\x5e\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "FlickAir",                // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x83\x5f\x83\x62\x83\x56\x83\x85\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "Rolling",             // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x94\xf2\x82\xd1\x82\xb7\x82\xb3\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "Bounce",                // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x56\x83\x87\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "GravityChange",       // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x58\x83\x4a\x83\x43\x83\x89\x83\x75\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "JumpTwin",              // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x58\x83\x50\x83\x4c\x83\x88"},  // mParent
        "Bury",        // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x83\x58\x83\x50\x83\x4c\x83\x88\x92\x45\x8f\x6f"},  // mParent
        "Bury",            // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8"},       // mParent
        "BuryStandWait",  // fileName
        0,                // animationHash
        0,                // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f"},  // mParent
        "BuryStandEnd",  // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x8b\x74\x92\x85\x92\x6e"},       // mParent
        "CannonFlyLand",  // fileName
        0,                // animationHash
        0,                // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        "HopperWaitA",          // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        "HopperWaitB",          // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x82\xd3\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        "HopperWaitA",              // fileName
        0,                          // animationHash
        0,                          // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x82\xd3\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        "HopperWaitB",              // fileName
        0,                          // animationHash
        0,                          // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x88\xda\x93\xae""A"},  // mParent
        "HopperRunA",       // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x88\xda\x93\xae""B"},  // mParent
        "HopperRunB",       // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        "HopperJumpA",              // fileName
        0,                          // animationHash
        0,                          // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        "HopperJumpB",              // fileName
        0,                          // animationHash
        0,                          // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "HopperWallJump",        // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        "HopperMarioHipDropStart",       // fileName
        0,                               // animationHash
        0,                               // fileHash
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        "HopperMarioHipDrop",        // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x82\xd3\x82\xdd""1"},  // mParent
        "Jump",             // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x82\xd3\x82\xdd""2"},  // mParent
        "JumpPress2nd",     // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x82\xd3\x82\xdd""3"},  // mParent
        "JumpPress3rd",     // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x88\xf8\x82\xab\x96\xdf\x82\xb5"},  // mParent
        "PullBack",    // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x88\xf8\x82\xab\x96\xdf\x82\xb5\x92\x85\x92\x6e"},  // mParent
        "PullBackLand",    // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x7c\x83\x62\x83\x68\x83\x8f\x81\x5b\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        "WarpPodStart",        // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x7c\x83\x62\x83\x68\x83\x8f\x81\x5b\x83\x76\x8f\x49\x97\xb9"},  // mParent
        "WarpPodEnd",          // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x95\x9d\x82\xc6\x82\xd1"},   // mParent
        "JumpBroad",  // fileName
        0,            // animationHash
        0,            // fileHash
    },
    {
        {"\x92\x85\x92\x6e"},  // mParent
        "Land",    // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x92\x85\x92\x6e""B"},    // mParent
        "Jump2Land",  // fileName
        0,            // animationHash
        0,            // fileHash
    },
    {
        {"\x92\x85\x92\x6e""C"},       // mParent
        "JumpRollLand",  // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x92\x85\x92\x6e\x83\x5e\x81\x5b\x83\x93"},  // mParent
        "JumpTurnLand",  // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x92\x85\x92\x6e\x95\x9d\x82\xc6\x82\xd1"},   // mParent
        "JumpBroadLand",  // fileName
        0,                // animationHash
        0,                // fileHash
    },
    {
        {"\x83\x6e\x81\x5b\x83\x68\x92\x85\x92\x6e"},  // mParent
        "LandStiffen",   // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x82\xb7\x82\xd7\x82\xe8\x92\x85\x92\x6e"},  // mParent
        "LandSlope",     // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x56\x83\x87\x81\x5b\x83\x67\x92\x85\x92\x6e"},  // mParent
        "Land",            // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        "HipDropStart",          // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        "HipDrop",           // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e"},  // mParent
        "HipDropLand",           // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x8a\x82\xe8"},  // mParent
        "LandRotation",          // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x8f\x87\x8a\x8a\x82\xe8"},  // mParent
        "LandRotation",      // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x8b\x74\x8a\x8a\x82\xe8"},  // mParent
        "Fall",              // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        "HipDropHomingStart",          // fileName
        0,                             // animationHash
        0,                             // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        "HipDropHoming",           // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e"},  // mParent
        "HipDropHomingLand",           // fileName
        0,                             // animationHash
        0,                             // fileHash
    },
    {
        {"\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76"},  // mParent
        "SlipUp",            // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76\x8f\x80\x94\xf5"},  // mParent
        "HangSlipUpStart",               // fileName
        0,                               // animationHash
        0,                               // fileHash
    },
    {
        {"\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76"},  // mParent
        "HangSlipUp",                // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x94\xf2\x8d\x73\x92\x86"},  // mParent
        "BeeFly",        // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x94\xf2\x8d\x73\x92\x86\x96\xb3\x93\xfc\x97\xcd"},  // mParent
        "BeeFlyWait",          // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab"},  // mParent
        "BeeLand",           // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab\x92\x86"},  // mParent
        "BeeWait",             // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x89\xd4\x88\xda\x93\xae"},      // mParent
        "BeeCreepWallWalk",  // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "BeeJump",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "BeeWallJump",       // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x99\xb3\x99\xb4\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "BeeCreepWait",        // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x99\xb3\x99\xb4\x91\x4f\x90\x69"},  // mParent
        "BeeCreepWalk",    // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x99\xb3\x99\xb4\x92\x85\x92\x6e"},  // mParent
        "BeeCreepLand",    // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        "BeeHipDropStart",           // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        "BeeHipDrop",            // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e"},  // mParent
        "BeeHipDropLand",            // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x95\xc7\x92\x85\x92\x6e"},  // mParent
        "BeeHipDropLand",              // fileName
        0,                             // animationHash
        0,                             // fileHash
    },
    {
        {"\x93\x44\x92\xe1\x91\xac\x95\xe0\x8d\x73"},  // mParent
        "WalkSoft",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x93\x44\x8d\x82\x91\xac\x95\xe0\x8d\x73"},  // mParent
        "WalkBury",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x97\x8e\x89\xba"},  // mParent
        "Fall",    // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73\x8a\x4a\x8e\x6e"},  // mParent
        "FooStart",                  // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73"},  // mParent
        "FooFly",                // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73\x8d\xc4\x8a\x4a"},  // mParent
        "FooFlyStart",               // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x90\xc3\x8e\x7e"},  // mParent
        "FooWait",               // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x89\xf0\x8f\x9c"},  // mParent
        "FooEnd",                // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x92\x85\x92\x6e"},  // mParent
        "LandSlope",             // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x83\x58\x83\x73\x83\x93"},  // mParent
        "FooSpin",                 // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "WallJump",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\xc7\x82\xb7\x82\xd7\x82\xe8"},  // mParent
        "WallSlide",   // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab"},  // mParent
        "WallKeep",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\xc7\x89\x9f\x82\xb5"},  // mParent
        "Push",      // fileName
        0,           // animationHash
        0,           // fileHash
    },
    {
        {"\x95\xc7\x8d\xb6\x95\xe0\x82\xab"},  // mParent
        "WallWalkL",   // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x95\xc7\x89\x45\x95\xe0\x82\xab"},  // mParent
        "WallWalkR",   // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x95\xc7\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "WallWait",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x91\x4f\x95\xc7\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "Push",            // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x95\xc7\x83\x71\x83\x62\x83\x67"},  // mParent
        "WallHit",     // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e"},  // mParent
        "WallHitLand",     // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x95\xc7\x82\xcd\x82\xb6\x82\xab"},  // mParent
        "WallHit",     // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x8a\x52\x82\xd3\x82\xf1\x82\xce\x82\xe8"},  // mParent
        "Stagger",       // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x8c\xc0\x8a\x45"},  // mParent
        "WaitHold",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8d\xe2\x8d\xb6\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "WaitSlopeL",      // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8d\xe2\x89\x45\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "WaitSlopeR",      // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8d\xe2\x91\x4f\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "WaitSlopeD",      // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8d\xe2\x8c\xe3\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "WaitSlopeU",      // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x89\x83\x93"},  // mParent
        "Run",     // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x95\xe0\x8d\x73"},  // mParent
        "Walk",    // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x93\xdd\x8d\x73"},    // mParent
        "WalkSoft",  // fileName
        0,           // animationHash
        0,           // fileHash
    },
    {
        {"\x83\x81\x83\x5e\x83\x8b\x83\x5f\x83\x62\x83\x56\x83\x85"},  // mParent
        "RunDash",           // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x95\xe0\x8d\x73"},  // mParent
        "WalkBury",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        "WalkBuryJumpLow",    // fileName
        0,                    // animationHash
        0,                    // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        "WalkBuryJumpLow2",   // fileName
        0,                    // animationHash
        0,                    // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "WalkBuryJumpHi",        // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea"},  // mParent
        "Press",     // fileName
        0,           // animationHash
        0,           // fileHash
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea\x95\x9c\x8b\x41"},  // mParent
        "PressRecover",  // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea\x89\xf0\x8f\x9c"},  // mParent
        "Jump",          // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x75\x83\x8c\x81\x5b\x83\x4c"},  // mParent
        "Brake",       // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x83\x75\x83\x8c\x81\x5b\x83\x4c\x8a\x8a\x82\xe8\x8f\xb0"},  // mParent
        "Run",               // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x75\x83\x8c\x81\x5b\x83\x4c"},  // mParent
        "Turn",              // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x75\x83\x8c\x81\x5b\x83\x4c\x8a\x8a\x82\xe8\x8f\xb0"},  // mParent
        "Run",                     // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c"},  // mParent
        "RunEnd",              // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x8a\xb5\x90\xab\x91\x96\x8d\x73"},  // mParent
        "Brake",           // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x8d\xb6"},  // mParent
        "SkateL",        // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x89\x45"},  // mParent
        "SkateR",        // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x90\xd8\x91\xd6\x8d\xb6"},  // mParent
        "SkateSwitchL",  // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x90\xd8\x91\xd6\x89\x45"},  // mParent
        "SkateSwitchR",  // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x8c\xe3\x8d\x73\x8d\xb6"},  // mParent
        "SkateBackL",    // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x8c\xe3\x8d\x73\x89\x45"},  // mParent
        "SkateBackR",    // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x95\x58\x8f\xe3\x83\x5e\x81\x5b\x83\x93"},  // mParent
        "SkateTurn",     // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd"},  // mParent
        "SquatWait",   // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\x4a\x8e\x6e"},  // mParent
        "SquatStart",      // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9"},  // mParent
        "SquatEnd",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x82\xa4\x82\xc2\x82\xd4\x82\xb9"},  // mParent
        "SlideStmach",               // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x8f\x49\x97\xb9"},  // mParent
        "SlideStmachEnd",        // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x8f\x49\x97\xb9"},  // mParent
        "SlideHipEnd",           // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x93\xc1\x8e\xea\x83\x45\x83\x47\x83\x43\x83\x67""1A"},  // mParent
        "Sleep",             // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x93\xc1\x8e\xea\x83\x45\x83\x47\x83\x43\x83\x67""1B"},  // mParent
        "SleepLie",          // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x90\xed\x93\xac\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "BattleWait",      // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8b\xf3\x93\x5d"},  // mParent
        "Run",     // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2"},    // mParent
        "CarryStart",  // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x8b\xf3\x92\x86"},   // mParent
        "CarryAirStart",  // fileName
        0,                // animationHash
        0,                // fileHash
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x83\x4e\x83\x43\x83\x62\x83\x4e"},  // mParent
        "CarryStartShort",   // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "CarryWait",         // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x4a\x83\x75\x94\xb2\x82\xab"},  // mParent
        "PullOut",     // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x83\x4a\x83\x75\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "PullOutWait",     // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x6e\x83\x93\x83\x7d\x81\x5b\x93\x8a\x82\xb0\x89\xf1\x93\x5d\x92\x86"},  // mParent
        "Swing",                 // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x6e\x83\x93\x83\x7d\x81\x5b\x93\x8a\x82\xb0\x83\x8a\x83\x8a\x81\x5b\x83\x58"},  // mParent
        "SwingThrow",              // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x83\x52\x83\x43\x83\x93\x83\x51\x83\x62\x83\x67"},  // mParent
        "CoinGet",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x70\x83\x93\x83\x60"},  // mParent
        "Spin2nd",   // fileName
        0,           // animationHash
        0,           // fileHash
    },
    {
        {"\x8b\xf3\x83\x70\x83\x93\x83\x60"},  // mParent
        "Spin2nd",     // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x8d\xb6\x83\x70\x83\x93\x83\x60"},  // mParent
        "Spin2nd",     // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x8d\xb6\x8b\xf3\x83\x70\x83\x93\x83\x60"},  // mParent
        "Spin2nd",       // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x89\x83\x93\x83\x6a\x83\x93\x83\x4f\x83\x4c\x83\x62\x83\x4e"},  // mParent
        "Kick",                // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x41\x83\x62\x83\x70\x81\x5b\x83\x70\x83\x93\x83\x60"},  // mParent
        "Jump",              // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x41\x83\x62\x83\x70\x81\x5b"},  // mParent
        "SquatEnd",            // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x54\x83\x7d\x81\x5b\x83\x5c\x83\x8b\x83\x67"},  // mParent
        "SpinLow",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x93\x8a\x82\xb0"},  // mParent
        "Throw",   // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x93\x8a\x82\xb0"},  // mParent
        "Throw",           // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x97\xbc\x8e\xe8\x93\x8a\x82\xb0"},  // mParent
        "ThrowBoth",   // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93"},  // mParent
        "FireSpin",          // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86"},  // mParent
        "FireSpin",              // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8a\x4a\x8e\x6e"},  // mParent
        "HangStartUnder",    // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x92\x86"},  // mParent
        "HangWait",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9"},  // mParent
        "HangUp",            // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9\x8d\xe2"},  // mParent
        "HangUp",              // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8d\x7e\x82\xe8"},  // mParent
        "HangStart",         // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "JumpBack",            // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76\x92\x85\x92\x6e"},  // mParent
        "JumpBackLand",            // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x95\xe0\x82\xab"},  // mParent
        "SquatWalk",       // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x82\xaa\x82\xf1\x82\xce\x82\xe8\x91\x96\x82\xe8"},  // mParent
        "RunSlope",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x95\xa0\x82\xce\x82\xa2\x83\x57\x83\x83\x83\x93\x83\x76"},     // mParent
        "SlideStomachRecover",  // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x90\x4b\x8a\x8a\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "SlideHipRecover",   // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8c\xfc\x82\xa9\x82\xa2\x95\x97\x91\x96\x82\xe8"},  // mParent
        "RunSlope",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x8c\xfc\x82\xa9\x82\xa2\x95\x97\x82\xd3\x82\xf1\x82\xce\x82\xe8"},  // mParent
        "Stagger",             // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x82\xbb\x82\xcc\x8f\xea\x91\xab\x93\xa5\x82\xdd\x8f\xe3\x94\xbc\x90\x67"},  // mParent
        "Walk",                  // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "SwimWait",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x88\xea\x91\x7e\x82\xab"},  // mParent
        "SwimBreast",    // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x90\x85\x8f\xe3\x88\xea\x91\x7e\x82\xab"},       // mParent
        "SwimBreastSurface",  // fileName
        0,                    // animationHash
        0,                    // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x90\xf6\x82\xe8"},  // mParent
        "SwimDive",    // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x83\x83\x93\x83\x76\x83\x5f\x83\x43\x83\x75"},  // mParent
        "LandWater",             // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x83\x83\x93\x83\x76\x83\x5f\x83\x43\x83\x75\x89\xf1\x93\x5d"},  // mParent
        "LandWaterDive",             // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x8f\xe3\x8f\xb8\x8c\xc4\x8b\x7a"},  // mParent
        "SwimRise",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x90\x85\x96\xca\x8f\x89\x8a\xfa\x88\xda\x93\xae"},  // mParent
        "SwimStartSurface",    // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x90\x85\x8f\xe3\x83\x58\x83\x73\x83\x93"},     // mParent
        "SwimSpinSurface",  // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93"},  // mParent
        "SwimSpin",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x88\xda\x93\xae"},  // mParent
        "SwimSpinAttack",    // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x90\x85\x8f\xe3\x83\x58\x83\x73\x83\x93\x88\xda\x93\xae"},       // mParent
        "SwimSpinAttackSurface",  // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "SwimJump",              // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57"},   // mParent
        "SwimDamageSmall",  // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\x54"},             // mParent
        "SwimFlutterBoardDamageSmall",  // fileName
        0,                              // animationHash
        0,                              // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86"},  // mParent
        "SwimDamageMiddle",  // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x90\x85\x8f\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86"},         // mParent
        "SwimDamageMiddleSurface",  // fileName
        0,                          // animationHash
        0,                          // fileHash
    },
    {
        {"\x90\x85\x8f\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x90\x85"},           // mParent
        "SwimDamageMiddleSurfaceLand",  // fileName
        0,                              // animationHash
        0,                              // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67"},    // mParent
        "SwimFlutterboard",  // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x8f\x49\x97\xb9"},  // mParent
        "SwimDamageMiddle",    // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x8b\x54\x93\x8a\x82\xb0"},           // mParent
        "SwimFlutterboardThrow",  // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x8a\x4a\x8e\x6e"},     // mParent
        "SwimFlutterboardStart",  // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x95\xc7\x83\x5e\x81\x5b\x83\x93"},  // mParent
        "SwimFlutterboardTurn",    // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x45\x83\x93"},  // mParent
        "SwimDie",       // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5e\x81\x5b\x83\x93\x8d\xb6"},  // mParent
        "SwimTurnL",       // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5e\x81\x5b\x83\x93\x89\x45"},  // mParent
        "SwimTurnR",       // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x95\xc7\x83\x71\x83\x62\x83\x67"},  // mParent
        "SwimWallHit",     // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x83\x5e\x81\x5b\x83\x93\x89\xba"},   // mParent
        "SwimTurnForward",  // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x90\x85\x89\x6a\x97\xa4\x82\xa4\x82\xbf\x82\xa0\x82\xb0"},  // mParent
        "SlideStmachEnd",    // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8f\x80\x94\xf5"},  // mParent
        "DiveWait",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "Dive",                // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "DiveBack",                // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x92\x85\x92\x6e"},  // mParent
        "Land",                // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x89\xf1\x93\x5d\x92\x85\x92\x6e"},  // mParent
        "LandRotation",            // fileName
        0,                         // animationHash
        0,                         // fileHash
    },
    {
        {"\x83\x8a\x83\x93\x83\x4f\x83\x5f\x83\x62\x83\x56\x83\x85"},  // mParent
        "SwimDashRing",      // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x8a\x83\x93\x83\x4f\x83\x5f\x83\x62\x83\x56\x83\x85\x8f\x80\x94\xf5"},  // mParent
        "SwimDashRingStart",     // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x4a\x83\x81\x8e\x9d\x82\xbf\x83\x8a\x83\x93\x83\x4f\x8f\x80\x94\xf5"},           // mParent
        "SwimFlutterBoardDashRingStart",  // fileName
        0,                                // animationHash
        0,                                // fileHash
    },
    {
        {"\x83\x4a\x83\x81\x8e\x9d\x82\xbf\x83\x8a\x83\x93\x83\x4f"},          // mParent
        "SwimFlutterBoardDashRing",  // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x99\xb3\x99\xb4\x91\x4f\x90\x69"},  // mParent
        "SwimGetUp",   // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x91\x4f\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        "DamageSmallFront",  // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8c\xe3\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        "DamageSmallBack",   // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8b\xad\x90\xa7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "Rise",            // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57"},       // mParent
        "DamageMiddleFront",  // fileName
        0,                    // animationHash
        0,                    // fileHash
    },
    {
        {"\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\xf3\x92\x86"},      // mParent
        "DamageMiddleFrontAir",  // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e"},       // mParent
        "DamageMiddleFrontLand",  // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x92\x86\x8c\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57"},    // mParent
        "DamageMiddleBack",  // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x92\x86\x8c\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\xf3\x92\x86"},   // mParent
        "DamageMiddleBackAir",  // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x92\x86\x8c\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e"},    // mParent
        "DamageMiddleBackLand",  // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        "DamageFire",    // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x6d\x81\x5b\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        "DamageBit",       // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x89\x8a\x82\xcc\x83\x89\x83\x93\x83\x69\x81\x5b"},  // mParent
        "FireRun",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x89\x83\x93\x91\x4f\x92\x9b"},  // mParent
        "FireRunStart",        // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        "DamageElectric",  // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9"},  // mParent
        "DamageElectricEnd",   // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x5f\x83\x93\x83\x58"},  // mParent
        "FireRun",           // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57"},   // mParent
        "DamageStart",  // fileName
        0,              // animationHash
        0,              // fileHash
    },
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e"},  // mParent
        "Land",            // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "DamageWait",          // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x6d\x81\x5b\x83\x7d\x83\x8b\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "Wait",                // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x95\x58\x8c\x8b"},  // mParent
        "Freeze",  // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x95\x58\x8c\x8b\x89\xf0\x8f\x9c"},  // mParent
        "IceFlick",    // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x82\xb5\x82\xd1\x82\xea"},    // mParent
        "DamageNumb",  // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x82\xb5\x82\xd1\x82\xea\x89\xf1\x95\x9c"},   // mParent
        "DamageNumbEnd",  // fileName
        0,                // animationHash
        0,                // fileHash
    },
    {
        {"\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea"},  // mParent
        "DamageFlick",     // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea\x8f\x49\x97\xb9"},  // mParent
        "DamageFlickEnd",      // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1"},   // mParent
        "DamageWeakFront",  // fileName
        0,                  // animationHash
        0,                  // fileHash
    },
    {
        {"\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1\x8b\xf3\x92\x86"},  // mParent
        "DamageWeakFrontAir",  // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1\x92\x85\x92\x6e"},   // mParent
        "DamageWeakFrontLand",  // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x8d\xc0\x82\xe8\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieSit",        // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x8a\xb4\x93\x64\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieElectric",   // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x89\x8a\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieFire",     // fileName
        0,             // animationHash
        0,             // fileHash
    },
    {
        {"\x8b\xc2\x8c\xfc\x82\xaf\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieOver",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x98\xeb\x82\xb9\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieUnder",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x93\xde\x97\x8e\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieFall",       // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x75\x83\x89\x83\x62\x83\x4e\x83\x7a\x81\x5b\x83\x8b\x97\x8e\x89\xba"},  // mParent
        "DieBlackHole",          // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieSit",          // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x51\x81\x5b\x83\x80\x83\x49\x81\x5b\x83\x6f\x81\x5b"},  // mParent
        "DieOver",           // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x95\x89\x82\xaf"},  // mParent
        "DieEvent",      // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x90\x85\x92\x86\x83\x8c\x81\x5b\x83\x58\x95\x89\x82\xaf"},  // mParent
        "DieSwimEvent",      // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x83\x5f\x83\x45\x83\x93"},  // mParent
        "DieBury",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x8d\xb6"},  // mParent
        "TennisShotL",         // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x89\x45"},  // mParent
        "TennisShotR",         // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x92\x86"},  // mParent
        "TennisShotM",         // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x8b\xf3"},  // mParent
        "TennisShotAir",       // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        "TennisWait",        // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x47\x83\x8c\x83\x81\x83\x93\x83\x67\x83\x51\x83\x62\x83\x67"},  // mParent
        "ElementGet",          // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x47\x83\x8c\x83\x81\x83\x93\x83\x67\x83\x51\x83\x62\x83\x67\x90\xda\x92\x6e\x92\x86"},  // mParent
        "ElementGetGround",          // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x8b\xf3\x92\x86\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        "Spin",          // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        "SpinGround",    // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        "IceSpin",         // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x88\xda\x93\xae"},  // mParent
        "IceSkateSpin",        // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x90\xc3\x8e\x7e"},  // mParent
        "IceSpin",             // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86"},  // mParent
        "BeeSpin",           // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93"},   // mParent
        "BeeSpinGround",  // fileName
        0,                // animationHash
        0,                // fileHash
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x8b\xf3\x92\x86"},  // mParent
        "IceSpinAir",          // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "IceJump",                     // fileName
        0,                             // animationHash
        0,                             // fileHash
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2"},  // mParent
        "IceJump2",             // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""3"},  // mParent
        "IceJump3",             // fileName
        0,                      // animationHash
        0,                      // fileHash
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e"},  // mParent
        "IceJumpLand",     // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x90\xc3\x8e\x7e\x92\x85\x92\x6e"},  // mParent
        "IceJumpStopLand",     // fileName
        0,                     // animationHash
        0,                     // fileHash
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "SurfJump",              // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        "SurfJumpHigh",              // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x92\x85\x92\x6e"},  // mParent
        "SurfLand",          // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x8c\xa9\x82\xe9"},  // mParent
        "Watch",   // fileName
        0,         // animationHash
        0,         // fileHash
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x8c\xa9\x82\xe9"},  // mParent
        "Watch",         // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93""A"},   // mParent
        "StageStartGround",  // fileName
        0,                   // animationHash
        0,                   // fileHash
    },
    {
        {"\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93""B"},    // mParent
        "LandScenarioStart",  // fileName
        0,                    // animationHash
        0,                    // fileHash
    },
    {
        {"\x83\x45\x83\x48\x81\x5b\x83\x4e\x83\x43\x83\x93"},  // mParent
        "GoThrough",       // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""1]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""2]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""2]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""3]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""4]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""5]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""4]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""6]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""7]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""8]"},  // mParent
        "DemoGetPower",           // fileName
        0,                        // animationHash
        0,                        // fileHash
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e"},  // mParent
        "RaceStart",     // fileName
        0,               // animationHash
        0,               // fileHash
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x83\x4e\x83\x89\x83\x45\x83\x60\x83\x93\x83\x4f\x8a\x4a\x8e\x6e"},  // mParent
        "RaceStartCrouch",           // fileName
        0,                           // animationHash
        0,                           // fileHash
    },
    {
        {"\x83\x53\x81\x5b\x83\x58\x83\x67\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e"},  // mParent
        "RaceStartGhost",        // fileName
        0,                       // animationHash
        0,                       // fileHash
    },
    {
        {"\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x9f\x97\x98"},  // mParent
        "WinGhost",        // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {"\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x6f\x8c\xbb"},  // mParent
        "AppearGhost",     // fileName
        0,                 // animationHash
        0,                 // fileHash
    },
    {
        {""},  // mParent
        "",    // fileName
        0,     // animationHash
        0,     // fileHash
    },
};

XanimeGroupInfo marioAnimeTable[] = {
    {
        {"\x8a\xee\x96\x7b"},        // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\xee\x96\x7b"},  // mParent
        1.00000000000f,    // 0x4
        0x2,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x8a\xee\x96\x7b"},    // mParent
        1.00000000000f,  // 0x4
        0x1e,            // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x8a\xee\x96\x7b"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x8d\xb6\x89\x45\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x91\x4f\x8c\xe3\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x8d\xb6\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x89\x45\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x91\x4f\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x8c\xe3\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x89\x83\x93"},        // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xe0\x8d\x73"},        // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\xdd\x8d\x73"},        // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x81\x83\x5e\x83\x8b\x83\x5f\x83\x62\x83\x56\x83\x85"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5f\x83\x62\x83\x56\x83\x85\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x94\xf2\x82\xd1\x82\xb7\x82\xb3\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x56\x83\x87\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x95\xe0\x8d\x73"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        1.00000000000f,       // 0x4
        0x1,                  // 0x8
        0.00000000000f,       // 0xC
        0.00000000000f,       // 0x10
        0.00000000000f,       // 0x14
        0,                    // 0x18
        0,                    // 0x1C
        0,                    // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        1.00000000000f,       // 0x4
        0x1,                  // 0x8
        0.00000000000f,       // 0xC
        0.00000000000f,       // 0x10
        0.00000000000f,       // 0x14
        0,                    // 0x18
        0,                    // 0x1C
        0,                    // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0x1,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\x74\x92\x85\x92\x6e"},      // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x75\x83\x8c\x81\x5b\x83\x4c"},    // mParent
        1.00000000000f,  // 0x4
        0x4,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x75\x83\x8c\x81\x5b\x83\x4c\x8a\x8a\x82\xe8\x8f\xb0"},  // mParent
        4.00000000000f,      // 0x4
        0x4,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x75\x83\x8c\x81\x5b\x83\x4c"},  // mParent
        1.00000000000f,      // 0x4
        0,                   // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x75\x83\x8c\x81\x5b\x83\x4c\x8a\x8a\x82\xe8\x8f\xb0"},  // mParent
        3.00000000000f,            // 0x4
        0x4,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xe0\x8d\x73\x90\xa7\x93\xae\x83\x75\x83\x8c\x81\x5b\x83\x4c"},  // mParent
        1.00000000000f,        // 0x4
        0x4,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x8a\xb5\x90\xab\x91\x96\x8d\x73"},  // mParent
        1.00000000000f,    // 0x4
        0x10,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x8d\xb6"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x97\xcd\x8d\x73\x89\x45"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x90\xd8\x91\xd6\x8d\xb6"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x90\xd8\x91\xd6\x89\x45"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x8c\xe3\x8d\x73\x8d\xb6"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x8c\xe3\x8d\x73\x89\x45"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8f\xe3\x83\x5e\x81\x5b\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\xc1\x8e\xea\x83\x45\x83\x47\x83\x43\x83\x67""1A"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\xc1\x8e\xea\x83\x45\x83\x47\x83\x43\x83\x67""1B"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\xed\x93\xac\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x1e,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x99\xb3\x99\xb4\x91\x4f\x90\x69"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76"},    // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76""B"},   // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76""C"},   // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x83\x8a\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x82\xd3\x82\xdd""1"},  // mParent
        1.00000000000f,     // 0x4
        0x6,                // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x82\xd3\x82\xdd""2"},  // mParent
        1.00000000000f,     // 0x4
        0x6,                // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x82\xd3\x82\xdd""3"},  // mParent
        1.00000000000f,     // 0x4
        0x6,                // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x88\xf8\x82\xab\x96\xdf\x82\xb5"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x88\xf8\x82\xab\x96\xdf\x82\xb5\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7c\x83\x62\x83\x68\x83\x8f\x81\x5b\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7c\x83\x62\x83\x68\x83\x8f\x81\x5b\x83\x76\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x4a\x83\x43\x83\x89\x83\x75\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0,                       // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\xf3\x92\x86\x88\xea\x89\xf1\x93\x5d"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x83\x4c\x83\x88"},    // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x83\x4c\x83\x88\x92\x45\x8f\x6f"},  // mParent
        1.00000000000f,    // 0x4
        0x1,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8"},      // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x92\x45\x8f\x6f"},  // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        1.00000000000f,         // 0x4
        0x6,                    // 0x8
        0.00000000000f,         // 0xC
        0.00000000000f,         // 0x10
        0.00000000000f,         // 0x14
        0,                      // 0x18
        0,                      // 0x1C
        0,                      // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        1.00000000000f,         // 0x4
        0x6,                    // 0x8
        0.00000000000f,         // 0xC
        0.00000000000f,         // 0x10
        0.00000000000f,         // 0x14
        0,                      // 0x18
        0,                      // 0x1C
        0,                      // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x82\xd3\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        1.00000000000f,             // 0x4
        0x6,                        // 0x8
        0.00000000000f,             // 0xC
        0.00000000000f,             // 0x10
        0.00000000000f,             // 0x14
        0,                          // 0x18
        0,                          // 0x1C
        0,                          // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x82\xd3\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        1.00000000000f,             // 0x4
        0x6,                        // 0x8
        0.00000000000f,             // 0xC
        0.00000000000f,             // 0x10
        0.00000000000f,             // 0x14
        0,                          // 0x18
        0,                          // 0x1C
        0,                          // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x88\xda\x93\xae""A"},  // mParent
        1.00000000000f,     // 0x4
        0x6,                // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x88\xda\x93\xae""B"},  // mParent
        1.00000000000f,     // 0x4
        0x6,                // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        1.00000000000f,             // 0x4
        0x1,                        // 0x8
        0.00000000000f,             // 0xC
        0.00000000000f,             // 0x10
        0.00000000000f,             // 0x14
        0,                          // 0x18
        0,                          // 0x1C
        0,                          // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        1.00000000000f,             // 0x4
        0x1,                        // 0x8
        0.00000000000f,             // 0xC
        0.00000000000f,             // 0x10
        0.00000000000f,             // 0x14
        0,                          // 0x18
        0,                          // 0x1C
        0,                          // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,                  // 0x4
        0x6,                             // 0x8
        0.00000000000f,                  // 0xC
        0.00000000000f,                  // 0x10
        0.00000000000f,                  // 0x14
        0,                               // 0x18
        0,                               // 0x1C
        0,                               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x9d\x82\xc6\x82\xd1"},      // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,          // 0x4
        0x2,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0,                   // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x8a\x82\xe8"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x8f\x87\x8a\x8a\x82\xe8"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76\x8b\x74\x8a\x8a\x82\xe8"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76\x8f\x80\x94\xf5"},  // mParent
        1.00000000000f,                  // 0x4
        0x4,                             // 0x8
        0.00000000000f,                  // 0xC
        0.00000000000f,                  // 0x10
        0.00000000000f,                  // 0x14
        0,                               // 0x18
        0,                               // 0x1C
        0,                               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x83\x58\x83\x8a\x83\x62\x83\x76\x83\x41\x83\x62\x83\x76"},  // mParent
        1.00000000000f,              // 0x4
        0x2,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,                // 0x4
        0x2,                           // 0x8
        0.00000000000f,                // 0xC
        0.00000000000f,                // 0x10
        0.00000000000f,                // 0x14
        0,                             // 0x18
        0,                             // 0x1C
        0,                             // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        1.00000000000f,            // 0x4
        0,                         // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,                // 0x4
        0x6,                           // 0x8
        0.00000000000f,                // 0xC
        0.00000000000f,                // 0x10
        0.00000000000f,                // 0x14
        0,                             // 0x18
        0,                             // 0x1C
        0,                             // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x97\x8e\x89\xba"},        // mParent
        1.00000000000f,  // 0x4
        0xf,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x85\x92\x6e"},        // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x85\x92\x6e""B"},       // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x81\x5b\x83\x68\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb7\x82\xd7\x82\xe8\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x56\x83\x87\x81\x5b\x83\x67\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x94\xf2\x8d\x73\x92\x86"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x94\xf2\x8d\x73\x92\x86\x96\xb3\x93\xfc\x97\xcd"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab\x92\x86"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x99\xb3\x99\xb4\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x99\xb3\x99\xb4\x91\x4f\x90\x69"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x99\xb3\x99\xb4\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x89\xd4\x88\xda\x93\xae"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,              // 0x4
        0x2,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0,                       // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x95\xc7\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,                // 0x4
        0x6,                           // 0x8
        0.00000000000f,                // 0xC
        0.00000000000f,                // 0x10
        0.00000000000f,                // 0x14
        0,                             // 0x18
        0,                             // 0x1C
        0,                             // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x94\xf2\x8d\x73\x8d\xc4\x8a\x4a"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x90\xc3\x8e\x7e"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x89\xf0\x8f\x9c"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x83\x58\x83\x73\x83\x93"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\x44\x92\xe1\x91\xac\x95\xe0\x8d\x73"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\x44\x8d\x82\x91\xac\x95\xe0\x8d\x73"},  // mParent
        2.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x82\xad\x82\xc1\x82\xc2\x82\xab"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x82\xb7\x82\xd7\x82\xe8"},    // mParent
        1.00000000000f,  // 0x4
        0xa,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x89\x9f\x82\xb5"},      // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x8d\xb6\x95\xe0\x82\xab"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x89\x45\x95\xe0\x82\xab"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x91\x4f\x95\xc7\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x83\x71\x83\x62\x83\x67"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xc7\x82\xcd\x82\xb6\x82\xab"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6d\x81\x5b\x83\x7d\x83\x8b\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8c\x8b"},        // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\x58\x8c\x8b\x89\xf0\x8f\x9c"},    // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xd1\x82\xea"},      // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xd1\x82\xea\x89\xf1\x95\x9c"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x52\x83\x43\x83\x93\x83\x51\x83\x62\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x1,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x70\x83\x93\x83\x60"},      // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\xf3\x83\x70\x83\x93\x83\x60"},    // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xb6\x83\x70\x83\x93\x83\x60"},    // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xb6\x8b\xf3\x83\x70\x83\x93\x83\x60"},  // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x89\x83\x93\x83\x6a\x83\x93\x83\x4f\x83\x4c\x83\x62\x83\x4e"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x41\x83\x62\x83\x70\x81\x5b\x83\x70\x83\x93\x83\x60"},  // mParent
        1.00000000000f,      // 0x4
        0x3,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x41\x83\x62\x83\x70\x81\x5b"},  // mParent
        1.00000000000f,        // 0x4
        0x3,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x83\x7d\x81\x5b\x83\x5c\x83\x8b\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x51\x81\x5b\x83\x80\x83\x49\x81\x5b\x83\x6f\x81\x5b"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea"},      // mParent
        0.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea\x95\x9c\x8b\x41"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea\x89\xf0\x8f\x9c"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\xf3\x93\x5d"},        // mParent
        4.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x82\xa4\x82\xc2\x82\xd4\x82\xb9"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x82\xa0\x82\xa8\x82\xde\x82\xaf"},  // mParent
        2.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xfc\x82\xa9\x82\xa2\x95\x97\x91\x96\x82\xe8"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xfc\x82\xa9\x82\xa2\x95\x97\x82\xd3\x82\xf1\x82\xce\x82\xe8"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0xf,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x81\x69\x89\xc1\x91\xac\x81\x6a"},  // mParent
        1.00000000000f,          // 0x4
        0xf,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x8c\x58\x82\xab\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,          // 0x4
        0x4,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x8c\x58\x82\xab\x8a\x4a\x8e\x6e\x81\x69\x89\xc1\x91\xac\x81\x6a"},  // mParent
        1.00000000000f,                  // 0x4
        0x4,                             // 0x8
        0.00000000000f,                  // 0xC
        0.00000000000f,                  // 0x10
        0.00000000000f,                  // 0x14
        0,                               // 0x18
        0,                               // 0x1C
        0,                               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0x1,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x83\x6e\x83\x43\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x97\x8e\x89\xba"},  // mParent
        1.00000000000f,      // 0x4
        0xf,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x54\x81\x5b\x83\x74\x83\x42\x83\x93\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,      // 0x4
        0x1,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5e\x83\x7d\x83\x52\x83\x8d\x88\xda\x93\xae"},  // mParent
        1.00000000000f,    // 0x4
        0xf,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x5e\x83\x7d\x83\x52\x83\x8d\x82\xb5\x82\xe1\x82\xaa\x82\xdd"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x89\x83\x43\x83\x5f\x81\x5b\x90\x4b"},  // mParent
        2.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xbb\x82\xcc\x8f\xea\x91\xab\x93\xa5\x82\xdd"},  // mParent
        1.00000000000f,    // 0x4
        0x2,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xbb\x82\xcc\x8f\xea\x91\xab\x93\xa5\x82\xdd\x8f\xe3\x94\xbc\x90\x67"},  // mParent
        1.00000000000f,          // 0x4
        0x4,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x88\xea\x91\x7e\x82\xab"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x8f\xe3\x88\xea\x91\x7e\x82\xab"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x90\xf6\x82\xe8"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x83\x83\x93\x83\x76\x83\x5f\x83\x43\x83\x75"},  // mParent
        1.00000000000f,          // 0x4
        0x1,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x83\x83\x93\x83\x76\x83\x5f\x83\x43\x83\x75\x89\xf1\x93\x5d"},  // mParent
        1.00000000000f,              // 0x4
        0x1,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x8f\xe3\x8f\xb8\x8c\xc4\x8b\x7a"},  // mParent
        1.00000000000f,    // 0x4
        0x10,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x90\x85\x96\xca\x8f\x89\x8a\xfa\x88\xda\x93\xae"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x8f\xe3\x83\x58\x83\x73\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x1,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x88\xda\x93\xae"},  // mParent
        1.00000000000f,      // 0x4
        0x1,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x8f\xe3\x83\x58\x83\x73\x83\x93\x88\xda\x93\xae"},  // mParent
        1.00000000000f,      // 0x4
        0x1,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\x54"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x8f\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x86"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x8f\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x90\x85"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5e\x81\x5b\x83\x93\x8d\xb6"},  // mParent
        1.00000000000f,    // 0x4
        0x14,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5e\x81\x5b\x83\x93\x89\x45"},  // mParent
        1.00000000000f,    // 0x4
        0x14,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x5e\x81\x5b\x83\x93\x89\xba"},  // mParent
        1.00000000000f,    // 0x4
        0x14,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x8b\x54\x93\x8a\x82\xb0"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x95\xc7\x83\x71\x83\x62\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x57\x83\x46\x83\x62\x83\x67\x95\xc7\x83\x5e\x81\x5b\x83\x93"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x97\xa4\x82\xa4\x82\xbf\x82\xa0\x82\xb0"},  // mParent
        1.00000000000f,      // 0x4
        0x10,                // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8f\x80\x94\xf5"},  // mParent
        1.00000000000f,    // 0x4
        0x14,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xe3\x95\xfb\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,        // 0x4
        0xf,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x94\xf2\x82\xd1\x8d\x9e\x82\xdd\x8e\xb8\x94\x73\x89\xf1\x93\x5d\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x89\x6a\x83\x58\x83\x73\x83\x93\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x8a\x83\x93\x83\x4f\x83\x5f\x83\x62\x83\x56\x83\x85"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x8a\x83\x93\x83\x4f\x83\x5f\x83\x62\x83\x56\x83\x85\x8f\x80\x94\xf5"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x4a\x83\x81\x8e\x9d\x82\xbf\x83\x8a\x83\x93\x83\x4f\x8f\x80\x94\xf5"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x4a\x83\x81\x8e\x9d\x82\xbf\x83\x8a\x83\x93\x83\x4f"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x85\x92\x6e""C"},       // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x85\x92\x6e\x83\x5e\x81\x5b\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x85\x92\x6e\x95\x9d\x82\xc6\x82\xd1"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,          // 0x4
        0x4,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,          // 0x4
        0x4,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2"},      // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,  // 0x4
        0x3,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x83\x4e\x83\x43\x83\x62\x83\x4e"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,      // 0x4
        0,                   // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x4a\x83\x75\x94\xb2\x82\xab"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x4a\x83\x75\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x93\x83\x7d\x81\x5b\x93\x8a\x82\xb0\x89\xf1\x93\x5d\x92\x86"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x93\x83\x7d\x81\x5b\x93\x8a\x82\xb0\x83\x8a\x83\x8a\x81\x5b\x83\x58"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\x8a\x82\xb0"},        // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x93\x8a\x82\xb0"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x97\xbc\x8e\xe8\x93\x8a\x82\xb0"},    // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93"},  // mParent
        1.00000000000f,      // 0x4
        0,                   // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,          // 0x4
        0,                       // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,      // 0x4
        0x5,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x92\x86"},  // mParent
        1.00000000000f,    // 0x4
        0x5,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,      // 0x4
        0x4,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9\x8d\xe2"},  // mParent
        1.00000000000f,        // 0x4
        0x4,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8d\x7e\x82\xe8"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\x52\x82\xd3\x82\xf1\x82\xce\x82\xe8"},  // mParent
        1.00000000000f,  // 0x4
        0x2,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x95\xe0\x82\xab"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xaa\x82\xf1\x82\xce\x82\xe8\x91\x96\x82\xe8"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x95\xa0\x82\xce\x82\xa2\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x4b\x8a\x8a\x82\xe8\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x91\x4f\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xe3\x95\xfb\x8f\xac\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\xad\x90\xa7\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x77\x83\x8a\x83\x52\x83\x76\x83\x5e\x81\x5b\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,            // 0x4
        0x6,                       // 0x8
        0.00000000000f,            // 0xC
        0.00000000000f,            // 0x10
        0.00000000000f,            // 0x14
        0,                         // 0x18
        0,                         // 0x1C
        0,                         // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x86\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x86\x8c\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x86\x8c\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x86\x8c\xe3\x83\x5f\x83\x81\x81\x5b\x83\x57\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6d\x81\x5b\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x89\x8a\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x89\x8a\x82\xcc\x83\x89\x83\x93\x83\x69\x81\x5b"},  // mParent
        8.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x89\x83\x93\x91\x4f\x92\x9b"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57"},  // mParent
        1.00000000000f,    // 0x4
        0x1,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\x64\x8b\x43\x83\x5f\x83\x81\x81\x5b\x83\x57\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x5f\x83\x93\x83\x58"},  // mParent
        6.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1"},  // mParent
        1.00000000000f,    // 0x4
        0x2,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xe3\x93\x5d\x82\xd3\x82\xc1\x82\xc6\x82\xd1\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,        // 0x4
        0x2,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8d\xc0\x82\xe8\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8a\xb4\x93\x64\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x89\x8a\x83\x5f\x83\x45\x83\x93"},    // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\xc2\x8c\xfc\x82\xaf\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x98\xeb\x82\xb9\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x93\xde\x97\x8e\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0x14,            // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x75\x83\x89\x83\x62\x83\x4e\x83\x7a\x81\x5b\x83\x8b\x97\x8e\x89\xba"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x95\x89\x82\xaf"},  // mParent
        1.00000000000f,  // 0x4
        0xf,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x90\x85\x92\x86\x83\x8c\x81\x5b\x83\x58\x95\x89\x82\xaf"},  // mParent
        1.00000000000f,      // 0x4
        0xf,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x96\x84\x82\xdc\x82\xe8\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,    // 0x4
        0x1e,              // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xc2\x82\xd4\x82\xea\x83\x5f\x83\x45\x83\x93"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x82\xcd\x82\xcb\x82\xc6\x82\xce\x82\xb3\x82\xea\x8f\x49\x97\xb9"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x8d\xb6"},  // mParent
        1.25000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x89\x45"},  // mParent
        1.25000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x92\x86"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x8b\xf3"},  // mParent
        1.25000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        1.00000000000f,      // 0x4
        0x6,                 // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x47\x83\x8c\x83\x81\x83\x93\x83\x67\x83\x51\x83\x62\x83\x67"},  // mParent
        1.00000000000f,        // 0x4
        0x6,                   // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x47\x83\x8c\x83\x81\x83\x93\x83\x67\x83\x51\x83\x62\x83\x67\x90\xda\x92\x6e\x92\x86"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8b\xf3\x92\x86\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x92\x6e\x8f\xe3\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93"},  // mParent
        1.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x6e\x83\x60\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,      // 0x4
        0,                   // 0x8
        0.00000000000f,      // 0xC
        0.00000000000f,      // 0x10
        0.00000000000f,      // 0x14
        0,                   // 0x18
        0,                   // 0x1C
        0,                   // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x88\xda\x93\xae"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x90\xc3\x8e\x7e"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8\x8b\xf3\x92\x86"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x41\x83\x4e\x83\x5a\x83\x8b\x83\x57\x83\x83\x83\x93\x83\x76"},  // mParent
        1.00000000000f,                // 0x4
        0,                             // 0x8
        0.00000000000f,                // 0xC
        0.00000000000f,                // 0x10
        0.00000000000f,                // 0x14
        0,                             // 0x18
        0,                             // 0x1C
        0,                             // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""2"},  // mParent
        1.00000000000f,         // 0x4
        0,                      // 0x8
        0.00000000000f,         // 0xC
        0.00000000000f,         // 0x10
        0.00000000000f,         // 0x14
        0,                      // 0x18
        0,                      // 0x1C
        0,                      // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x83\x57\x83\x83\x83\x93\x83\x76""3"},  // mParent
        1.00000000000f,         // 0x4
        0,                      // 0x8
        0.00000000000f,         // 0xC
        0.00000000000f,         // 0x10
        0.00000000000f,         // 0x14
        0,                      // 0x18
        0,                      // 0x1C
        0,                      // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x50\x81\x5b\x83\x67\x90\xc3\x8e\x7e\x92\x85\x92\x6e"},  // mParent
        1.00000000000f,        // 0x4
        0,                     // 0x8
        0.00000000000f,        // 0xC
        0.00000000000f,        // 0x10
        0.00000000000f,        // 0x14
        0,                     // 0x18
        0,                     // 0x1C
        0,                     // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x8c\xa9\x82\xe9"},        // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x8c\xa9\x82\xe9"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x4a\x83\x8a\x83\x4a\x83\x8a\x8c\xc0\x8a\x45"},  // mParent
        1.00000000000f,    // 0x4
        0x6,               // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93""A"},  // mParent
        1.00000000000f,     // 0x4
        0,                  // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x65\x81\x5b\x83\x57\x83\x43\x83\x93""B"},  // mParent
        1.00000000000f,     // 0x4
        0,                  // 0x8
        0.00000000000f,     // 0xC
        0.00000000000f,     // 0x10
        0.00000000000f,     // 0x14
        0,                  // 0x18
        0,                  // 0x1C
        0,                  // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x45\x83\x48\x81\x5b\x83\x4e\x83\x43\x83\x93"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""1]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""1]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""2]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""2]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""3]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""3]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""4]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""5]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x89\xef\x98\x62""4]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""6]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""7]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x51\x83\x62\x83\x67[\x83\x66\x83\x82""8]"},  // mParent
        1.00000000000f,           // 0x4
        0,                        // 0x8
        0.00000000000f,           // 0xC
        0.00000000000f,           // 0x10
        0.00000000000f,           // 0x14
        0,                        // 0x18
        0,                        // 0x1C
        0,                        // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,  // 0x4
        0x6,             // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x8c\x81\x5b\x83\x58\x83\x4e\x83\x89\x83\x45\x83\x60\x83\x93\x83\x4f\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,              // 0x4
        0x6,                         // 0x8
        0.00000000000f,              // 0xC
        0.00000000000f,              // 0x10
        0.00000000000f,              // 0x14
        0,                           // 0x18
        0,                           // 0x1C
        0,                           // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x53\x81\x5b\x83\x58\x83\x67\x83\x8c\x81\x5b\x83\x58\x8a\x4a\x8e\x6e"},  // mParent
        1.00000000000f,          // 0x4
        0x6,                     // 0x8
        0.00000000000f,          // 0xC
        0.00000000000f,          // 0x10
        0.00000000000f,          // 0x14
        0,                       // 0x18
        0,                       // 0x1C
        0,                       // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x9f\x97\x98"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {"\x83\x53\x81\x5b\x83\x58\x83\x67\x8f\x6f\x8c\xbb"},  // mParent
        1.00000000000f,    // 0x4
        0,                 // 0x8
        0.00000000000f,    // 0xC
        0.00000000000f,    // 0x10
        0.00000000000f,    // 0x14
        0,                 // 0x18
        0,                 // 0x1C
        0,                 // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
    {
        {""},            // mParent
        0.00000000000f,  // 0x4
        0,               // 0x8
        0.00000000000f,  // 0xC
        0.00000000000f,  // 0x10
        0.00000000000f,  // 0x14
        0,               // 0x18
        0,               // 0x1C
        0,               // 0x1D
        {
            0,
            0,
            0,
            0,
        },  // 0x20
        {
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
            0.00000000000f,
        },        // 0x30
        0,        // 0x40
        0,        // 0x44
        nullptr,  // 0x48
    },
};

XanimeAuxInfo marioAnimeAuxTable[] = {{""}};

XanimeOfsInfo marioAnimeOfsTable[] = {
    {
        {"\x83\x5f\x83\x81\x81\x5b\x83\x57"},    // mParent
        0.00000000000f,  // 0x4
        10.0000000000f,  // 0x8
        10.0000000000f,  // 0xC
        0x1,             // 0x10
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x89\xba\x8c\xfc\x82\xab\x82\xa0\x82\xa8\x82\xde\x82\xaf"},  // mParent
        0.00000000000f,              // 0x4
        2.00000000000f,              // 0x8
        2.00000000000f,              // 0xC
        0x1,                         // 0x10
    },
    {
        {"\x8d\xe2\x82\xb7\x82\xd7\x82\xe8\x8f\xe3\x8c\xfc\x82\xab\x82\xa4\x82\xc2\x82\xd4\x82\xb9"},  // mParent
        0.00000000000f,              // 0x4
        2.00000000000f,              // 0x8
        2.00000000000f,              // 0xC
        0x1,                         // 0x10
    },
    {
        {"\x83\x5e\x81\x5b\x83\x93\x83\x75\x83\x8c\x81\x5b\x83\x4c"},  // mParent
        0.00000000000f,      // 0x4
        20.0000000000f,      // 0x8
        20.0000000000f,      // 0xC
        0x1,                 // 0x10
    },
    {
        {"\x83\x57\x83\x83\x83\x93\x83\x76"},    // mParent
        0.00000000000f,  // 0x4
        24.0000000000f,  // 0x8
        24.0000000000f,  // 0xC
        0x1,             // 0x10
    },
    {
        {"\x95\xc7\x83\x71\x83\x62\x83\x67"},    // mParent
        0.00000000000f,  // 0x4
        25.0000000000f,  // 0x8
        25.0000000000f,  // 0xC
        0x1,             // 0x10
    },
    {
        {"\x95\xc7\x83\x71\x83\x62\x83\x67\x92\x85\x92\x6e"},  // mParent
        0.00000000000f,    // 0x4
        67.0000000000f,    // 0x8
        0.00000000000f,    // 0xC
        0,                 // 0x10
    },
    {
        {"\x83\x4a\x83\x75\x94\xb2\x82\xab"},    // mParent
        0.00000000000f,  // 0x4
        49.0000000000f,  // 0x8
        49.0000000000f,  // 0xC
        0x1,             // 0x10
    },
    {
        {"\x97\x8e\x89\xba"},        // mParent
        0.00000000000f,  // 0x4
        29.0000000000f,  // 0x8
        29.0000000000f,  // 0xC
        0x1,             // 0x10
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x8a\x4a\x8e\x6e"},  // mParent
        0.00000000000f,          // 0x4
        13.0000000000f,          // 0x8
        13.0000000000f,          // 0xC
        0x1,                     // 0x10
    },
    {
        {"\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        0.00000000000f,      // 0x4
        9.00000000000f,      // 0x8
        9.00000000000f,      // 0xC
        0x1,                 // 0x10
    },
    {
        {"\x83\x58\x83\x73\x83\x93\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76"},  // mParent
        0.00000000000f,            // 0x4
        19.0000000000f,            // 0x8
        19.0000000000f,            // 0xC
        0x1,                       // 0x10
    },
    {
        {"\x83\x6e\x83\x60\x83\x71\x83\x62\x83\x76\x83\x68\x83\x8d\x83\x62\x83\x76\x95\xc7\x92\x85\x92\x6e"},  // mParent
        0.00000000000f,                // 0x4
        20.0000000000f,                // 0x8
        20.0000000000f,                // 0xC
        0,                             // 0x10
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x82\xd3\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76""A"},  // mParent
        10.0000000000f,             // 0x4
        50.0000000000f,             // 0x8
        0.00000000000f,             // 0xC
        0x1,                        // 0x10
    },
    {
        {"\x83\x7a\x83\x62\x83\x70\x81\x5b\x82\xd3\x82\xdd\x83\x57\x83\x83\x83\x93\x83\x76""B"},  // mParent
        10.0000000000f,             // 0x4
        50.0000000000f,             // 0x8
        0.00000000000f,             // 0xC
        0x1,                        // 0x10
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x8d\xb6"},  // mParent
        4.00000000000f,        // 0x4
        26.0000000000f,        // 0x8
        26.0000000000f,        // 0xC
        0,                     // 0x10
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x89\x45"},  // mParent
        4.00000000000f,        // 0x4
        26.0000000000f,        // 0x8
        26.0000000000f,        // 0xC
        0,                     // 0x10
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x92\x86"},  // mParent
        3.00000000000f,        // 0x4
        38.0000000000f,        // 0x8
        38.0000000000f,        // 0xC
        0,                     // 0x10
    },
    {
        {"\x83\x65\x83\x6a\x83\x58\x83\x56\x83\x87\x83\x62\x83\x67\x8b\xf3"},  // mParent
        5.00000000000f,        // 0x4
        26.0000000000f,        // 0x8
        26.0000000000f,        // 0xC
        0,                     // 0x10
    },
    {
        {"\x82\xbb\x82\xcc\x8f\xea\x91\xab\x93\xa5\x82\xdd"},  // mParent
        0.00000000000f,    // 0x4
        17.0000000000f,    // 0x8
        8.00000000000f,    // 0xC
        0,                 // 0x10
    },
    {
        {"\x83\x75\x83\x8c\x81\x5b\x83\x4c"},    // mParent
        0.00000000000f,  // 0x4
        2.00000000000f,  // 0x8
        0.00000000000f,  // 0xC
        0x1,             // 0x10
    },
    {
        {"\x83\x56\x83\x87\x81\x5b\x83\x67\x92\x85\x92\x6e"},  // mParent
        0.00000000000f,    // 0x4
        5.00000000000f,    // 0x8
        0.00000000000f,    // 0xC
        0,                 // 0x10
    },
    {
        {"\x83\x41\x83\x62\x83\x70\x81\x5b\x83\x70\x83\x93\x83\x60"},  // mParent
        22.0000000000f,      // 0x4
        23.0000000000f,      // 0x8
        0.00000000000f,      // 0xC
        0x1,                 // 0x10
    },
    {
        {"\x82\xb5\x82\xe1\x82\xaa\x82\xdd\x83\x41\x83\x62\x83\x70\x81\x5b"},  // mParent
        0.00000000000f,        // 0x4
        6.00000000000f,        // 0x8
        0.00000000000f,        // 0xC
        0,                     // 0x10
    },
    {
        {"\x83\x58\x83\x50\x83\x4c\x83\x88"},    // mParent
        0.00000000000f,  // 0x4
        59.0000000000f,  // 0x8
        30.0000000000f,  // 0xC
        0x2,             // 0x10
    },
    {
        {"\x83\x58\x83\x50\x83\x4c\x83\x88\x92\x45\x8f\x6f"},  // mParent
        60.0000000000f,    // 0x4
        120.000000000f,    // 0x8
        0.00000000000f,    // 0xC
        0,                 // 0x10
    },
    {
        {"\x93\xc1\x8e\xea\x83\x45\x83\x47\x83\x43\x83\x67""1A"},  // mParent
        0.00000000000f,      // 0x4
        494.000000000f,      // 0x8
        375.000000000f,      // 0xC
        0x2,                 // 0x10
    },
    {
        {"\x93\xc1\x8e\xea\x83\x45\x83\x47\x83\x43\x83\x67""1B"},  // mParent
        0.00000000000f,      // 0x4
        254.000000000f,      // 0x8
        135.000000000f,      // 0xC
        0x2,                 // 0x10
    },
    {
        {"\x83\x41\x83\x43\x83\x58\x82\xd0\x82\xcb\x82\xe8"},  // mParent
        0.00000000000f,    // 0x4
        35.0000000000f,    // 0x8
        35.0000000000f,    // 0xC
        0,                 // 0x10
    },
    {
        {"\x83\x74\x83\x40\x83\x43\x83\x41\x83\x58\x83\x73\x83\x93\x8b\xf3\x92\x86"},  // mParent
        0.00000000000f,          // 0x4
        30.0000000000f,          // 0x8
        30.0000000000f,          // 0xC
        0,                       // 0x10
    },
    {
        {"\x8a\x52\x82\xc2\x82\xa9\x82\xdc\x82\xe8\x8f\x49\x97\xb9\x8d\xe2"},  // mParent
        0.00000000000f,        // 0x4
        27.0000000000f,        // 0x8
        27.0000000000f,        // 0xC
        0,                     // 0x10
    },
    {
        {"\x91\x4f\x95\xc7\x83\x45\x83\x47\x83\x43\x83\x67"},  // mParent
        57.0000000000f,    // 0x4
        59.0000000000f,    // 0x8
        57.0000000000f,    // 0xC
        0x1,               // 0x10
    },
    {
        {"\x83\x74\x81\x5b\x83\x74\x83\x40\x83\x43\x83\x5e\x81\x5b\x90\xc3\x8e\x7e"},  // mParent
        0.00000000000f,          // 0x4
        159.000000000f,          // 0x8
        40.0000000000f,          // 0xC
        0x2,                     // 0x10
    },
    {
        {"\x82\xd0\x82\xeb\x82\xa2\x83\x4e\x83\x43\x83\x62\x83\x4e"},  // mParent
        0.00000000000f,      // 0x4
        26.0000000000f,      // 0x8
        0.00000000000f,      // 0xC
        0x1,                 // 0x10
    },
    {
        {""},            // mParent
        0.00000000000f,  // 0x4
        0.00000000000f,  // 0x8
        0.00000000000f,  // 0xC
        0,               // 0x10
    },
};

XanimeSwapTable luigiAnimeSwapTable[] = {
    {"Run", "LuigiRun"},
    {"Jump", "LuigiJump"},
    {"JumpRoll", "LuigiJumpRoll"},
    {"JumpBack", "LuigiJumpBack"},
    {"RunEnd", "LuigiRunEnd"},
    {"Spin", "LuigiSpin"},
    {"SpinGround", "LuigiSpinGround"},
    {"SpaceFlyShort", "LuigiSpaceFlyShort"},
    {"Wait", "LuigiWait"},
    {"WaitSlopeL", "LuigiWaitSlopeL"},
    {"WaitSlopeR", "LuigiWaitSlopeR"},
    {"WaitSlopeU", "LuigiWaitSlopeU"},
    {"WaitSlopeD", "LuigiWaitSlopeD"},
    {"", nullptr},
};
