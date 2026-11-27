#!/usr/bin/env python3
"""check_weak_links.py: fail when the final Vita link left out code that the port only references WEAKLY.

A weak reference never pulls a member out of an archive. The port declares many of its cross-module hooks weak (`if (fn) fn();`) so that host
tests link without them; on the Vita the runtime is an archive, so a member whose every reference is weak is silently left out and the
feature does not exist on the console while the host build, which links everything, works. Found three times: the multiplayer objects
(MP3 vitalink), the SceNet watch stubs, and the native Settings' live setters (settings_live.cpp, linkfix31: no "Triangle: Settings" hint).

usage: tools/check_weak_links.py ELF [--build-dir B [--remap OLD=NEW ...]] [--inputs A.a|X.o|DIR ...] [--allow F] [--must-define F]
                                     [--src ROOT] [--map LINKMAP] [--nm NM] [--readelf R] [-v]
  ELF            the UNSTRIPPED final link (vita-vgx-game/galaxy_native), not eboot.bin
  --build-dir B  the build directory of tools/vgx_build/build.py (BUILD_DIR): the link inputs are read from B/vita-vgx-game/build.ninja
                 (the galaxy_native link's archives, -l libraries from $VITASDK/arm-vita-eabi/lib). Paths recorded under another build
                 root (a build run in a mount namespace) are mapped to B; --remap OLD=NEW maps any other prefix (e.g. /kit=<kit dir>).
  --inputs P...  more link inputs to scan: archives, objects, or directories (searched for *.a *.o *.obj)
  --allow F      symbols that may stay undefined / weak, one per line: `name  # the reason` (a reason is required)
  --must-define F  symbols the ELF must DEFINE strongly and globally (nm T/D/B/R), one per line with `#` comments (tools/vita_must_define.txt)
  --src ROOT     repo root: also scans platform/ vita/ decomp/src/Game for weak declarations and checks that each linked one is strong
  --map F        ld -Map file: names the archive member a missing name would come from

Checks (each failure is one FAIL line; exit 1 when there is any, 0 otherwise, 2/3 on a usage or tool error):
  left-out      a symbol is weak-undefined in the ELF, and a link input DEFINES it: the member that has the code was not linked
  weak-won      the ELF has a WEAK definition (a default stub), and a link input has a STRONG one that was not linked
  undefined-weak  a weak-undefined symbol that no input defines and the allow list does not name (with --build-dir/--inputs this is
                  reported but only fails when no input was scanned, i.e. in the old mode)
  must-define   a --must-define symbol that the ELF does not define strongly and globally (a local t/d/b/r of the same name cannot satisfy
                another unit's reference: C186.1's Settings gate asked for global text, folded in here)
Run after every build:  python3 tools/check_weak_links.py <B>/vita-vgx-game/galaxy_native --build-dir <B> \\
                            --allow tools/weak_links.allow --must-define tools/vita_must_define.txt
(tools/vgx_build/build.py's link stage runs exactly this.)
"""
import argparse, os, re, shlex, subprocess, sys

# libc/toolchain weak hooks that are normally absent (the C runtime's own optional hooks)
BUILTIN_OK = {"_ITM_deregisterTMCloneTable": "toolchain: transactional memory clone table (no libitm)",
              "_ITM_registerTMCloneTable": "toolchain: transactional memory clone table (no libitm)",
              "__register_frame_info": "toolchain: crtbegin's frame registration (ARM uses .ARM.exidx)",
              "__deregister_frame_info": "toolchain: crtbegin's frame registration (ARM uses .ARM.exidx)",
              "__libc_fini": "newlib: optional fini hook",
              "__gnu_Unwind_Find_exidx": "libgcc: optional exidx finder (the static one is used)",
              "_pthread_stack_default_user": "pthread-embedded: optional default stack size override",
              "__gmon_start__": "toolchain: profiling hook (no gprof)",
              "__cxa_finalize": "toolchain: shared-object finalizer (static link)"}

STRONG_TYPES = set("TtDdBbRrGgSsC")      # nm letters of a strong (non-weak) definition; lower case = local
GLOBAL_STRONG = set("TDBRGSC")           # a global strong definition (what can satisfy another unit's reference)
WEAK_DEF_TYPES = set("WV")               # nm: W/V weak defined (lower case w/v = weak undefined)


def tool(name, override=None):
    if override:
        return override
    sdk = os.environ.get("VITASDK", "/opt/vitasdk")
    cand = os.path.join(sdk, "bin", "arm-vita-eabi-" + name)
    return cand if os.path.exists(cand) else name


