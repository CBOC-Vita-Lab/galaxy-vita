#!/usr/bin/env python3
"""Offline pictures of the multiplayer menus (decomp/src/Game/MP/MpMenuSequence.cpp + MpPanelPlan.cpp) at the console's 960 x 544,
with the game's own layouts (PauseMenu, RetryButton, SysInfoWindow(Mini), IconMario from LayoutData/*.arc) and fonts (Font.arc).

    render.py <cooked data root> <output dir> [screen ...]
        e.g. render.py /data/cooked /home/claude/g3/wt/ui/previews

preview.cpp (built here with the host's clang++) runs the real state machine against a fake port to reach each screen and prints
its layout uses and GX plan; layout.py draws the layouts (static pose + the animations' end frames: Appear, ButtonAppear,
ButtonWait, ButtonSelectIn for the selected button) and the plan. Output is local only: it contains the game's art and glyphs.
The backdrop is a plain space gradient standing in for the game scene behind the menus.
"""
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, HERE)
import layout as L  # noqa: E402


def handheld_glyphs():
    text = open(os.path.join(ROOT, "platform", "src", "port", "handheld_glyphs.inc")).read()
    out = {}
    for m in re.finditer(r"kGlyph(\w+)\[(\d+)\] = \{([^}]*)\}", text):
        vals = np.array([int(x) for x in m.group(3).replace("\n", "").split(",") if x.strip()], np.float32) / 255.0
        out[m.group(1)] = vals.reshape(35, 34, 4)
    return {1: out["Cross"], 2: out["Circle"], 3: out["Triangle"], 6: out["Square"], 7: out["R"], 24: out["Select"], 25: out["Start"], 26: out["L"]}


def backdrop(cv, seed=3):
    yy = np.linspace(0, 1, L.SH)[:, None, None]
    cv.px[:] = np.array([0.05, 0.08, 0.22]) * (1 - yy) + np.array([0.16, 0.10, 0.30]) * yy
    rng = np.random.default_rng(seed)
    for _ in range(140):
        x, y, s = rng.integers(0, L.SW), rng.integers(0, L.SH), rng.random()
        cv.px[y, x] = 0.6 + 0.4 * s
    # a planet, as a stand-in for the game scene
    Y, X = np.mgrid[0:L.SH, 0:L.SW]
    d = np.hypot(X - 760, Y - 470)
    a = np.clip(220 - d, 0, 1)[..., None]
    shade = np.clip(1 - (Y - 300) / 300, 0.3, 1)[..., None]
    cv.px[:] = cv.px * (1 - a) + a * np.array([0.25, 0.55, 0.30]) * shade


def to_text(codes):
    return "".join(chr(c) for c in codes)


def toad_stages(m):
    """Yellow Toad's TEV stages, mirroring MpPanelDraw.cpp SetToadStages (arguments: GX_CC_* / GX_CA_* numbers, GX_TEV_REG*, GX_TEV_KCSEL_1_2 = 4):
    mask = clamp((1/2 - G) * 4) -> REG2; PREV = swapped (R, R, B, A); PREV = mix(original, PREV, mask); the tint lerp; x raster colour."""
    CPREV, C0, C1, C2, TEXC, RASC, KONST, ZERO = 0, 2, 4, 6, 8, 10, 14, 15
    APREV, A0, A1, TEXA, RASA, AZERO = 0, 1, 2, 4, 5, 7
    ADD, SUB, S1, S4 = 0, 1, 0, 2

    def st(texmap, sw, ci, co, ai):   # ci: (a, b, c, d); co: (op, scale, outreg, ksel); ai: (a, b, c, d) alpha
        texcoord, chan = (0, 0xFF) if texmap == 0 else (0xFF, 4)   # (GX_TEXCOORD0 / NULL, GX_COLOR_NULL / GX_COLOR0A0; the rasterised colour is only read by RASC / RASA)
        return bytes([texcoord, chan, texmap, (sw << 3), ci[0] | (ci[1] << 4), ci[2] | (ci[3] << 4), co[0] | (co[1] << 6), 1 | (co[2] << 1) | (co[3] << 3),
                      ai[0] | (ai[1] << 4), ai[2] | (ai[3] << 4), 0, 1 | (0 << 1) | (8 << 3), 0, 0, 0, 0])
    m.stages = [
        st(0, 2, (TEXC, ZERO, ZERO, KONST), (SUB, S4, 3, 4), (AZERO, AZERO, AZERO, AZERO)),
        st(0, 1, (ZERO, ZERO, ZERO, TEXC), (ADD, S1, 0, 8), (AZERO, AZERO, AZERO, TEXA)),
        st(0, 0, (TEXC, CPREV, C2, ZERO), (ADD, S1, 0, 8), (AZERO, AZERO, AZERO, TEXA)),
        st(0xFF, 0, (C0, C1, CPREV, ZERO), (ADD, S1, 0, 8), (A0, A1, APREV, AZERO)),
        st(0xFF, 0, (ZERO, CPREV, RASC, ZERO), (ADD, S1, 0, 8), (AZERO, APREV, RASA, AZERO)),
    ]
    m.swaps = [(0, 1, 2, 3), (0, 0, 2, 3), (1, 1, 1, 3), (0, 1, 2, 3)]


