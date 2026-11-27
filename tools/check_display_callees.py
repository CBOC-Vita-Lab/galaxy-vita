#!/usr/bin/env python3
"""check_display_callees.py: best-effort list of calls in the display-callback functions that tools/check_display_stack.py does not know (fable31fix, S2).

The stack gate sums only the frames it is TOLD about (FUNCS / CHAINS in check_display_stack.py). A new function called from PaceDisplay, SyncDisplay, EvenShown, SmoothShown,
PaceGate, LatencyBlock or LatencyShown runs on the display queue's 4 KiB system stack and is not seen unless someone adds it there. This tool reads those functions' bodies in
platform/vgx/vita/vgx_host_galaxy_vita.cpp (brace matching, comments and string literals blanked), collects every `name(` call, and lists the ones that are neither in KNOWN
(the gate's table, the library calls, the cheap inlined helpers already reviewed) nor a language keyword. Best effort: it does not follow calls into the callees, does not see macros
that hide a call, and calls through a function pointer look like their variable name. A listed name is not an error: look at its frame (-fstack-usage), then add it to FUNCS / CHAINS
in check_display_stack.py (if it needs a frame budget) and to KNOWN here.
Usage: check_display_callees.py [--root DIR] [--strict] [--file F]      exit 0 (unknown callees only listed), 1 with --strict when there are some, 2 a function was not found.
"""
import argparse, os, re, sys

DISPLAY_FUNCS = ["PaceDisplay", "SyncDisplay", "EvenShown", "SmoothShown", "PaceGate", "LatencyBlock", "LatencyShown"]
SRC = "platform/vgx/vita/vgx_host_galaxy_vita.cpp"

KEYWORDS = set("if for while switch return sizeof static_cast reinterpret_cast const_cast dynamic_cast alignof decltype catch defined else do case new delete throw operator".split())
# Reviewed callees. Group 1: the functions the stack gate measures (check_display_stack.py FUNCS). Group 2: library, std:: and cheap helpers (no frame of note: checked with the .su files).
KNOWN = set("""
PaceDisplay SyncDisplay EvenShown SmoothShown PaceGate LatencyBlock LatencyShown
port_log_summary_cb port_log_summary port_pace_even_info port_pose_phase_window_take port_q30lock_shown port_vgx_fif_retire port_vgx_even_take port_vgx_gate_wait_take
Format Compute Picture Drain33On EnvLong
port_vgx_pace_waited port_hrw_still port_screen_quiet port_scene_loading_active port_pace_steady30_active port_pace_steady30_set
snprintf vsnprintf memcpy memset strlen memcmp strcmp getenv atol atoi
min max abs fabs lround llround
load store fetch_add exchange fetch_sub compare_exchange_strong
clock_gettime sceKernelGetSystemTimeWide sceKernelGetSystemTimeLow sceKernelGetThreadId
vglSwapBuffers vglSetDisplayCallback sqrt fmax GXV_CB_LOG
""".split())
# Group 3 (fable31fix, 18 Nov): the other 52 callees of the seven functions, read against the .su files of the real Vita compile (check_display_stack.py --keep): header-only
# model methods (port/*.h) and small renderer helpers, each inlined or with a standalone frame of at most 64 bytes (the largest: Ladder::Take6 64, NotePicture 64); none allocates.
# A name added here is a claim that its frame was read: re-check with --keep when a model's body changes much.
KNOWN |= set("""
Active Aligned Allow AllowDrop Behind Break Clear ClearStats Config Configure ConfigureLatency24 Count DrainFloor DrainNext Dropped Due EnvInt FifRetire GetStats GlobalHoldMeter
Latency Latency24On LatencyCensus LatencyLogOn Level MarginUs Next NoteBacklog NoteTight NowUs Pop PublishWindow QualityRung Reset SetExternalDrop SetSlack Shown Size Take Target Total
WaitForSlot WaitLogOn WaitVblank fmax port_input_age_take port_pace_steady30_changes port_pace_steady30_engaged port_vgx_pace_divisor port_vgx_present_sync_mode
port_vgx_queue_max_take sceDisplayGetVcount sceDisplayWaitVblankStart
""".split())


def blank(text):
    """comments and string / char literals replaced by spaces (newlines kept), so braces and names inside them are not seen"""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i)); i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(re.sub(r"[^\n]", " ", text[i:j])); i = j
        else:
            out.append(c); i += 1
    return "".join(out)


def bodies(text, name):
    """[(start line, body text)] of every DEFINITION of `name` (a `name(...) {` at the start of a line, not a declaration ending in ';')"""
    res = []
    for m in re.finditer(r"^[A-Za-z_][^\n;{}]*\b" + re.escape(name) + r"\s*\(", text, re.M):
        j = m.end(); depth = 1
        while j < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[j], 0); j += 1
        k = j
        while k < len(text) and text[k] in " \t\r\n":   # (const / noexcept qualifiers are not used on these functions)
            k += 1
        if k >= len(text) or text[k] != "{":
            continue
        depth = 0; e = k
        while e < len(text):
            depth += {"{": 1, "}": -1}.get(text[e], 0)
            e += 1
            if depth == 0: break
        res.append((text.count("\n", 0, k) + 1, text[k:e]))
    return res


def callees(body):
    names = {}
    for m in re.finditer(r"([A-Za-z_][A-Za-z_0-9]*)\s*\(", body):
        nm = m.group(1)
        if nm in KEYWORDS: continue
        names.setdefault(nm, body.count("\n", 0, m.start()))
    return names


def scan(text, funcs=DISPLAY_FUNCS, known=KNOWN):
    """({function: [(callee, line offset)]} of the unknown callees, [functions not found])"""
    clean = blank(text)
    unknown, missing = {}, []
    for f in funcs:
        bs = bodies(clean, f)
        if not bs:
            missing.append(f); continue
        for start, b in bs:
            for nm, off in callees(b).items():
                if nm not in known:
                    unknown.setdefault(f, []).append((nm, start + off))
    return unknown, missing


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    ap.add_argument("--file")
    ap.add_argument("--strict", action="store_true")
    a = ap.parse_args(argv)
    path = a.file or os.path.join(a.root, SRC)
    text = open(path, errors="replace").read()
    unknown, missing = scan(text)
    for f in missing:
        print(f"callees: function {f} not found in {path} (renamed? update DISPLAY_FUNCS)")
    if missing:
        return 2
    allnames = sorted({n for v in unknown.values() for n, _ in v})
    for f, v in unknown.items():
        print(f"callees: {f}: " + ", ".join(f"{n} (line {ln})" for n, ln in v))
    if allnames:
        print(f"callees: {len(allnames)} call(s) on the display thread not in the stack gate's tables / KNOWN: {', '.join(allnames)}")
        print("callees: for each: read its frame (-fstack-usage), add it to FUNCS / CHAINS in tools/check_display_stack.py if it matters, then to KNOWN in this file")
        return 1 if a.strict else 0
    print("callees: every call in the display-callback functions is in the table")
    return 0


if __name__ == "__main__":
    sys.exit(main())
