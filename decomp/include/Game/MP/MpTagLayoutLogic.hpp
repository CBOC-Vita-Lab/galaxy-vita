// Galaxy 3.0 multiplayer: name tag overlap resolution, pure logic (owner: tags; no game headers, host tested in
// tests/mp/tag_layout_test.cpp). The game side is MpNameTag.cpp.
//
// Remote players standing together project their name tags onto the same few pixels. Once per picture, after every wanted tag has
// been projected, resolve() nudges overlapping plates apart VERTICALLY (a stack), deterministically, with no allocation:
//   - tags are visited nearest first (squared camera distance); the nearest of a group keeps its own place and every other tag is
//     pushed clear of the ones already placed (above or below them: the side is chosen when the overlap begins and kept while it
//     lasts, so a tag never flips sides under a small camera motion; the push is the minimal one, so it starts at zero);
//   - a tag that was pushed last picture needs a small gap more to be released (hysteresis), and the nearest is chosen with a bias
//     for the one that was not pushed (no swapping of roles when two puppets are equally far);
//   - the pushed offset follows its target through an exponential smoothing (kSmooth) and snaps when closer than kSnap, so a tag
//     glides instead of jumping;
//   - a pushed tag is kept inside [minY, maxY]; a push that would leave goes to the other side.
// Up to kMax tags: O(n^2) with n <= 4 is a handful of compares. Edge-marker tags (nameTags 2) take part like any other.
#pragma once
#include <cmath>

namespace MpTagLayout {
    static constexpr int kMax = 4;
    static constexpr float kSmooth = 0.35f;   // fraction of the remaining distance covered per picture
    static constexpr float kSnap = 0.5f;      // px
    static constexpr float kHoldGap = 4.0f;   // extra px a pushed tag needs before it is released
    static constexpr float kNearBias = 0.85f; // an unpushed tag's squared distance is scaled by this when ranking

    struct Item {
        bool active;   // wanted this picture (a fading-out or dead plate does not take part)
        float x, y;    // projected screen position
        float w;       // estimated plate width
        float d2;      // squared distance camera -> head
    };

    struct State {
        float off[kMax];     // smoothed vertical offset applied to the plate
        signed char dir[kMax];   // -1 pushed up, +1 pushed down, 0 free
        bool pushed[kMax];   // target offset != 0 last picture
        bool seen[kMax];     // was active last picture
        void reset() {
            for (int i = 0; i < kMax; i++) { off[i] = 0.0f; dir[i] = 0; pushed[i] = false; seen[i] = false; }
        }
    };

    static inline bool finite(float v) { return v == v && v > -1.0e30f && v < 1.0e30f; }

    // h: plate height (incl. a gap), minY/maxY: allowed range of a pushed tag's y. Writes the final y of every active item to outY
    // (inactive: the item's own y plus its frozen offset). n <= kMax (extra items are left untouched).
    static inline void resolve(State& st, const Item* items, int n, float h, float minY, float maxY, float* outY) {
        if (n > kMax) n = kMax;
        if (n < 0) n = 0;
        float target[kMax];
        int order[kMax];
        int cnt = 0;
        for (int i = 0; i < n; i++) {
            target[i] = 0.0f;
            if (!items[i].active || !finite(items[i].x) || !finite(items[i].y)) {
                st.dir[i] = 0; st.pushed[i] = false;
                continue;
            }
            // insertion sort by biased distance (stable by slot)
            const float ki = items[i].d2 * (st.pushed[i] ? 1.0f : kNearBias);
            int p = cnt;
            while (p > 0) {
                const int o = order[p - 1];
                const float ko = items[o].d2 * (st.pushed[o] ? 1.0f : kNearBias);
                if (ko <= ki) break;
                order[p] = o; p--;
            }
            order[p] = i;
            cnt++;
        }
        float placedY[kMax];   // final (target) y of the tags already placed, in `order`
        for (int a = 0; a < cnt; a++) {
            const int i = order[a];
            const float baseY = items[i].y;
            float y = baseY;
            int dir = st.dir[i];
            const float gap = h + (st.pushed[i] ? kHoldGap : 0.0f);
            for (int iter = 0; iter <= kMax; iter++) {
                bool moved = false;
                for (int b = 0; b < a; b++) {
                    const int j = order[b];
                    const float dx = items[i].x - items[j].x;
                    const float hw = 0.5f * (items[i].w + items[j].w);
                    const float dy = y - placedY[b];
                    if (std::fabs(dx) >= hw || std::fabs(dy) >= gap) continue;
                    if (dir == 0) dir = (dy < 0.0f || (dy == 0.0f)) ? -1 : 1;   // the side it is already on (tie: up)
                    float ny = placedY[b] + (float)dir * h;
                    if (ny < minY || ny > maxY) {
                        dir = -dir;
                        ny = placedY[b] + (float)dir * h;
                    }
                    y = ny;
                    moved = true;
                }
                if (!moved) break;
            }
            if (y == baseY) dir = 0;
            placedY[a] = y;
            target[i] = y - baseY;
            st.dir[i] = (signed char)dir;
            st.pushed[i] = (y != baseY);
        }
        for (int i = 0; i < n; i++) {
            if (items[i].active && finite(items[i].y)) {
                if (!st.seen[i]) st.off[i] = target[i];   // newly wanted: no glide from a stale place
                else {
                    const float d = target[i] - st.off[i];
                    st.off[i] = (std::fabs(d) < kSnap) ? target[i] : st.off[i] + d * kSmooth;
                }
            }
            st.seen[i] = items[i].active;
            outY[i] = items[i].y + st.off[i];
        }
    }
}  // namespace MpTagLayout