class Kit:
    def __init__(self, data, lang):
        d = os.path.join(data, "files", "LayoutData")
        self.arcs = {}
        for n in ("PauseMenu", "RetryButton", "SysInfoWindowMini", "SysInfoWindow", "MiiIcon", "PlayerLeft", "DPDPointer", "MiiSelect", "AllStarList"):
            self.arcs[n] = L.rarc_files(open(os.path.join(d, n + ".arc"), "rb").read())
        self.fonts = L.load_fonts(L.rarc_files(open(os.path.join(d, "Font.arc"), "rb").read()), handheld_glyphs())
        self.lang = lang
        self.data = data
        self.wide = True   # (16:9; False: a 4:3 picture, no location adjust)

    def lay(self, n):
        l = L.Layout(self.arcs[n], n, self.fonts)
        L.localize(l, self.lang)
        return l

    def frame(self, cv, title, line, alpha, state=1, tk=1.0, lk=1.0):
        l = self.lay("PauseMenu")
        L.pose(l, "Appear")
        l.hide("Stars", "Counter", "BackButton", "LetterButton", "PositionNull")
        if not state & 2: l.hide("PicStagebase1")
        if state & 8: l.hide("PicStagebase")   # (kFrameNoTitle)
        l.panes["PicStagebase"].ty += 30.0 + (28.0 if state & 4 else 0.0)   # (MpPanel::kBandLift, kBandShrink)
        st, sl = l.panes["ShaStageTitle"], l.panes["ScenarioTitle"]   # (MpPanelDraw MpFrameLayout::use)
        st.ty -= 12.0 + (40.0 if state & 4 else 0.0)                 # (kTitleDrop, kTitleAlone)
        tk = tk if 0 < tk < 1 else 1.0; lk = lk if 0 < lk < 1 else 1.0
        st.scx = st.scy = tk
        sl.scx = sl.scy = lk
        sl.ty += 10.0 - 52.0 * (1.0 - lk)                             # (kLineLift)
        l.panes["PicStagebase1"].ty -= 54.0  # (MpPanel::kFootDrop)
        l.set_text("ShaStageTitle", title)
        l.set_text("ScenarioTitle", line)
        l.draw(cv, alpha=alpha, wide=self.wide)

    def button(self, cv, x, y, text, state, alpha, scale=1.0):
        l = self.lay("RetryButton")
        l.hide("BG", "NBackDown")
        L.pose(l, "ButtonAppear", under="NBackUp")
        L.pose(l, "ButtonWait", 0, under="NBackUp")
        if state == 1: L.pose(l, "ButtonSelectIn", under="NBackUp")
        l.set_text("Text1", text)
        l.draw(cv, x, y, alpha * (0.45 if state == 2 else 1.0), wide=self.wide, scale=scale or 1.0)

    def pointer(self, cv, x, y, scale, turn, alpha):
        """the Star Pointer's hand (DPDPointer HandPointer, its point pose), the fingertip at (x, y), turned (MpPanelDraw MpPointerLayout)"""
        l = self.lay("DPDPointer")
        L.pose(l, "Wait", 0)
        L.pose(l, "HandType", 0)
        l.hide("StarPointer", "HandArrow", "PicNozzle", "PicPlayer2")
        l.root.rz = turn
        l.draw(cv, x, y, alpha, wide=self.wide, scale=scale or 1.0)

    def arrows(self, cv, x, y, w, scale, alpha):
        """the Mii select's blue arrows, tips at x - w and x + w (MpPanelDraw MpArrowsLayout)"""
        l = self.lay("MiiSelect")
        l.only(["Arrow"])
        a = l.panes["Arrow"]; a.tx = a.ty = 0.0
        for n, sx in (("Left", -1), ("Right", 1)):
            p = l.panes[n]; p.tx, p.ty = sx * w, 0.0; p.scx = p.scy = scale
        l.draw(cv, x, y, alpha, wide=self.wide)

    def scroll(self, cv, x, y, h, state, text, alpha):
        """the star list's page number and round up / down buttons (MpPanelDraw MpScrollLayout)"""
        l = self.lay("AllStarList")
        l.only(["NTitle"])
        l.hide("Title", "PicButtonbase", "Capture")
        for n in ("NTitle", "Arrow"): l.panes[n].alpha = 255
        ax = l.panes["Arrow"].tx
        up, dn, pg = l.panes["ArrowUpButton"], l.panes["ArrowDownButton"], l.panes["PageNumber"]
        up.tx, up.ty = x - ax, y + h
        dn.tx, dn.ty = x - ax, y - h
        pg.tx, pg.ty = x, y
        if not state & 1: up.alpha = 90
        if not state & 2: dn.alpha = 90
        l.set_text("PageNumber", text)
        l.draw(cv, 0, 0, alpha, wide=self.wide)

    def window(self, cv, x, y, w, h, alpha):
        l = self.lay("SysInfoWindowMini")
        l.hide("BG", "InfoWindowM", "AButtonPosition", "SaveIconPosition")
        L.pose(l, "ButtonAppear")
        L.pose(l, "ButtonWait", 0)
        for n in ("WinInfo", "ShaInfo"):
            l.panes[n].w, l.panes[n].h = w, h
        l.draw(cv, x, y, alpha, wide=self.wide)

    def hud(self, cv):
        """the HUD's life counter (PlayerLeft, its Wait pose: appeared), the strip sits beside it"""
        l = self.lay("PlayerLeft")
        l.hide("CenterPlayerLeft")
        L.pose(l, "Appear")
        L.pose(l, "Wait", 0)
        l.set_text("TxtNumber", "3") if "TxtNumber" in l.panes else None
        l.draw(cv)

    def character(self, look, selected=False, ghost=False, dark=False):
        """MiiIcon showing a character (MpPanel::kCharacterFrame; Cosmic Mario: Mario tinted, as MpPanelDraw SetCharacter)"""
        l = self.lay("MiiIcon")
        l.hide("MiiIcon")
        L.pose(l, "ButtonAppear", under="MarioIcon")
        L.pose(l, "ButtonWait", 0, under="MarioIcon")
        if selected: L.pose(l, "ButtonSelectIn", under="MarioIcon")
        L.pose(l, "Hide", 0)
        look = look if 0 <= look < 6 else 0
        L.pose(l, "Character", [0.0, 4.0, 2.0, 0.0, 2.0, 4.0][look])
        tint = {3: ((0, 2, 34, 0), (92, 124, 236, 255)), 4: ((0, 0, 0, 0), (255, 255, 255, 255)), 5: ((0, 2, 34, 0), (92, 124, 236, 255))}
        if look in tint:   # (MpPanel::kCharTint)
            for n in ("PicMario", "PicMarioShine"):
                m = l.mats[l.panes[n].mat]
                m.tevc[0] = np.array(tint[look][0], np.float32) / 255
                m.tevc[1] = np.array(tint[look][1], np.float32) / 255
                if look == 4 and n == "PicMario" and not m.stages: toad_stages(m)   # (MpPanelDraw SetToadStages: PicMario only; the Shine keeps its own stages)
        if ghost:   # (MpPanel::kGhostTint, as MpPanelDraw SetCharacter)
            for n in ("PicMario", "PicMarioShine"):
                m = l.mats[l.panes[n].mat]
                m.tevc[0] = np.array([120, 152, 225, 0], np.float32) / 255
                m.tevc[1] = np.array([238, 246, 255, 255], np.float32) / 255
        if dark:    # (the party strip's down head: dark grey wash)
            for n in ("PicMario", "PicMarioShine"):
                m = l.mats[l.panes[n].mat]
                m.tevc[0] = np.array([0, 0, 0, 0], np.float32) / 255
                m.tevc[1] = np.array([110, 114, 130, 255], np.float32) / 255
        return l

    def head(self, cv, x, y, look, scale, alpha):
        # (state: the character | 0x20 pale blue wash (bubble) | 0x40 dark wash (down))
        self.character(look & 0x0F, ghost=bool(look & 0x20), dark=bool(look & 0x40)).draw(cv, x, y, alpha, scale=scale, wide=self.wide)

    def char_panel(self, cv, x, y, state, scale, alpha):
        self.character(state & 0x0F, bool(state & 0x10), bool(state & 0x20)).draw(cv, x, y, alpha, scale=scale or 1.0, wide=self.wide)

    def line(self, cv, text, x, y, alpha):
        # flow look: the game's bottom message (InformationWindow's upper window; MpLineLayout in MpPanelDraw.cpp). The text is wrapped greedily at
        # 34 letters a line (the real box is 480 units wide: about 36 letters of the message font).
        if "InformationWindow" not in self.arcs:
            self.arcs["InformationWindow"] = L.rarc_files(open(os.path.join(self.data, "files", "LayoutData", "InformationWindow.arc"), "rb").read())
        l = self.lay("InformationWindow")
        l.hide("InfoWindowCenter")
        L.pose(l, "Appear")
        L.pose(l, "Line")
        L.pose(l, "Wait", 0)
        lines, cur = [], ""
        for para in text.split("\n"):
            for w in para.split(" "):
                if cur and len(cur) + 1 + len(w) > 34: lines.append(cur); cur = w
                else: cur = (cur + " " + w) if cur else w
            lines.append(cur); cur = ""
        wrapped = "\n".join(lines)
        for n in ("InfConfirmU", "ShaConfirmU"):
            if n in l.panes: l.set_text(n, wrapped)
        l.draw(cv, x, y - 180.0, alpha, wide=self.wide)

    def dialog(self, cv, text, yes, no):
        l = self.lay("SysInfoWindow")
        L.pose(l, "ButtonAppear")
        L.pose(l, "ButtonWait", 0)
        l.set_text("InfoWindow", text)
        if yes:
            l.set_text("RightText", yes); l.set_text("LeftText", no)
        else:
            l.hide("Left", "Right")   # (a Type_Key window: the game shows its A-button icon instead)
        l.draw(cv)


