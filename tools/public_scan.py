#!/usr/bin/env python3
"""public_scan.py: the checks a tree must pass before it is published (release31). Prints counts and file paths, NEVER a private value.

  public_scan.py --tree DIR --repo REPO [--list-identity]

  identity   every author / committer name and e-mail in `git log --all` of REPO that is not a generic or the public identity (build, Claude, Codex, CBOC-Vita, the
             @local / @localhost / noreply addresses) is private. The names (whole and by word, 4 letters or more), the addresses (whole and the part before @) and
             the machine names inside them are searched in the tree, case-insensitively, text files and (long tokens only) binary files. Reported: "N matches in M
             files" and the paths; the tokens themselves are never printed.
  links      links to the Claude web app (code, chat, artifact, share pages) and session ids
  private    a path with a private / secrets / Vita Conversion folder (the SDK's include/private headers are not that), key and certificate files, .env files
  secrets    token and key patterns (GitHub, AWS, Slack, Cloudflare-style 40 character tokens next to the word token/key/secret, private key blocks)
  paths      a user's home path (/Users/<name>, C:\\Users\\<name>, /home/<name> other than the build machine's generic one) and LAN addresses: reported as WARN (the
             checklist section 5 asks for them to be looked at), counted, not a failure unless --strict-paths
  size       a file over 20 MB (game data and disc images are never in the tree)
Exit 0 pass, 1 a check failed, 2 unusable input.
"""
import argparse, os, re, subprocess, sys
from collections import Counter

GENERIC_NAMES = {"build", "claude", "codex", "cboc-vita", "vitalink", "rev-claims", "root", "unknown"}
GENERIC_EMAIL = re.compile(r"(@local$|@localhost(\.invalid)?$|^noreply@anthropic\.com$|@users\.noreply\.github\.com$|^[a-z]@x$|^x@x$|^noreply@|\.invalid$)", re.I)
OWNER_NOREPLY = re.compile(r"^\d+\+(.+)@users\.noreply\.github\.com$", re.I)
LINKS = re.compile(rb"claude" rb"\.ai/(code|chat|artifact|share|project)|session_01[0-9A-Za-z]{16,}|cse_01[0-9A-Za-z]{16,}", re.I)
SECRETS = [re.compile(p) for p in (
    rb"ghp_[A-Za-z0-9]{30,}", rb"github_pat_[A-Za-z0-9_]{30,}", rb"gho_[A-Za-z0-9]{30,}", rb"ghs_[A-Za-z0-9]{30,}", rb"AKIA[0-9A-Z]{16}", rb"xox[abpr]-[A-Za-z0-9-]{10,}",
    rb"sk-[A-Za-z0-9]{32,}", rb"-----BEGIN (?:RSA |EC |OPENSSH |DSA |ENCRYPTED )?PRIVATE KEY-----", rb"AIza[0-9A-Za-z_-]{35}",
    rb"(?i)\b(?:api[_-]?key|secret|token|passwd|password|auth[_-]?token)\b[\"']?\s*[:=]\s*[\"'][A-Za-z0-9_\-+/=]{24,}[\"']")]
HOME = re.compile(rb"/Users/(?!Shared\b|username\b|user\b|name\b|you\b|me\b|<)[A-Za-z][A-Za-z0-9._-]+|C:\\Users\\(?!Public\b|username\b|user\b|name\b|you\b)[A-Za-z][A-Za-z0-9._ -]+|C:/Users/(?!Public\b|username\b|user\b|name\b|you\b)[A-Za-z][A-Za-z0-9._-]+|/home/(?!claude\b|user\b|username\b|name\b|you\b|runner\b|vita\b|build\b|me\b|x\b|\$)[a-z][a-z0-9._-]{2,}/")
FIXTURE = re.compile(rb"(?i)test|example|dummy|fake|sample|placeholder|0123456789|x{6,}")   # obvious fixtures, not secrets
LAN = re.compile(rb"\b(?:192\.168\.\d{1,3}\.\d{1,3}|10\.\d{1,3}\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})\b")
PRIVATE_DIR = re.compile(r"(^|/)(private|\.private|secrets?|vita conversion|\.secrets)(/|$)", re.I)
PRIVATE_FILE = re.compile(r"(\.pem|\.p12|\.pfx|\.key|\.jks|\.keystore|^id_rsa|^id_ed25519|\.dev\.vars)$|(^|/)\.env(\.|$)", re.I)
BIG = 20 * 1024 * 1024


def git(repo, *a):
    return subprocess.run(["git", "-C", repo, *a], capture_output=True, text=True, check=True).stdout


