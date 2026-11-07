#pragma once

// (1.3 port) What a J3D state display list writes, for the game-side list cuts of the draw pass (GXV_J3D_MATMERGE,
// GXV_J3D_DIFFSKIP: Game/System/DrawBuffer.cpp; GXV_J3D_DIFFKEEP: J3DPacket.cpp). Header-only and free of the game's types:
// tests/vgx_j3d_lists.cpp drives the real front end with the same decisions.
//
// A list is "pure" when it holds only NOPs and register writes whose only effect is the value they leave:
//   - BP writes, except the registers that act on every write (the front end's kBpEffect, vgx_state.cpp: the draw-done / token
//     / copy triggers, the palette load, the port's marker) and, to be safe, the palette address, the texture-cache invalidate and
//     the copy-filter trigger (0x64, 0x66, 0x67); a mask (0xFE) must be used by a BP write of the same list;
//   - CP writes (flagged: the shapes' own registers, array bases and vertex formats, are CP registers);
//   - XF writes of at most 16 words (the front end's own limit), classified by what else writes there in the draw pass.
// Anything else (a draw, a call, an indexed load, an unknown opcode, a cut command) makes the list impure: no cut is made.
// Writing a pure list again, onto registers that already hold what it writes, leaves every register as it was: the front end's
// WriteBP / WriteXF / WriteCP store (and mark dirty) only what differs, apart from the kBpEffect registers excluded above.

#include <stdint.h>
#include <string.h>

enum {
    J3DPortDL_Pure = 0x01,       // only NOPs and plain register writes (see above)
    J3DPortDL_CP = 0x02,         // a CP register
    J3DPortDL_PreDrawXF = 0x04,  // XF 0x1008 (vertex spec) or 0x1018 / 0x1019 (matrix index): J3DShape::loadPreDrawSetting's registers
    J3DPortDL_PosNrmMem = 0x08,  // XF position matrix rows 0..0x77 or normal matrix memory 0x400..0x45F: J3DShapeMtx's loads
    J3DPortDL_TexMem = 0x10,     // XF texture matrix rows 0x78..0x3FF or post-transform matrices 0x460..0x5FF
    J3DPortDL_LightMem = 0x20,   // XF light memory 0x600..0xFFF
    J3DPortDL_Scanned = 0x80000000u  // (a cached result: the scan was made)
};

inline uint32_t J3DPortXFClass(uint32_t addr) {
    if (addr >= 0x1000) {
        return (addr == 0x1008 || addr == 0x1018 || addr == 0x1019) ? (uint32_t)J3DPortDL_PreDrawXF : 0u;
    }
    if (addr < 0x78 || (addr >= 0x400 && addr < 0x460)) {
        return J3DPortDL_PosNrmMem;
    }
    if (addr < 0x600) {
        return J3DPortDL_TexMem;
    }
    return J3DPortDL_LightMem;
}

