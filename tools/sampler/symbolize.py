#!/usr/bin/env python3
"""symbolize.py: turn a Galaxy Vita sampler report (ux0:data/smg/sampler-<time>.txt, platform/src/port/sampler_vita.cpp) into function tables.

usage: tools/sampler/symbolize.py REPORT.txt ELF [--top N] [--thread NAME] [--lines] [--nm PATH] [--addr2line PATH] [--nm-text FILE]

ELF is the unstripped executable of the SAME build that produced the report (galaxy_native next to eboot.bin; eboot.bin itself has no symbols).
Addresses in the report are run-time addresses. The report names the run-time address of port_sampler_boot; the difference to that symbol's address
in the ELF is the load offset and is applied to every pc / lr. (If that symbol is missing the offset is 0 and the tool says so.)

Per thread it prints: the running / waiting split, the wait types, the top functions by self samples (pc), the top callers (lr: the function the thread
was called from, or the return address of the leaf), and the top self<-caller pairs. --lines adds file:line for the top self addresses (addr2line).
--nm-text FILE reads `nm -S -n` text from a file instead of running nm (host tests)."""
import argparse, bisect, collections, os, re, subprocess, sys

NM_DEFAULT = "/opt/vitasdk/bin/arm-vita-eabi-nm"
A2L_DEFAULT = "/opt/vitasdk/bin/arm-vita-eabi-addr2line"
WAIT_NAMES = {0: "none", 1: "thread", 2: "semaphore", 3: "eventflag", 4: "mbox", 5: "vpl", 6: "fpl", 7: "msgpipe", 8: "callback", 9: "thread-event",
              10: "alarm", 11: "vtimer", 64: "sleep", 65: "delay", 66: "suspend", 67: "dormant"}

def parse_report(path):
    h = {"threads": {}, "run": {}, "wait": collections.defaultdict(dict), "pc": collections.defaultdict(list), "anchor": None}
    with open(path, errors="replace") as f:
        lines = f.read().splitlines()
    if not lines or not lines[0].startswith("SMGSAMPLER 1"):
        raise SystemExit("%s: not a sampler report (first line must be 'SMGSAMPLER 1')" % path)
    for ln in lines[1:]:
        p = ln.split()
        if not p:
            continue
        k = p[0]
        if k in ("period_us", "window_ms", "pushed", "kept", "dropped", "ring", "pc_word", "lr_word") and len(p) > 1:
            h[k] = int(p[1])
        elif k in ("reason", "stage") and len(p) > 1:
            h[k] = " ".join(p[1:])
        elif k == "anchor" and len(p) >= 3:
            h["anchor"] = (p[1], int(p[2], 16))
        elif k == "thread" and len(p) >= 5:
            h["threads"][int(p[1])] = {"uid": p[2], "name": p[3], "fails": int(p[4])}
        elif k == "run" and len(p) >= 3:
            h["run"][int(p[1])] = int(p[2])
        elif k == "wait" and len(p) >= 4:
            h["wait"][int(p[1])][int(p[2])] = int(p[3])
        elif k == "pc" and len(p) >= 5:
            h["pc"][int(p[1])].append((int(p[2], 16), int(p[3], 16), int(p[4])))
    return h

class Symbols:
    def __init__(self, text):
        self.addrs, self.rows = [], []
        for ln in text.splitlines():
            p = ln.split(None, 3)
            # nm -S -n: ADDR SIZE TYPE NAME  (SIZE is missing for some symbols)
            if len(p) == 4 and re.fullmatch(r"[0-9a-fA-F]+", p[0]) and re.fullmatch(r"[0-9a-fA-F]+", p[1]) and len(p[2]) == 1:
                addr, size, typ, name = int(p[0], 16) & ~1, int(p[1], 16), p[2], p[3]
            elif len(p) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", p[0]) and len(p[1]) == 1:
                addr, size, typ, name = int(p[0], 16) & ~1, 0, p[1], " ".join(p[2:])
            else:
                continue
            if typ not in "TtWwVvIi":
                continue
            self.addrs.append(addr)
            self.rows.append((size, name))
        order = sorted(range(len(self.addrs)), key=lambda i: self.addrs[i])
        self.addrs = [self.addrs[i] for i in order]
        self.rows = [self.rows[i] for i in order]
    def find(self, name):
        for a, (s, n) in zip(self.addrs, self.rows):
            if n == name or n.startswith(name + "("):
                return a
        return None
    def lookup(self, addr):
        a = addr & ~1
        i = bisect.bisect_right(self.addrs, a) - 1
        if i < 0:
            return None
        size, name = self.rows[i]
        if size and a >= self.addrs[i] + size:
            # inside a gap after a sized symbol: still report it when it is close (alignment padding), else unknown
            if a - (self.addrs[i] + size) > 64:
                return None
        return name