def run(args):
    out = subprocess.run(args, capture_output=True, text=True, errors="replace")
    if out.returncode != 0:
        print("%s failed: %s" % (os.path.basename(args[0]), out.stderr.strip()[:400]), file=sys.stderr)
        sys.exit(3)
    return out.stdout


def elf_symbols(elf, nm):
    """name -> set of nm type letters (mangled names)."""
    syms = {}
    for line in run([nm, "--no-demangle", elf]).splitlines():
        p = line.split()
        if len(p) == 3: _, t, name = p
        elif len(p) == 2: t, name = p
        else: continue
        syms.setdefault(name, set()).add(t)
    return syms


def undefined_weak(elf, readelf):
    """names that are WEAK and UND in the ELF's symbol table (nm prints them as 'w'/'v' with no address)."""
    res = []
    for line in run([readelf, "-sW", elf]).splitlines():
        p = line.split()
        if len(p) >= 8 and p[4] == "WEAK" and p[6] == "UND":
            res.append(p[7].split("@")[0])
    return sorted(set(res))


def read_list(path, need_reason=False):
    """name -> reason ('' when none). Lines: `name  # reason`."""
    out = {}
    if not path:
        return out
    for i, ln in enumerate(open(path, errors="replace"), 1):
        body, _, reason = ln.partition("#")
        body = body.strip()
        if not body:
            continue
        name = body.split()[0]
        if need_reason and not reason.strip():
            print("%s:%d: %s has no reason (write `%s  # why it may stay unlinked`)" % (path, i, name, name), file=sys.stderr)
            sys.exit(2)
        out[name] = reason.strip()
    return out


# ---- link inputs -----------------------------------------------------------------------------------------------------------------------
def link_inputs_from_build(build_dir, remaps):
    """The archives of the galaxy_native link, from <build>/vita-vgx-game/build.ninja (LINK_LIBRARIES of the executable)."""
    game = os.path.join(build_dir, "vita-vgx-game")
    ninja = os.path.join(game, "build.ninja")
    if not os.path.isfile(ninja):
        print("no %s (run the link stage first)" % ninja, file=sys.stderr); sys.exit(2)
    text = open(ninja, errors="replace").read()
    m = re.search(r"^build galaxy_native\b.*?(?=^build |\Z)", text, re.S | re.M)
    libs = re.search(r"^\s*LINK_LIBRARIES = (.*)$", m.group(0), re.M) if m else None
    if not libs:
        print("%s: no LINK_LIBRARIES for galaxy_native" % ninja, file=sys.stderr); sys.exit(2)
    # The build root the paths were recorded under (a build in a mount namespace records its own): the runtime archive's grandparent.
    tokens = shlex.split(libs.group(1))
    recorded = None
    for t in tokens:
        if t.endswith("/vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a"):
            recorded = t[: -len("/vita-vgx-runtime/libgalaxy_vita_runtime_vgx.a")]
    maps = list(remaps)
    if recorded and os.path.abspath(recorded) != os.path.abspath(build_dir):
        maps.append((recorded, build_dir))
    sdk = os.environ.get("VITASDK", "/opt/vitasdk")
    libdirs = [os.path.join(sdk, "arm-vita-eabi", "lib")]
    paths, missing = [], []
    for t in tokens:
        if t.startswith("-Wl,") or t in ("-pthread",):
            if t == "-pthread":
                t = "-lpthread"   # VitaSDK's spec: --whole-archive -lpthread
            else:
                continue
        if t.startswith("-l"):
            found = [os.path.join(d, "lib%s.a" % t[2:]) for d in libdirs if os.path.isfile(os.path.join(d, "lib%s.a" % t[2:]))]
            (paths if found else missing).append(found[0] if found else t)
            continue
        if t.startswith("-"):
            continue
        p = t if os.path.isabs(t) else os.path.join(game, t)
        if not os.path.exists(p):
            for old, new in maps:
                if p == old or p.startswith(old.rstrip("/") + "/"):
                    q = new.rstrip("/") + p[len(old.rstrip("/")):]
                    if os.path.exists(q):
                        p = q
                        break
        (paths if os.path.exists(p) else missing).append(p)
    return paths, missing


def expand_inputs(items):
    out = []
    for p in items:
        if os.path.isdir(p):
            for dp, dn, fn in os.walk(p):
                dn.sort()
                for f in sorted(fn):
                    if f.endswith((".a", ".o", ".obj")):
                        out.append(os.path.join(dp, f))
        else:
            out.append(p)
    return out