// The flags above for the list's bytes (0 when it is not pure), always with J3DPortDL_Scanned.
inline uint32_t J3DPortScanDL(const void* pDL, uint32_t size) {
    const uint8_t* p = static_cast< const uint8_t* >(pDL);
    const uint8_t* const end = p + size;
    uint32_t flags = 0;
    bool maskPending = false;
    if (p == nullptr) {
        return J3DPortDL_Scanned;
    }
    while (p < end) {
        const uint8_t cmd = *p;
        if (cmd == 0x00) {
            ++p;
            continue;
        }
        if (cmd == 0x61) {
            if (end - p < 5) {
                return J3DPortDL_Scanned;
            }
            const uint8_t reg = p[1];
            p += 5;
            if (reg == 0xFE) {
                maskPending = true;
                continue;
            }
            if (reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52 || reg == 0x64 || reg == 0x65 || reg == 0x66 || reg == 0x67 ||
                reg == 0xFF) {
                return J3DPortDL_Scanned;
            }
            maskPending = false;
            continue;
        }
        if (maskPending) {
            return J3DPortDL_Scanned;  // (a mask left for something that is not a BP write)
        }
        if (cmd == 0x08) {
            if (end - p < 6) {
                return J3DPortDL_Scanned;
            }
            flags |= J3DPortDL_CP;
            p += 6;
            continue;
        }
        if (cmd == 0x10) {
            if (end - p < 5) {
                return J3DPortDL_Scanned;
            }
            const uint32_t count = (static_cast< uint32_t >(p[1]) << 8) | p[2];
            const uint32_t addr = (static_cast< uint32_t >(p[3]) << 8) | p[4];
            if (count > 0xF || static_cast< uint32_t >(end - p) < 5u + 4u * (count + 1u)) {
                return J3DPortDL_Scanned;
            }
            for (uint32_t a = addr; a <= addr + count; ++a) {
                flags |= J3DPortXFClass(a);
            }
            p += 5u + 4u * (count + 1u);
            continue;
        }
        return J3DPortDL_Scanned;  // a draw, a call, an indexed load or an unknown opcode
    }
    if (maskPending) {
        return J3DPortDL_Scanned;  // (a mask at the end applies to the caller's next write)
    }
    return flags | J3DPortDL_Pure | J3DPortDL_Scanned;
}

// GXV_J3D_MATMERGE: the first packet's differed list may be sent at the end of the material's list, ahead of
// J3DShape::loadPreDrawSetting (vertex format list, array bases, matrix index: CP registers and XF 0x1008 / 0x1018 / 0x1019),
// when it writes none of what that writes. The material list must be pure too: the merged list is then a pure state list for the
// front end, as its two parts were (a list with an effect register is taken another way there).
inline bool J3DPortMergeable(uint32_t matFlags, uint32_t diffFlags) {
    return (matFlags & J3DPortDL_Pure) && (diffFlags & J3DPortDL_Pure) && !(diffFlags & (J3DPortDL_CP | J3DPortDL_PreDrawXF));
}

// GXV_J3D_DIFFSKIP: a differed list with the bytes of the one sent last, with nothing sent between but shapes (J3DShape::drawFast:
// CP registers, XF 0x1008 / 0x1018 / 0x1019, position / normal matrices, primitives; texture matrices only for a packet with a
// differed texture matrix, user data or a normal matrix in texture memory, after which the caller forgets the last list) and no light
// load (the caller forgets it there too), leaves every register as it is: it is not sent.
inline bool J3DPortDiffSkippable(uint32_t flags) {
    return (flags & J3DPortDL_Pure) && !(flags & (J3DPortDL_CP | J3DPortDL_PreDrawXF | J3DPortDL_PosNrmMem));
}

