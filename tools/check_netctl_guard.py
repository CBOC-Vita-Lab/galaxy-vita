#!/usr/bin/env python3
"""mpcrash32 / rev32 F4: every call into sceNet* (sceNetCtl*, sceNetAdhoc*, sceNetSocket..., sceNetGetStatisticsInfo...) must be guarded, because the
NET module may not be loaded and an unresolved import stub jumps to 0.

platform/src/mp: a call passes when
  (a) mp_netctl_live() is in the SAME STATEMENT / condition as the call (from the previous ';', '{' or '}' up to the call), or
  (a2) mp_netctl_live() is in the condition of an enclosing if/while block (`if (mp_netctl_live() && ...) { ... call ... }`), or
  (b) an early-return guard `if (!mp_netctl_live() ...) return` stands earlier in the SAME FUNCTION (no '}' in column 0 in between).
net/src: the backends below load the NET module themselves before using it, so they are allow-listed as whole files (each with its reason);
any other file there is checked like platform/src/mp.

Run: python3 tools/check_netctl_guard.py  (exit 1 = an unguarded call; --root DIR checks another tree, for the self test)."""
import re, sys, pathlib

ALLOW = {  # net/src files that load SCE_SYSMODULE_NET (or are only reached after a backend did) before any sceNet call
    "vlink_vita.cpp": "WifiOpen loads SCE_SYSMODULE_NET (sceSysmoduleIsLoaded / LoadModule) before sceNetInit and every later call",
    "vlink_vita_radio.cpp": "the radio's start sequence has a MODULE_NET step that loads the module before any sceNet call; calls run only after it",
    "np_udp_vita.cpp": "NetUp() loads SCE_SYSMODULE_NET before sceNetInit; every other call goes through NetUp",
    "np_natpmp.cpp": "online code: reached only through np_udp_vita's NetUp, which loads the module",
    "np_https_sys.cpp": "EnsureNet() loads SCE_SYSMODULE_NET before sceNetInit; every other call goes through it",
}
call = re.compile(r"\bsceNet\w*\s*\(")
early = re.compile(r"if\s*\(\s*!\s*mp_netctl_live\s*\(\s*\)[^;{]*\)\s*return")


def strip(src):
    out = re.sub(r"/\*.*?\*/", lambda m: re.sub(r"[^\n]", " ", m.group(0)), src, flags=re.S)
    out = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', out)
    out = re.sub(r"//[^\n]*", "", out)
    return "\n".join("" if l.lstrip().startswith("#") else l for l in out.split("\n"))


def enclosing_guard(pre):
    depth = 0
    i = len(pre) - 1
    while i >= 0:
        c = pre[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                head = max(pre.rfind(";", 0, i), pre.rfind("{", 0, i), pre.rfind("}", 0, i)) + 1
                if "mp_netctl_live" in pre[head:i]:
                    return True
            else:
                depth -= 1
        i -= 1
    return False


def check(path, strict_mp):
    src = path.read_text()
    code = strip(src)
    bad = []
    for m in call.finditer(code):
        pre = code[:m.start()]
        st = max(pre.rfind(";"), pre.rfind("{"), pre.rfind("}")) + 1
        if "mp_netctl_live" in code[st:m.end()]:
            continue
        if enclosing_guard(pre):
            continue
        fn = pre.rfind("\n}") + 1
        if early.search(code[fn:m.start()]):
            continue
        bad.append((code.count("\n", 0, m.start()) + 1, src.split("\n")[code.count("\n", 0, m.start())].strip()))
    return bad


def main():
    args = sys.argv[1:]
    root = pathlib.Path(__file__).resolve().parent.parent
    if "--root" in args:
        root = pathlib.Path(args[args.index("--root") + 1])
    n = 0
    for sub in ("platform/src/mp", "net/src"):
        d = root / sub
        for f in sorted(d.rglob("*.cpp")):
            if sub == "net/src" and f.name in ALLOW:
                continue
            for line, text in check(f, sub != "net/src"):
                print("%s:%d: unguarded: %s" % (f.relative_to(root), line, text)); n += 1
    sys.exit(1 if n else 0)


main()