def input_definitions(paths, nm):
    """name -> list of (input, member, type) for every DEFINED global/weak symbol of the inputs."""
    defs = {}
    for path in paths:
        out = subprocess.run([nm, "-A", "-P", "--defined-only", "--no-demangle", path], capture_output=True, text=True, errors="replace")
        if out.returncode != 0 and not out.stdout:
            print("  (not scanned: %s: %s)" % (path, out.stderr.strip()[:200]))
            continue
        for line in out.stdout.splitlines():
            # posix format with -A: "lib.a[member.o]: name T value size" or "file.o: name T value size"
            head, sep, rest = line.partition(": ")
            if not sep:
                continue
            p = rest.split()
            if len(p) < 2:
                continue
            name, t = p[0], p[1]
            if t not in GLOBAL_STRONG and t not in WEAK_DEF_TYPES:
                continue       # locals, undefined, debug
            m = re.match(r"^(.*)\[(.*)\]$", head)
            src, member = (m.group(1), m.group(2)) if m else (head, "")
            defs.setdefault(name, []).append((src, member, t))
    return defs


def where(entries):
    seen = []
    for src, member, t in entries:
        s = "%s(%s) %s" % (os.path.basename(src), member, t) if member else "%s %s" % (src, t)
        if s not in seen:
            seen.append(s)
    return ", ".join(seen[:4]) + (" ..." if len(seen) > 4 else "")