// GXV_J3D_MATMERGE's list: the material list, then the differed list, made one list (into pOut, room cap; returns its size, a
// 32-byte multiple, or 0 when it is not made). The material list's writes that the differed list writes again whole are left out
// (no draw is between them: they are dead), so that the merged list writes each register with one value, as each of its two parts
// does: the front end compiles such a list into register deltas (GXV_SLDELTA), and replays one that writes a register with two
// values op by op, which costs more than the call it saves. In detail:
//   - a BP write (with the mask write before it, if any) is left out when the differed list writes the register (TEV colour halves:
//     the address and its type bit) with no mask;
//   - an XF register block (0x1000..0x10FF) keeps the words the differed list does not write, as single-word writes when some are
//     left out (the front end writes a register block word by word: the same);
//   - an XF memory block (matrices, lights) is left out only when the differed list writes all of its words;
//   - everything else (CP writes, NOPs) as it is; both lists must be pure (J3DPortMergeable).
// Made only when the result writes each BP register (and TEV half) with one value and one mask, each XF register with one value
// and each XF memory word once. Not made otherwise (0): the two lists are then sent as before.
inline uint32_t J3DPortBuildMerged(const uint8_t* pMat, uint32_t matSize, const uint8_t* pDiff, uint32_t diffSize, uint8_t* pOut, uint32_t cap) {
    // (the game thread's only: one at a time)
    static uint32_t bpFull[(256 + 16 + 31) / 32];    // differed list: BP keys written with no mask (TEV halves: 256 + slot)
    static uint32_t xfRegW[256 / 32];          // differed list: XF registers 0x1000..0x10FF
    static uint32_t xfMemW[0x1000 / 32];       // differed list: XF memory words
    static uint32_t bpSeen[(256 + 16 + 31) / 32], bpVal[256 + 16], bpMask[256 + 16];
    static uint32_t xfRegSeen[256 / 32], xfRegVal[256];
    static uint32_t xfMemSeen[0x1000 / 32];
    memset(bpFull, 0, sizeof(bpFull));
    memset(xfRegW, 0, sizeof(xfRegW));
    memset(xfMemW, 0, sizeof(xfMemW));
    memset(bpSeen, 0, sizeof(bpSeen));
    memset(xfRegSeen, 0, sizeof(xfRegSeen));
    memset(xfMemSeen, 0, sizeof(xfMemSeen));
#define J3DPORT_BIT(a, i) ((a)[(i) >> 5] & (1u << ((i)&31)))
#define J3DPORT_SET(a, i) ((a)[(i) >> 5] |= (1u << ((i)&31)))
    struct Local {
        static uint32_t be32(const uint8_t* p) { return (static_cast< uint32_t >(p[0]) << 24) | (static_cast< uint32_t >(p[1]) << 16) | (static_cast< uint32_t >(p[2]) << 8) | p[3]; }
        static uint32_t bpKey(uint32_t rv) {
            const uint32_t reg = rv >> 24;
            return (reg >= 0xE0 && reg <= 0xE7) ? 256u + (((reg - 0xE0) << 1) | ((rv >> 23) & 1u)) : reg;
        }
    };
    // 1. what the differed list writes whole
    {
        const uint8_t* p = pDiff;
        const uint8_t* const end = pDiff + diffSize;
        bool masked = false;
        while (p < end) {
            if (*p == 0x00) { ++p; continue; }
            if (*p == 0x61) {
                const uint32_t rv = Local::be32(p + 1);
                p += 5;
                if ((rv >> 24) == 0xFE) { masked = true; continue; }
                if (!masked) J3DPORT_SET(bpFull, Local::bpKey(rv));
                masked = false;
                continue;
            }
            if (*p == 0x10) {
                const uint32_t n = ((static_cast< uint32_t >(p[1]) << 8) | p[2]) + 1u, addr = (static_cast< uint32_t >(p[3]) << 8) | p[4];
                for (uint32_t a = addr; a < addr + n; ++a) {
                    if (a >= 0x1000 && a < 0x1100) J3DPORT_SET(xfRegW, a - 0x1000);
                    else if (a < 0x1000) J3DPORT_SET(xfMemW, a);
                }
                p += 5 + 4 * n;
                continue;
            }
            if (*p == 0x08) { p += 6; continue; }
            return 0;  // (J3DPortScanDL accepted it: not reached)
        }
    }
    // 2. the material list without what that makes dead, then the differed list
    uint32_t o = 0;
    {
        const uint8_t* p = pMat;
        const uint8_t* const end = pMat + matSize;
        while (p < end) {
            const uint8_t cmd = *p;
            if (cmd == 0x00) { ++p; continue; }
            uint32_t len = 0;
            bool keep = true;
            if (cmd == 0x61) {
                const uint32_t rv = Local::be32(p + 1);
                if ((rv >> 24) == 0xFE) {
                    if (end - p < 10 || p[5] != 0x61) return 0;  // (only a mask right before its write)
                    len = 10;
                    keep = !J3DPORT_BIT(bpFull, Local::bpKey(Local::be32(p + 6)));
                } else {
                    len = 5;
                    keep = !J3DPORT_BIT(bpFull, Local::bpKey(rv));
                }
            } else if (cmd == 0x08) {
                len = 6;
            } else if (cmd == 0x10) {
                const uint32_t n = ((static_cast< uint32_t >(p[1]) << 8) | p[2]) + 1u, addr = (static_cast< uint32_t >(p[3]) << 8) | p[4];
                len = 5 + 4 * n;
                uint32_t dead = 0;
                for (uint32_t a = addr; a < addr + n; ++a) {
                    if ((a >= 0x1000 && a < 0x1100) ? J3DPORT_BIT(xfRegW, a - 0x1000) : (a < 0x1000 && J3DPORT_BIT(xfMemW, a))) ++dead;
                }
                if (dead == n) {
                    keep = false;
                } else if (dead != 0 && addr >= 0x1000) {
                    for (uint32_t i = 0; i < n; ++i) {
                        if (J3DPORT_BIT(xfRegW, addr + i - 0x1000)) continue;
                        if (o + 9 > cap) return 0;
                        pOut[o + 0] = 0x10;
                        pOut[o + 1] = 0;
                        pOut[o + 2] = 0;
                        pOut[o + 3] = static_cast< uint8_t >((addr + i) >> 8);
                        pOut[o + 4] = static_cast< uint8_t >(addr + i);
                        memcpy(pOut + o + 5, p + 5 + 4 * i, 4);
                        o += 9;
                    }
                    keep = false;
                }
            } else {
                return 0;
            }
            if (keep) {
                if (o + len > cap) return 0;
                memcpy(pOut + o, p, len);
                o += len;
            }
            p += len;
        }
    }
    if (o + diffSize > cap) return 0;
    memcpy(pOut + o, pDiff, diffSize);
    o += diffSize;
    while (o & 0x1F) {
        if (o >= cap) return 0;
        pOut[o++] = 0;
    }
    // 3. one value per register
    {
        const uint8_t* p = pOut;
        const uint8_t* const end = pOut + o;
        uint32_t mask = 0x00FFFFFFu;
        while (p < end) {
            if (*p == 0x00) { ++p; continue; }
            if (*p == 0x61) {
                const uint32_t rv = Local::be32(p + 1);
                p += 5;
                if ((rv >> 24) == 0xFE) { mask = rv & 0x00FFFFFFu; continue; }
                const uint32_t k = Local::bpKey(rv), v = rv & mask;
                if (J3DPORT_BIT(bpSeen, k)) {
                    if (bpMask[k] != mask || bpVal[k] != (v & 0x00FFFFFFu)) return 0;
                } else {
                    J3DPORT_SET(bpSeen, k);
                    bpMask[k] = mask;
                    bpVal[k] = v & 0x00FFFFFFu;
                }
                mask = 0x00FFFFFFu;
                continue;
            }
            if (*p == 0x10) {
                const uint32_t n = ((static_cast< uint32_t >(p[1]) << 8) | p[2]) + 1u, addr = (static_cast< uint32_t >(p[3]) << 8) | p[4];
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t a = addr + i, w = Local::be32(p + 5 + 4 * i);
                    if (a >= 0x1000 && a < 0x1100) {
                        if (J3DPORT_BIT(xfRegSeen, a - 0x1000)) {
                            if (xfRegVal[a - 0x1000] != w) return 0;
                        } else {
                            J3DPORT_SET(xfRegSeen, a - 0x1000);
                            xfRegVal[a - 0x1000] = w;
                        }
                    } else if (a < 0x1000) {
                        if (J3DPORT_BIT(xfMemSeen, a)) return 0;
                        J3DPORT_SET(xfMemSeen, a);
                    } else {
                        return 0;
                    }
                }
                p += 5 + 4 * n;
                continue;
            }
            if (*p == 0x08) { p += 6; continue; }
            return 0;
        }
    }
#undef J3DPORT_BIT
#undef J3DPORT_SET
    return o;
}