def run_nm(nm, elf):
    try:
        return subprocess.run([nm, "-C", "-S", "-n", "--defined-only", elf], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise SystemExit("cannot run %s on %s: %s" % (nm, elf, e))

def table(title, counter, total, top, fmt_name=lambda x: x):
    print("  %s" % title)
    for key, n in counter.most_common(top):
        print("    %6d %5.1f%%  %s" % (n, 100.0 * n / total if total else 0, fmt_name(key)))

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("report"); ap.add_argument("elf", nargs="?")
    ap.add_argument("--top", type=int, default=25); ap.add_argument("--thread")
    ap.add_argument("--lines", action="store_true"); ap.add_argument("--nm", default=NM_DEFAULT)
    ap.add_argument("--addr2line", default=A2L_DEFAULT); ap.add_argument("--nm-text")
    a = ap.parse_args(argv)
    h = parse_report(a.report)
    if a.nm_text:
        text = open(a.nm_text).read()
    elif a.elf:
        text = run_nm(a.nm, a.elf)
    else:
        raise SystemExit("give the ELF (galaxy_native) or --nm-text")
    syms = Symbols(text)
    delta = 0
    if h["anchor"]:
        name, run_addr = h["anchor"]
        elf_addr = syms.find(name)
        if elf_addr is None:
            print("warning: %s not found in the ELF: load offset taken as 0 (is this the ELF of the build that ran?)" % name)
        else:
            delta = run_addr - elf_addr
            print("load offset %+#x (%s at run time 0x%08x, in the ELF 0x%08x)" % (delta, name, run_addr, elf_addr))
    print("report %s: stage %s, window %d ms (%s), period %d us, %d samples kept, %d dropped" %
          (os.path.basename(a.report), h.get("stage", "?"), h.get("window_ms", 0), h.get("reason", "?"), h.get("period_us", 0), h.get("kept", 0), h.get("dropped", 0)))
    def fn(addr):
        if addr == 0:
            return "(no lr)"
        n = syms.lookup(addr - delta)
        return n if n else "?? 0x%08x" % addr
    for slot in sorted(h["threads"]):
        t = h["threads"][slot]
        if a.thread and t["name"] != a.thread:
            continue
        run, waits = h["run"].get(slot, 0), h["wait"].get(slot, {})
        wsum = sum(waits.values()); tot = run + wsum
        print("\nthread %s (uid %s): %d samples, running %d (%.1f%%), waiting %d (%.1f%%), failed reads %d" %
              (t["name"], t["uid"], tot, run, 100.0 * run / tot if tot else 0, wsum, 100.0 * wsum / tot if tot else 0, t["fails"]))
        if waits:
            table("waits by type", collections.Counter({k: v for k, v in waits.items()}), tot, 8, lambda k: "%d (%s)" % (k, WAIT_NAMES.get(k, "?")))
        selfc, callc, pairc, addrc = collections.Counter(), collections.Counter(), collections.Counter(), collections.Counter()
        for pc, lr, n in h["pc"].get(slot, []):
            f, c = fn(pc), fn(lr)
            selfc[f] += n; callc[c] += n; pairc[(f, c)] += n; addrc[pc] += n
        if run:
            table("top functions, self (pc), of %d running samples" % run, selfc, run, a.top)
            table("top callers (lr)", callc, run, a.top)
            table("top self <- caller", pairc, run, a.top, lambda k: "%s  <-  %s" % k)
            if a.lines:
                try:
                    addrs = [pc - delta for pc, _ in addrc.most_common(min(a.top, 15))]
                    out = subprocess.run([a.addr2line, "-C", "-f", "-e", a.elf] + ["0x%x" % x for x in addrs], capture_output=True, text=True).stdout.splitlines()
                    print("  top self addresses (addr2line)")
                    for (pc, n), i in zip(addrc.most_common(len(addrs)), range(len(addrs))):
                        print("    %6d  0x%08x  %s  %s" % (n, pc, out[2 * i] if 2 * i < len(out) else "?", out[2 * i + 1] if 2 * i + 1 < len(out) else ""))
                except OSError as e:
                    print("  (addr2line unavailable: %s)" % e)

if __name__ == "__main__":
    main()