# ---- source scan (the old --src mode) ----------------------------------------------------------------------------------------------------
def source_weak_names(root):
    """names declared weak in platform/ vita/ decomp/src/Game (declaration lines only)."""
    names = {}
    for sub in ("platform", "vita", "decomp/src/Game"):
        for dp, dn, fn in os.walk(os.path.join(root, sub)):
            if "/vitaGL" in dp or "/third" in dp: continue
            for f in fn:
                if not f.endswith((".c", ".cc", ".cpp", ".h", ".hpp", ".inl")): continue
                path = os.path.join(dp, f)
                try: lines = open(path, errors="replace").read().splitlines()
                except OSError: continue
                for i, ln in enumerate(lines, 1):
                    if "weak" not in ln: continue
                    m = re.search(r'#pragma\s+weak\s+(\w+)', ln)
                    if m: names.setdefault(m.group(1), (path, i, False)); continue
                    if "__attribute__" not in ln or not re.search(r'weak', ln): continue
                    # a definition (has a body on the line) is a weak STUB; a declaration is a weak reference
                    isdef = bool(re.search(r'\)\s*(?:const\s*)?\{', ln))
                    cand = [x for x in re.findall(r'(\w+)\s*\(', ln) if x not in ("__attribute__", "weak", "format", "printf", "__asm__", "extern", "void", "int", "unsigned", "const", "sizeof")]
                    asm = re.search(r'__asm__\("(\w+)"\)', ln)
                    name = asm.group(1) if asm else (cand[0] if cand else None)
                    if not name:
                        m = re.search(r'(\w+)\s*(?:\[[^\]]*\])?\s*__attribute__', ln)
                        name = m.group(1) if m else None
                    if name: names.setdefault(name, (path, i, isdef))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf")
    ap.add_argument("--build-dir")
    ap.add_argument("--remap", action="append", default=[], metavar="OLD=NEW")
    ap.add_argument("--inputs", nargs="+", default=[])
    ap.add_argument("--allow")
    ap.add_argument("--must-define")
    ap.add_argument("--src")
    ap.add_argument("--map")
    ap.add_argument("--nm")
    ap.add_argument("--readelf")
    ap.add_argument("-v", action="store_true", help="list every input and the names absent from the ELF")
    a = ap.parse_args()
    nm, readelf = tool("nm", a.nm or os.environ.get("NM")), tool("readelf", a.readelf)
    if not os.path.isfile(a.elf):
        print("no ELF %s" % a.elf, file=sys.stderr); sys.exit(2)
    remaps = []
    for r in a.remap:
        old, sep, new = r.partition("=")
        if not sep or not old or not new:
            print("--remap wants OLD=NEW, got %r" % r, file=sys.stderr); sys.exit(2)
        remaps.append((old, new))

    allow = dict(BUILTIN_OK)
    allow.update(read_list(a.allow, need_reason=True))
    bad = []
    syms = elf_symbols(a.elf, nm)
    und = undefined_weak(a.elf, readelf)

    # 1. the link inputs and what they define
    inputs, missing_inputs = [], []
    if a.build_dir:
        inputs, missing_inputs = link_inputs_from_build(os.path.abspath(a.build_dir), remaps)
    inputs += expand_inputs(a.inputs)
    defs = input_definitions(inputs, nm) if inputs else {}
    if inputs:
        print("link inputs scanned: %d (%d defined names)%s" % (len(inputs), len(defs),
              "; NOT FOUND: " + " ".join(missing_inputs) if missing_inputs else ""))
        if a.v:
            for p in inputs: print("  input %s" % p)

    # 2. weak-undefined in the ELF
    print("weak-undefined symbols in the ELF: %d" % len(und))
    for n in und:
        d = defs.get(n)
        if d:
            tag = "allowed" if n in allow else "LEFT-OUT"
            print("  %-8s %s: defined in %s, but no strong reference pulled it in%s" % (tag, n, where(d), (" (" + allow[n] + ")") if n in allow else ""))
            if n not in allow:
                bad.append(("left-out", "%s: defined in %s; the ELF has only a weak reference (require it in vita/vgx-game/CMakeLists.txt "
                            "with -Wl,--require-defined=%s and list it in tools/vita_must_define.txt)" % (n, where(d), n)))
        elif n in allow:
            print("  allowed  %s (%s)" % (n, allow[n]))
        else:
            print("  MISSING  %s: no scanned input defines it" % n)
            if not inputs:
                bad.append(("undefined-weak", n))

    # 3. a weak default won over a strong definition that was not linked
    if defs:
        won = 0
        for n, types in sorted(syms.items()):
            if not (types & WEAK_DEF_TYPES) or (types & STRONG_TYPES):
                continue
            strong = [e for e in defs.get(n, ()) if e[2] in GLOBAL_STRONG]
            if not strong:
                continue
            won += 1
            if n in allow:
                print("  allowed  weak-won %s: strong in %s (%s)" % (n, where(strong), allow[n]))
            else:
                bad.append(("weak-won", "%s: the ELF has the WEAK default; the strong definition in %s was not linked" % (n, where(strong))))
        print("weak definitions with an unlinked strong one: %d" % won)

    # 4. must-define
    if a.must_define:
        must = read_list(a.must_define)
        miss = [n for n in must if not (syms.get(n, set()) & GLOBAL_STRONG)]
        for n in miss:
            got = "".join(sorted(syms.get(n, set()))) or "absent"
            bad.append(("must-define", "%s: nm type %s in the ELF, want T (global; D/B/R for data) (%s)" % (n, got, must[n] or a.must_define)))
        print("must-define: %d/%d defined strongly" % (len(must) - len(miss), len(must)))

    # 5. the old source scan
    if a.src:
        base = {}
        for line in run([nm, "-C", a.elf]).splitlines():
            p = line.split(None, 2)
            if len(p) == 3:
                m = re.search(r'(?:^|::)(\w+)\(', p[2])
                if m: base.setdefault(m.group(1), set()).add(p[1])
        names = source_weak_names(a.src)
        counts = {"strong": 0, "weak-def": 0, "gc'd/absent": 0}
        for n, (path, line, isdef) in sorted(names.items()):
            if n in allow: continue
            types = syms.get(n) or base.get(n)
            if not types:
                counts["gc'd/absent"] += 1   # no caller survived gc-sections, or the ELF is older than the source: informational
                if a.v: print("  absent: %s (%s:%d)" % (n, path, line))
                continue
            if types & STRONG_TYPES: counts["strong"] += 1
            elif types & WEAK_DEF_TYPES:
                counts["weak-def"] += 1
                if isdef:
                    bad.append(("weak-stub-won", "%s (%s:%d: the weak stub is what linked; its strong definition did not come in)" % (n, path, line)))
            elif n not in und:
                bad.append(("undefined", "%s (%s:%d)" % (n, path, line)))
        print("source weak names: %d  strong in link: %d  weak-defined: %d  absent from symbol table: %d"
              % (len(names), counts["strong"], counts["weak-def"], counts["gc'd/absent"]))

    if a.map and und:
        txt = open(a.map, errors="replace").read()
        for n in und:
            if n in allow: continue
            m = re.search(r'\n\s*(\S+\.a\([^)]*\)|\S+\.o)\s*\n?[^\n]*\b%s\b' % re.escape(n), txt)
            print("  map hint for %s: %s" % (n, m.group(1) if m else "not in map"))

    seen = set()
    for kind, n in bad:
        if (kind, n) in seen: continue
        seen.add((kind, n))
        print("FAIL %s: %s" % (kind, n))
    print("RESULT: %s" % ("FAIL (%d)" % len(seen) if seen else "OK"))
    sys.exit(1 if seen else 0)


if __name__ == "__main__":
    main()
