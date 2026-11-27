#!/usr/bin/env python3
"""Checker for the multiplayer word tables (MpMessages.cpp, MpMessagesExtra.cpp). menus5: a suite (tools/allsuites.sh: `suite words`).

Parses the C++ initializer lists with a small tokenizer (string literals, identifiers,
punctuation, comments, #define macros), so every entry is read by structure, not by regex.
Fails (exit 1) on: a parse error, a duplicate or unlisted id, a missing / empty / English-identical translation (except the names
and loan words listed below), a placeholder set that differs between languages, a line count that differs by more than one, a Wii control
word, a banned word in English, a double space, a space before a newline, and a text whose LAST line lacks final punctuation where the
other languages' last line has it. (Mid-text line breaks are not sentence ends.)
Lines over 44 characters are only listed: the real limit is the box that draws each id, measured in tests/mp/ui_menu/boxfit_test.cpp.
"""
import re
import sys

import os
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "decomp")
# same in every language on purpose: names, loan words
SAME_AS_EN_OK = {"MP_T_CHAR_LUIGI", "MP_T_CHAR_MARIO", "MP_T_CHAR_TOAD", "MP_T_INTERNET", "MP_T_OFFLINE", "MP_T_ONLINE", "MP_T_SP_FILE", "MP_T_STAMPS", "MP_T_TEAM_TOTAL",
    # native Settings: the disc's language names in their own language, loan words, the button letters
    "MP_T_SET_LANG_ENGLISH", "MP_T_SET_LANG_FRENCH", "MP_T_SET_LANG_SPANISH", "MP_T_SET_LANG_GERMAN", "MP_T_SET_LANG_ITALIAN", "MP_T_SET_L_CAMERA", "MP_T_SET_V_CAMERA", "MP_T_SET_V_NORMAL", "MP_T_SET_M_PAUSE",
    "MP_T_SET_V_CONSOLE", "MP_T_SET_V_CONTROLLER", "MP_T_SET_B_R", "MP_T_SET_B_L2", "MP_T_SET_B_R2", "MP_T_SET_B_L3", "MP_T_SET_B_R3", "MP_T_SET_B_L", "MP_T_SET_B_SELECT", "MP_T_SET_B_START", "MP_T_SET_B_TRIANGLE"}
# the star pointer is a feature of the game, not a Wii control
POINTER_OK = {"MP_T_DLG_HOWTO", "MP_T_SET_D_CONTROLS", "MP_T_SET_D_POINTER_STEADY", "MP_T_SET_D_POINTER_QUICK", "MP_T_SET_L_POINTER", "MP_T_SET_MD_AIM",
              "MP_T_SET_D_REAR_ON", "MP_T_SET_D_RSTICK_POINTER", "MP_T_SET_D_TAPJUMP_OFF", "MP_T_SET_V_POINTER"}   # (native Settings: the Pointer pill and its sentences)
# header markers, not texts
MARKER_IDS = {"MP_T_DLG_FIRST", "MP_T_TOAST_LAST"}
MAIN = ROOT + "/src/Game/MP/MpMessages.cpp"
EXTRA = ROOT + "/src/Game/MP/MpMessagesExtra.cpp"
HDR = ROOT + "/include/Game/MP/MpMessages.hpp"
LANGS = ["EN", "FR", "ES", "DE", "IT"]
BANNED = [r"\bHost\b", r"\bJoin\b", r"\bLobby\b", r"\bServer\b", r"\bSession\b", r"\bNAT\b", r"\bRoom Settings\b"]
WII = [r"\bWii\b", r"\bRemote\b", r"\bNunchuk\b", r"\bA Button\b", r"\bB Button\b", r"\+ Button", r"(?<!\w)- Button", r"\bpointer\b"]
ESC = {"n": "\n", "t": "\t", "r": "\r", "0": "\0", "\\": "\\", '"': '"', "'": "'", "a": "\a", "b": "\b", "f": "\f", "v": "\v", "?": "?"}