def parse(lines):
    out, cur = {}, None
    for ln in lines:
        p = ln.split()
        if not p: continue
        if p[0] in ("MENU", "TOAST", "STRIP"):
            cur = out.setdefault(p[0], {"T": {}, "U": [], "V": [], "R": []}); continue
        if p[0] == "S": out["S"] = p; continue
        if cur is None: continue
        if p[0] == "T": cur["T"][int(p[1])] = to_text(int(c) for c in p[2:])
        elif p[0] == "U": cur["U"].append([int(v) for v in p[1:6]] + [float(v) for v in p[6:11]])
        elif p[0] == "V": cur["V"].append((float(p[1]), float(p[2]), tuple(int(c) for c in p[3:7])))
        elif p[0] == "R": cur["R"].append((int(p[1]), [float(v) for v in p[2:6]], [int(c) for c in p[6:10]], [int(c) for c in p[10:]]))
        elif p[0] == "E": cur["E"] = p[1:]
    return out


def draw_plan(cv, kit, part):
    for i in range(0, len(part["V"]) - 2, 3):
        cv.tri(part["V"][i:i + 3])
    fonts = {0: kit.fonts["messagefont26.brfnt"], 1: kit.fonts["picturefont.brfnt"]}
    for f in (0, 1):
        for font, (x, y, sx, sy), col, codes in part["R"]:
            if font != f: continue
            fnt = fonts[f]
            rgba = [c / 255.0 for c in col]
            for code in codes:
                g, tex, left, gw, cw = fnt.glyph(code)
                if gw > 0:
                    X0, Y0 = (x + left * sx) * cv.sx, y * cv.sy
                    X1, Y1 = (x + (left + gw) * sx) * cv.sx, (y + fnt.ch * sy) * cv.sy

                    def shader(u, v, tex=tex, pic=(f == 1)):
                        s = L.sample(tex, u, v)
                        if pic:
                            out = s * np.array(rgba, np.float32)
                        else:
                            out = np.concatenate([np.broadcast_to(np.array(rgba[:3], np.float32), s[..., :3].shape), (s[..., 3] * rgba[3])[..., None]], -1)
                        return out
                    cv.shade_quad((X0, Y0, X1, Y1), shader)
                x += cw * sx


