// Galaxy 3.0 multiplayer: when the in-play panels (party strip, edge heads, watch badge, star row, bubble prompt) need composing again
// (optfix item 3; pure, host tested in tests/mp/compose_key_test.cpp; used by MpUiSystem.cpp).
//
// The compose functions turn plain models into a Scene and a Plan every frame. When the models are the same as in the frame before and
// nothing else (menu, results card, toast, chat ring) shares the strip scene's frame, the result is the same: the last one is kept and
// only MpPanelDraw::update / draw run. The key is a hash of the fields the compose functions read (field by field: no padding bytes),
// the screen, and the party strip's bob phase only while a head uses it (a head in a bubble floats, a lost link blinks).
#pragma once

#include <stdint.h>

#include "Game/MP/MpPanelPlan.hpp"

namespace MpComposeKey {
    class Hasher {
    public:
        void u32(uint32_t v) {
            for (int i = 0; i < 4; i++) {
                mH ^= (v >> (i * 8)) & 0xFFu;
                mH *= 1099511628211ull;
            }
        }
        void f32(float f) {
            union { float f; uint32_t u; } c;
            c.f = f == 0.0f ? 0.0f : f;   // (-0 and +0 compose the same)
            u32(c.u);
        }
        uint64_t value() const { return mH; }

    private:
        uint64_t mH = 14695981039346656037ull;
    };

    // True when the strip's composition reads `bob`: a member floats (a bubble) or blinks (a lost link).
    static inline bool stripUsesBob(const MpPanel::Strip& s) {
        for (int i = 0; i < s.count && i < MpPanel::kMaxStrip; i++)
            if (s.m[i].state == MpPanel::PARTY_BUBBLE || s.m[i].link == MpPanel::LINK_LOST) return true;
        return false;
    }

    struct Inputs {
        bool stripOn, edgeOn, badgeOn, starRowOn, promptOn;
        const MpPanel::Strip* strip;
        const MpPanel::Edge* edge;
        const MpPanel::Badge* badge;
        const MpPanel::StarRow* starRow;
        const MpPanel::Prompt* prompt;
        MpPanel::Screen screen;
        uint32_t frame;   // the frame counter: bob = (frame % 180) / 180
    };

    static inline uint64_t key(const Inputs& in) {
        Hasher h;
        h.u32((in.stripOn ? 1u : 0u) | (in.edgeOn ? 2u : 0u) | (in.badgeOn ? 4u : 0u) | (in.starRowOn ? 8u : 0u) | (in.promptOn ? 16u : 0u));
        h.f32(in.screen.width);
        h.f32(in.screen.height);
        if (in.stripOn) {
            const MpPanel::Strip& s = *in.strip;
            h.u32((uint32_t)s.count);
            for (int i = 0; i < s.count && i < MpPanel::kMaxStrip; i++) {
                const MpPanel::StripMember& m = s.m[i];
                h.u32((uint32_t)m.look | (uint32_t)m.state << 8 | (uint32_t)m.link << 16 | (m.away ? 1u << 24 : 0u));
                h.f32(m.flash);
                h.f32(m.enter);
                h.f32(m.pos);
            }
            h.f32(s.appear);
            h.f32(s.counterX);
            h.f32(s.counterY);
            h.f32(s.counterScale);
            h.u32((uint32_t)s.localLook);
            h.u32(stripUsesBob(s) ? 1u + in.frame % 180u : 0u);
        }
        if (in.edgeOn) {
            const MpPanel::Edge& e = *in.edge;
            h.u32((uint32_t)e.count);
            for (int i = 0; i < e.count && i < MpPanel::kMaxEdges; i++) {
                h.u32(e.e[i].look);
                h.f32(e.e[i].x);
                h.f32(e.e[i].y);
                h.f32(e.e[i].ax);
                h.f32(e.e[i].ay);
                h.f32(e.e[i].alpha);
            }
        }
        if (in.badgeOn) {
            h.u32((uint32_t)in.badge->look | (in.badge->canSwitch ? 256u : 0u));
            h.f32(in.badge->alpha);
        }
        if (in.starRowOn) {
            const MpPanel::StarRow& r = *in.starRow;
            h.u32((uint32_t)r.count);
            for (int i = 0; i < r.count && i < MpPanel::kStarRowMax; i++) h.u32(r.look[i]);
            h.u32((uint32_t)(int32_t)r.by);
            h.f32(r.alpha);
        }
        if (in.promptOn) {
            h.f32(in.prompt->x);
            h.f32(in.prompt->y);
            h.f32(in.prompt->alpha);
        }
        return h.value();
    }
}  // namespace MpComposeKey