def decode(body):
    out, i = [], 0
    while i < len(body):
        c = body[i]
        if c != "\\":
            out.append(c)
            i += 1
            continue
        n = body[i + 1]
        if n == "x":
            j = i + 2
            while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(chr(int(body[i + 2:j], 16)))
            i = j
        elif n in "uU":
            w = 4 if n == "u" else 8
            out.append(chr(int(body[i + 2:i + 2 + w], 16)))
            i += 2 + w
        elif n in "01234567":
            j = i + 1
            while j < len(body) and j < i + 4 and body[j] in "01234567":
                j += 1
            out.append(chr(int(body[i + 1:j], 8)))
            i = j
        else:
            out.append(ESC[n])
            i += 2
    return "".join(out)


TOK = re.compile(r"""
    (?P<ws>\s+)
  | (?P<lc>//[^\n]*)
  | (?P<bc>/\*.*?\*/)
  | (?P<str>(?:u8|u|U|L)?"(?:[^"\\\n]|\\.)*")
  | (?P<chr>'(?:[^'\\\n]|\\.)*')
  | (?P<id>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<num>[0-9][0-9A-Za-z_.]*)
  | (?P<p>.)
""", re.VERBOSE | re.DOTALL)


def tokenize(text):
    toks = []
    for m in TOK.finditer(text):
        k = m.lastgroup
        if k in ("ws", "lc", "bc"):
            continue
        v = m.group(k)
        if k == "str":
            body = v[v.index('"') + 1:-1]
            toks.append(("str", decode(body)))
        elif k == "chr":
            toks.append(("chr", v))
        else:
            toks.append((k, v))
    return toks


def load(path):
    src = open(path, encoding="utf-8").read()
    macros = {}
    kept = []
    for line in src.split("\n"):
        s = line.lstrip()
        if s.startswith("#"):
            m = re.match(r"#\s*define\s+(\w+)\s+(.*)$", s)
            if m:
                macros[m.group(1)] = tokenize(m.group(2))
            continue
        kept.append(line)
    return tokenize("\n".join(kept)), macros


def expand_macro_value(name, macros, depth=0):
    if depth > 5:
        raise ValueError("macro recursion " + name)
    out = []
    for k, v in macros[name]:
        if k == "str":
            out.append(v)
        elif k == "id" and v in macros:
            out.append(expand_macro_value(v, macros, depth + 1))
        else:
            raise ValueError("unsupported token in macro %s: %r" % (name, v))
    return "".join(out)


class Parser:
    def __init__(self, toks, macros):
        self.t = toks
        self.m = macros
        self.i = 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else ("eof", "")

    def expect(self, v):
        k, x = self.peek()
        if x != v:
            raise ValueError("expected %r got %r at token %d" % (v, x, self.i))
        self.i += 1

    def value(self):
        k, v = self.peek()
        if v == "{":
            return self.braced()
        parts = []
        while True:
            k, v = self.peek()
            if v in (",", "}", "{") or k == "eof":
                break
            self.i += 1
            if k == "str":
                parts.append(("s", v))
            elif k == "id":
                if v in ("nullptr", "NULL"):
                    parts.append(("null", None))
                elif v.startswith("MP_T_"):
                    parts.append(("id", v))
                elif v in self.m:
                    parts.append(("s", expand_macro_value(v, self.m)))
                else:
                    raise ValueError("unknown identifier %r at token %d" % (v, self.i))
            else:
                raise ValueError("unexpected token %r at %d" % (v, self.i))
        if not parts:
            raise ValueError("empty value at token %d" % self.i)
        if len(parts) == 1:
            return parts[0][1] if parts[0][0] != "null" else None
        if any(p[0] in ("id", "null") for p in parts):
            raise ValueError("bad concatenation at token %d" % self.i)
        return "".join(p[1] for p in parts)

    def braced(self):
        self.expect("{")
        items = []
        while True:
            k, v = self.peek()
            if v == "}":
                self.i += 1
                return items
            items.append(self.value())
            k, v = self.peek()
            if v == ",":
                self.i += 1
            elif v != "}":
                raise ValueError("expected , or } at token %d, got %r" % (self.i, v))