def render_part(cv, kit, part, heads_first=False):
    T = part["T"]
    uses = part["U"]
    order = {0: 0, 2: 1, 1: 2, 3: 4}   # frame, windows, buttons, (plan), heads
    for kind in (0, 2, 1, 4):
        for kind_, slot, state, t1, t2, x, y, w, h, a in uses:
            if kind_ != kind: continue
            if kind == 0: kit.frame(cv, T.get(t1, ""), T.get(t2, ""), a, state, w, h)
            elif kind == 2: kit.window(cv, x, y, w, h, a)
            elif kind == 1: kit.button(cv, x, y, T.get(t1, ""), state, a, w)
            elif kind == 4: kit.char_panel(cv, x, y, state, w, a)
    if heads_first:   # (the party strip: its heads, then its Power Star pictures)
        for kind_, slot, state, t1, t2, x, y, w, h, a in uses:
            if kind_ == 3: kit.head(cv, x, y, state, w, a)
        draw_plan(cv, kit, part)
        return
    draw_plan(cv, kit, part)
    for kind_, slot, state, t1, t2, x, y, w, h, a in uses:
        if kind_ == 6: kit.arrows(cv, x, y, w, h, a)
        elif kind_ == 7: kit.scroll(cv, x, y, h, state, T.get(t1, ""), a)
    for kind_, slot, state, t1, t2, x, y, w, h, a in uses:
        if kind_ == 3: kit.head(cv, x, y, state, w, a)
    for kind_, slot, state, t1, t2, x, y, w, h, a in uses:
        if kind_ == 5: kit.pointer(cv, x, y, w, h, a)
    for kind_, slot, state, t1, t2, x, y, w, h, a in uses:
        if kind_ == 8: kit.line(cv, T.get(t1, ""), x, y, a)   # (USE_LINE: the game's bottom message)


