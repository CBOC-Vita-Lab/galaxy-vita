// Scripted controller input for tests (PETARI_INPUT), shared by the headless
// launcher and the VR app: comma-separated "<t>[-<t1>]:<item>|<item>..."
// entries (seconds since boot; a single time holds for 0.15 s).  Items: A B 1
// 2 PLUS MINUS HOME Z C UP DOWN LEFT RIGHT, SPIN (remote shake), SX=<f>
// SY=<f> (stick), PX=<f> PY=<f> (pointer), PITCH=<deg> ROLL=<deg> (the right
// controller's tilt from level: pitched up, then rolled right; negative:
// down, left).
// Example: PETARI_INPUT="22:A,26:A,30-33:SY=1|A"
#pragma once

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/input.h"

typedef struct PortInputEvent {
    int t0, t1;  // ms
    uint32_t buttons;
    int spin, hasStick, hasPointer, hasTilt;
    float sx, sy, px, py, pitch, roll;
} PortInputEvent;

static const struct {
    const char* name;
    uint32_t bit;
} kPortScriptButtons[] = {{"LEFT", 0x0001}, {"RIGHT", 0x0002}, {"DOWN", 0x0004}, {"UP", 0x0008},   {"PLUS", 0x0010},
                          {"2", 0x0100},    {"1", 0x0200},     {"B", 0x0400},    {"A", 0x0800},    {"MINUS", 0x1000},
                          {"Z", 0x2000},    {"C", 0x4000},     {"HOME", 0x8000}};

static inline int portParseInputScript(const char* spec, PortInputEvent* out, int max) {
    int n = 0;
    static char buf[16384];
    snprintf(buf, sizeof(buf), "%s", spec);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok && n < max; tok = strtok_r(NULL, ",", &save)) {
        char* colon = strchr(tok, ':');
        if (!colon) {
            fprintf(stderr, "input script: bad entry '%s'\n", tok);
            continue;
        }
        *colon = 0;
        PortInputEvent e;
        memset(&e, 0, sizeof(e));
        char* dash = strchr(tok, '-');
        e.t0 = (int)(atof(tok) * 1000.0);
        e.t1 = dash ? (int)(atof(dash + 1) * 1000.0) : e.t0 + 150;
        char* s2 = NULL;
        for (char* item = strtok_r(colon + 1, "|", &s2); item; item = strtok_r(NULL, "|", &s2)) {
            if (!strncmp(item, "SX=", 3)) {
                e.hasStick = 1, e.sx = (float)atof(item + 3);
            } else if (!strncmp(item, "SY=", 3)) {
                e.hasStick = 1, e.sy = (float)atof(item + 3);
            } else if (!strncmp(item, "PX=", 3)) {
                e.hasPointer = 1, e.px = (float)atof(item + 3);
            } else if (!strncmp(item, "PY=", 3)) {
                e.hasPointer = 1, e.py = (float)atof(item + 3);
            } else if (!strncmp(item, "PITCH=", 6)) {
                e.hasTilt = 1, e.pitch = (float)atof(item + 6);
            } else if (!strncmp(item, "ROLL=", 5)) {
                e.hasTilt = 1, e.roll = (float)atof(item + 5);
            } else if (!strcmp(item, "SPIN")) {
                e.spin = 1;
            } else {
                int found = 0;
                for (size_t i = 0; i < sizeof(kPortScriptButtons) / sizeof(kPortScriptButtons[0]); i++) {
                    if (!strcmp(item, kPortScriptButtons[i].name)) {
                        e.buttons |= kPortScriptButtons[i].bit;
                        found = 1;
                    }
                }
                if (!found) fprintf(stderr, "input script: unknown item '%s'\n", item);
            }
        }
        out[n++] = e;
    }
    return n;
}

// Applies the entries active at `ms` since boot to `pad`; returns nonzero if
// any was.
static inline int portApplyInputScript(const PortInputEvent* ev, int count, int ms, PortPadState* pad) {
    int active = 0;
    for (int i = 0; i < count; i++) {
        const PortInputEvent* e = &ev[i];
        if (ms < e->t0 || ms >= e->t1) continue;
        active = 1;
        pad->buttons |= e->buttons;
        if (e->hasStick) pad->stickX = e->sx, pad->stickY = e->sy;
        if (e->hasPointer) pad->pointerValid = 1, pad->pointerX = e->px, pad->pointerY = e->py;
        if (e->hasTilt) {
            // Down in the controller's axes (x right, y up, z where it aims).
            float p = e->pitch * 3.14159265f / 180.0f, r = e->roll * 3.14159265f / 180.0f;
            pad->tilted = 1;
            pad->downX = cosf(p) * sinf(r);
            pad->downY = -cosf(p) * cosf(r);
            pad->downZ = -sinf(p);
        }
        if (e->spin) {
            // Alternating spikes, like the XR layer's shake.
            float s = ((ms - e->t0) / 16) & 1 ? 2.5f : -2.5f;
            pad->accX = s;
            pad->accY = s;
        }
    }
    return active;
}