def find_table(toks, name):
    for idx in range(len(toks) - 3):
        if toks[idx] == ("id", name) and toks[idx + 1] == ("p", "[") and toks[idx + 2] == ("p", "]"):
            j = idx + 3
            while toks[j] != ("p", "{"):
                j += 1
            return j
    raise ValueError("table %s not found" % name)


def parse_table(path, name):
    toks, macros = load(path)
    p = Parser(toks, macros)
    p.i = find_table(toks, name)
    rows = p.braced()
    rest = toks[p.i:]
    leftover = [t for t in toks[p.i:] if t[1] == "{"]  # sanity only
    return rows, macros


def parse_enum(path):
    toks, _ = load(path)
    s = toks.index(("id", "MpText"))
    while toks[s] != ("p", "{"):
        s += 1
    names, depth, i, expect_name = [], 0, s, True
    i = s + 1
    while True:
        k, v = toks[i]
        if v == "}" and depth == 0:
            break
        if depth == 0 and expect_name and k == "id":
            names.append(v)
            expect_name = False
        elif v in "([{":
            depth += 1
        elif v in ")]}":
            depth -= 1
        elif v == "," and depth == 0:
            expect_name = True
        i += 1
    return names


def esc_show(s):
    if s is None:
        return "(no text)"
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\n":
            out.append("\\n")
        elif o == 1:
            out.append("<S1>")
        elif o == 2:
            out.append("<S2>")
        elif o == 3:
            out.append("<N1>")
        elif o == 4:
            out.append("<TM>")
        elif 0xE000 <= o <= 0xF8FF:
            out.append("<PIC%d>" % (o - 0xE000))
        else:
            out.append(ch)
    return "".join(out)


def placeholders(s):
    found = set()
    for ch in s:
        o = ord(ch)
        if o == 1:
            found.add("S1")
        elif o == 2:
            found.add("S2")
        elif o == 3:
            found.add("N1")
        elif o == 4:
            found.add("TM")
        elif 0xE000 <= o <= 0xF8FF:
            found.add("PIC%d" % (o - 0xE000))
    found.update(re.findall(r"\{[A-Za-z0-9_]*\}", s))
    found.update(re.findall(r"%[-+ #0]*\d*(?:\.\d+)?[dsuxXfcoleEgGi%]", s))
    found.update(re.findall(r"\$\d+", s))
    return found