def main():
    data, out = sys.argv[1], sys.argv[2]
    only = sys.argv[3:]
    os.makedirs(out, exist_ok=True)
    work = os.path.join(out, "preview-work")
    os.makedirs(work, exist_ok=True)
    fontarc = L.rarc_files(open(os.path.join(data, "files", "LayoutData", "Font.arc"), "rb").read())
    msg = L.RFont(fontarc["font/messagefont26.brfnt"]); pic = L.RFont(fontarc["font/picturefont.brfnt"])
    metrics = os.path.join(work, "metrics.txt")
    with open(metrics, "w") as f:
        for i, font in enumerate((msg, pic)):
            f.write("H %d %d %d\n" % (i, font.height, font.base))
            for code, g in font.cmap.items():
                f.write("C %d %d %d\n" % (i, code, font.widths.get(g, (0, 0, font.cw))[2]))
    exe = os.path.join(work, "preview")
    src = os.path.join(ROOT, "decomp", "src", "Game", "MP")
    srcs = [os.path.join(HERE, "preview.cpp")] + [os.path.join(src, n) for n in ("MpMenuSequence.cpp", "MpMenuMain.cpp", "MpMenuGather.cpp", "MpMenuChars.cpp",
        "MpMenuSession.cpp", "MpMenuFiles.cpp", "MpMenuFriends.cpp", "MpMenuConnTest.cpp", "MpMenuRules.cpp", "MpToast.cpp", "MpMessages.cpp", "MpMessagesExtra.cpp", "MpPanelPlan.cpp")]
    ismock = lambda s: s.startswith("settings-mock") or s.startswith("leave-mock")
    if not only or not all(ismock(s) for s in only):
        subprocess.check_call(["clang++", "-std=c++20", "-O1", "-fshort-wchar", "-Wall", "-Wextra", "-Wno-unused-parameter",
                               "-I" + os.path.join(ROOT, "decomp", "include"), "-I" + os.path.join(ROOT, "platform", "include"), "-o", exe] + srcs)
    # native Settings mocks: their own small program (no menu state machine)
    sexe = os.path.join(work, "settings_preview")
    subprocess.check_call(["clang++", "-std=c++20", "-O1", "-fshort-wchar", "-Wall", "-Wextra", "-Wno-unused-parameter",
                           "-I" + os.path.join(ROOT, "decomp", "include"), "-I" + os.path.join(ROOT, "platform", "include"), "-o", sexe,
                           os.path.join(HERE, "settings_preview.cpp"), os.path.join(src, "MpMessages.cpp"), os.path.join(src, "MpMessagesExtra.cpp"), os.path.join(src, "MpPanelPlan.cpp")])
    screens = only or subprocess.check_output([exe, "list"]).decode().split()
    ALL = ["UsEnglish", "UsFrench", "UsSpanish"]
    if os.environ.get("PREVIEW_LANGS") in ("all", "en"):
        langs = {}
    else:
      langs = {"address": ALL, "lobby-address": ALL, "lobby-address-forward": ALL, "hostjoin": ALL, "friends": ALL, "friends-none": ALL, "requests": ALL, "request": ALL, "blocked": ALL, "blocked-empty": ALL,
             "main": ["UsEnglish", "UsFrench", "UsSpanish"], "lobby-host": ["UsEnglish", "UsFrench"], "setup": ["UsEnglish", "UsFrench", "UsSpanish"], "setup-race": ["UsEnglish", "UsFrench", "UsSpanish"], "more": ["UsEnglish", "UsFrench", "UsSpanish"], "more-race": ["UsEnglish", "UsFrench", "UsSpanish"], "save": ["UsEnglish", "UsFrench", "UsSpanish"],
             "chars": ["UsEnglish", "UsFrench", "UsSpanish"], "chars-taken": ["UsEnglish", "UsFrench", "UsSpanish"],
             # chat
             "messages": ALL, "messages-public": ALL, "players-mute": ALL, "ring": ALL, "ring-picked": ["UsEnglish"], "chat-toasts": ALL}
    ALLL = ["UsEnglish", "UsFrench", "UsSpanish"]
    for screen in screens:
        if os.environ.get("PREVIEW_LANGS") == "all": langs[screen] = ALLL
        if screen.startswith("dialog:"):
            key = screen.endswith(":key")
            screen = screen[:-4] if key else screen
            for lang in ["UsEnglish", "UsFrench", "UsSpanish"]:
                txt = subprocess.check_output([exe, metrics, screen, lang]).decode().splitlines()
                parts = {ln.split()[0]: to_text(int(c) for c in ln.split()[1:]) for ln in txt}
                kit = Kit(data, lang)
                cv = L.Canvas(); backdrop(cv)
                body = re.sub("\x1a.{2}", "", parts.get("D", ""), flags=re.S)
                kit.dialog(cv, body, None if key else parts.get("D_Yes"), parts.get("D_No"))
                name = "dialog-%s-%s" % (screen[7:].lower(), lang[2:4].lower())
                cv.image().save(os.path.join(out, name + ".png"))
                print("%s.png" % name)
            continue
        if ismock(screen):   # (native Settings: all five languages; German and Italian use the English art)
            langs[screen] = ["UsEnglish", "UsFrench", "UsSpanish", "UsGerman", "UsItalian"]
        for lang in langs.get(screen, ["UsEnglish"]):
            txt = subprocess.check_output([sexe if ismock(screen) else exe, metrics, screen, lang]).decode().splitlines()
            res = parse(txt)
            kit = Kit(data, lang if lang in ("UsEnglish", "UsFrench", "UsSpanish") else "UsEnglish")
            cv = L.Canvas(float(res["S"][1]), float(res["S"][2]))
            kit.wide = float(res["S"][1]) > 700
            backdrop(cv)
            hudscreen = screen.startswith("party")
            if hudscreen: kit.hud(cv)
            render_part(cv, kit, res["MENU"])
            render_part(cv, kit, res["TOAST"])
            render_part(cv, kit, res["STRIP"], heads_first=True)
            name = screen if lang == "UsEnglish" else "%s-%s" % (screen, {"UsGerman": "de", "UsItalian": "it"}.get(lang, lang[2:4].lower()))
            if os.environ.get("PREVIEW_ASPECT") == "43": name += "-43"
            cv.image().save(os.path.join(out, name + ".png"))
            print("%s.png: state %s, %d layouts, %d vertices, overflow %s" % (name, res["S"][3], len(res["MENU"]["U"]) + len(res["TOAST"]["U"]),
                                                                            len(res["MENU"]["V"]), res["MENU"].get("E")))


if __name__ == "__main__":
    main()