def private_tokens(repo):
    seen = Counter()
    for line in git(repo, "log", "--all", "--format=%an|%ae|%cn|%ce").splitlines():
        for i in range(0, 4, 2):
            p = line.split("|")
            if len(p) == 4: seen[(p[i].strip(), p[i + 1].strip())] += 1
    toks, idents = set(), 0
    for (name, email) in seen:
        n, e = name.lower(), email.lower()
        if n in GENERIC_NAMES and (not e or GENERIC_EMAIL.search(e)): continue
        if GENERIC_EMAIL.search(e) and n in GENERIC_NAMES: continue
        m = OWNER_NOREPLY.match(e)
        if m and m.group(1).lower() in GENERIC_NAMES: continue
        idents += 1
        if n not in GENERIC_NAMES:
            toks.add(n)
            for w in re.split(r"[^a-z0-9]+", n):
                if len(w) >= 4: toks.add(w)
        if e and not GENERIC_EMAIL.search(e):
            toks.add(e)
            loc, _, dom = e.partition("@")
            if len(loc) >= 4: toks.add(loc)
            for w in re.split(r"[^a-z0-9]+", dom):
                if len(w) >= 5 and w not in ("local", "localhost", "invalid", "github", "users", "noreply", "gmail", "icloud", "outlook", "hotmail", "yahoo", "proton", "protonmail", "home", "lan"): toks.add(w)
    return toks, idents


def files(tree):
    for d, dirs, fs in os.walk(tree):
        dirs[:] = [x for x in dirs if x != ".git"]
        for f in fs:
            p = os.path.join(d, f)
            if os.path.islink(p): continue
            yield p, os.path.relpath(p, tree)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree", required=True); ap.add_argument("--repo", required=True); ap.add_argument("--strict-paths", action="store_true")
    ap.add_argument("--max-list", type=int, default=15)
    a = ap.parse_args()
    if not os.path.isdir(a.tree): print("no such tree"); return 2
    try: toks, idents = private_tokens(a.repo)
    except Exception as e: print(f"cannot read the history of {a.repo}: {e}"); return 2
    toks = {t for t in toks if t}
    long_toks = {t.encode() for t in toks if len(t) >= 6}
    all_toks = {t.encode() for t in toks}
    hits = {k: {} for k in ("identity", "links", "secrets", "paths", "lan")}
    bad_priv, big, nfiles = [], [], 0
    for p, rel in files(a.tree):
        nfiles += 1
        if PRIVATE_DIR.search(rel) and "include/private/" not in rel: bad_priv.append(rel)
        elif PRIVATE_FILE.search(rel): bad_priv.append(rel)
        try: size = os.path.getsize(p)
        except OSError: continue
        if size > BIG: big.append(f"{rel} ({size >> 20} MB)")
        try: data = open(p, "rb").read()
        except OSError: continue
        low = data.lower()
        binary = b"\0" in data[:8192]
        n = sum(low.count(t) for t in (long_toks if binary else all_toks))
        if n: hits["identity"][rel] = n
        n = len(LINKS.findall(data))
        if n: hits["links"][rel] = n
        n = sum(1 for s in SECRETS for m in s.finditer(data) if not FIXTURE.search(m.group(0))) if not binary else 0
        if n: hits["secrets"][rel] = n
        if not binary:
            n = len(HOME.findall(data))
            if n: hits["paths"][rel] = n
            n = len(LAN.findall(data))
            if n: hits["lan"][rel] = n
    failed = 0
    def report(name, d, fail=True, what=""):
        nonlocal failed
        tot = sum(d.values())
        if not d: print(f"{name}: ok ({what} 0 matches)"); return
        print(f"{'FAIL' if fail else 'WARN'} {name}: {tot} matches in {len(d)} files {what}")
        for rel in sorted(d)[:a.max_list]: print(f"    {rel}  ({d[rel]})")
        if len(d) > a.max_list: print(f"    ... {len(d) - a.max_list} more files")
        if fail: failed += 1
    print(f"public_scan: {nfiles} files in the tree; {idents} private identit{'y' if idents == 1 else 'ies'} in the history, {len(toks)} search tokens (not printed)")
    if idents == 0: print("WARN identity: no private identity found in the history: check that --repo is the full history (git log --all)")
    report("identity", hits["identity"], what="(names, addresses, machine names of the history's private authors)")
    report("links", hits["links"], what="(claude.ai / session links)")
    if bad_priv: print(f"FAIL private: {len(bad_priv)} files in a private folder or key / certificate / .env files"); [print("    " + x) for x in sorted(bad_priv)[:a.max_list]]; failed += 1
    else: print("private: ok (no private / secrets / Vita Conversion folder, no key, certificate or .env file)")
    report("secrets", hits["secrets"], what="(token / key patterns)")
    if big: print(f"FAIL size: {len(big)} files over 20 MB"); [print("    " + x) for x in big]; failed += 1
    else: print("size: ok (no file over 20 MB)")
    report("paths", hits["paths"], fail=a.strict_paths, what="(a user's home path)")
    report("lan", hits["lan"], fail=a.strict_paths, what="(LAN addresses)")
    print("public_scan: " + ("FAILED" if failed else "passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