def main():
    main_rows, _ = parse_table(MAIN, "kEntries")
    extra_rows, _ = parse_table(EXTRA, "kExtra")
    enum_names = parse_enum(HDR)

    rows = {}
    seen_main, seen_extra = {}, {}
    errors = []
    for item in main_rows:
        if not (isinstance(item, list) and len(item) == 3 and isinstance(item[0], str) and isinstance(item[2], list)):
            errors.append("bad kEntries item: %r" % (item,))
            continue
        mid, group = item[0], item[2]
        seen_main[mid] = seen_main.get(mid, 0) + 1
        if len(group) > 3:
            errors.append("%s has %d language texts in kEntries" % (mid, len(group)))
        r = rows.setdefault(mid, {l: None for l in LANGS})
        for lang, t in zip(["EN", "FR", "ES"], group + [None] * (3 - len(group))):
            r[lang] = t
        r["_main"] = True
    for item in extra_rows:
        if not (isinstance(item, list) and len(item) == 3 and isinstance(item[0], str)):
            errors.append("bad kExtra item: %r" % (item,))
            continue
        eid = item[0]
        seen_extra[eid] = seen_extra.get(eid, 0) + 1
        r = rows.setdefault(eid, {l: None for l in LANGS})
        r["DE"] = item[1]
        r["IT"] = item[2]
        r["_extra"] = True

    print("PARSE")
    print("  kEntries items parsed: %d (distinct ids %d)" % (len(main_rows), len(seen_main)))
    print("  kExtra items parsed:   %d (distinct ids %d)" % (len(extra_rows), len(seen_extra)))
    print("  header enum MpText names (excl. MP_T_NONE, MP_T_COUNT): %d" % len([n for n in enum_names if n not in ("MP_T_NONE", "MP_T_COUNT")]))
    print("  parse errors: %d" % len(errors))
    for e in errors:
        print("   ", e)
    dup_main = {k: v for k, v in seen_main.items() if v > 1}
    dup_extra = {k: v for k, v in seen_extra.items() if v > 1}
    print("  duplicate ids in kEntries: %s" % (dup_main or "none"))
    print("  duplicate ids in kExtra: %s" % (dup_extra or "none"))
    enum_set = set(n for n in enum_names if n not in ("MP_T_NONE", "MP_T_COUNT"))
    table_set = set(rows)
    print("  enum ids with no row in either table: %s" % (sorted(enum_set - table_set) or "none"))
    print("  table ids not in enum: %s" % (sorted(table_set - enum_set) or "none"))
    print("  kExtra ids with no kEntries row (no English): %s" % (sorted(k for k in seen_extra if k not in seen_main) or "none"))
    print("  ids with an EN text: %d" % sum(1 for r in rows.values() if r["EN"]))

    def text_of(r, lang):
        t = r[lang]
        return t if t else None  # the game falls back to English for missing or empty

    C = {k: [] for k in range(1, 8)}
    lines_by_lang = {l: 0 for l in LANGS}
    ids = sorted(rows)

    for mid in ids:
        r = rows[mid]
        en = r["EN"]
        en_letters = sum(ch.isalpha() for ch in en) if en else 0
        # check 1: missing / empty / identical to English
        for lang in LANGS:
            t = r[lang]
            if lang == "EN":
                continue
            if t is None:
                C[1].append((mid, lang, "(no row: game falls back to English)"))
            elif t.strip() == "":
                C[1].append((mid, lang, "(empty string)"))
            elif en and t == en and en_letters > 3 and mid not in SAME_AS_EN_OK:
                C[1].append((mid, lang, esc_show(t) + "  [EN identical]"))
        # check 2: placeholder sets
        present = {l: r[l] for l in LANGS if r[l]}
        sets = {l: placeholders(t) for l, t in present.items()}
        if len(set(frozenset(s) for s in sets.values())) > 1:
            for l in present:
                C[2].append((mid, l, esc_show(present[l]) + "  [set " + ",".join(sorted(sets[l])) + "]"))
        # check 3: lines > 44 chars
        for l in present:
            for ln in present[l].split("\n"):
                if len(ln) > 44:
                    C[3].append((mid, l, ln, len(ln)))
        # check 4: line count spread
        if present:
            counts = {l: t.count("\n") + 1 for l, t in present.items()}
            if max(counts.values()) - min(counts.values()) > 1:
                C[4].append((mid, counts))
        # check 5: Wii control words
        for l, t in present.items():
            for pat in WII:
                if pat == r"\bpointer\b" and mid in POINTER_OK:
                    continue
                # ("A Button" / "B Button" are Wii names, capitalised: "Choose a button" is plain English)
                if re.search(pat, t, 0 if pat in (r"\bA Button\b", r"\bB Button\b") else re.IGNORECASE):
                    C[5].append((mid, l, esc_show(t) + "  [" + pat + "]"))
                    break
        # check 6: banned English words in EN
        if en:
            for pat in BANNED:
                m = re.search(pat, en, re.IGNORECASE)
                if m:
                    C[6].append((mid, "EN", esc_show(en) + "  [" + m.group(0) + "]"))
        # check 7: double spaces, space before newline, missing final punctuation
        for l, t in present.items():
            if "  " in t:
                C[7].append((mid, l, "double space: " + esc_show(t)))
            if re.search(r" \n", t):
                C[7].append((mid, l, "space before newline: " + esc_show(t)))
        def ends_punct(s):
            s = s.rstrip(" ").rstrip(")]\"'”’»")
            return s[-1:] in (".", "?", "!", "…") if s else False
        lasts = {l: t.split("\n")[-1] for l, t in present.items()}
        for l, ln in lasts.items():
            if len(present[l].split()) > 4 and not ends_punct(ln):
                if any(ends_punct(o) for lo, o in lasts.items() if lo != l):
                    C[7].append((mid, l, "no final punctuation: " + esc_show(ln)))
        # per-language totals
    print()
    print("COUNTS")
    print("  check 1 (missing / empty / identical to EN, EN > 3 letters): %d" % len(C[1]))
    print("  check 2 (placeholder set differs across languages): ids %d, rows %d" % (len(set(x[0] for x in C[2])), len(C[2])))
    print("  check 3 (lines > 44 chars): %d lines" % len(C[3]))
    for l in LANGS:
        print("     %s: %d" % (l, sum(1 for x in C[3] if x[1] == l)))
    print("  check 4 (newline count differs by more than one): %d ids" % len(C[4]))
    print("  check 5 (Wii control words): %d rows" % len(C[5]))
    print("  check 6 (banned words in EN): %d rows" % len(C[6]))
    print("  check 7 (double space / space before newline / missing final punctuation): %d" % len(C[7]))
    c7 = {"double space": 0, "space before newline": 0, "no final punctuation": 0}
    for x in C[7]:
        for k in c7:
            if x[2].startswith(k):
                c7[k] += 1
    for k, v in c7.items():
        print("     %s: %d" % (k, v))

    print()
    print("CHECK 1 (%d)" % len(C[1]))
    for mid, l, t in C[1]:
        print("%s | %s | %s" % (mid, l, esc_show(t) if not t.startswith("(") else t))
    print()
    print("CHECK 2 (%d)" % len(C[2]))
    for mid, l, t in C[2]:
        print("%s | %s | %s" % (mid, l, t))
    print()
    print("CHECK 5 (%d)" % len(C[5]))
    for mid, l, t in C[5]:
        print("%s | %s | %s" % (mid, l, t))
    print()
    print("CHECK 6 (%d)" % len(C[6]))
    for mid, l, t in C[6]:
        print("%s | %s | %s" % (mid, l, t))
    print()
    print("CHECK 3 top 30 by length (of %d lines)" % len(C[3]))
    for mid, l, ln, n in sorted(C[3], key=lambda x: -x[3])[:30]:
        print("%s | %s | %s  (len %d)" % (mid, l, esc_show(ln), n))
    print()
    print("CHECK 4 (ids, newline counts per language)")
    for mid, counts in C[4]:
        print("  %s %s" % (mid, counts))
    print()
    print("CHECK 7 detail")
    for mid, l, t in C[7]:
        print("  %s | %s | %s" % (mid, l, t))

    problems = []
    if errors:
        problems.append("%d parse errors" % len(errors))
    if dup_main or dup_extra:
        problems.append("duplicate ids")
    if (enum_set - table_set) - MARKER_IDS or table_set - enum_set:
        problems.append("ids missing from a table or the enum: %s" % sorted(((enum_set - table_set) - MARKER_IDS) | (table_set - enum_set)))
    if [k for k in seen_extra if k not in seen_main]:
        problems.append("ids with no English")
    for k, what in ((1, "missing / empty / English-identical translations"), (2, "placeholder sets that differ"), (4, "line counts that differ by more than one"),
                    (5, "Wii control words"), (6, "banned words"), (7, "double spaces / missing final punctuation")):
        if C[k]:
            problems.append("%d %s" % (len(C[k]), what))
    print()
    if problems:
        print("WORDS CHECK FAIL: " + "; ".join(problems))
        return 1
    print("WORDS CHECK PASS (%d ids x 5 languages)" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
