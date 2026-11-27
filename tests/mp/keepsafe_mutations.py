#!/usr/bin/env python3
# keepfix 3: "a guarantee undone must fail a test". Each mutation puts ONE guarantee of the keepsafe design back to the unsafe behaviour in a copy
# of the source, builds the test program against that copy (the other objects come from the default build, tests/mp/keepsafe_build.sh) and runs
# the named test: it must FAIL. The unmutated build must pass the same tests first.
#   kind 'unit': keepsafe_test (ASan + UBSan), KS_ONLY=<test>;  'cut': keepsafe_cut_test (the power-cut model), KS_CUT_ONLY=<operation>;
#   'cfg': keepsafe_cfg_test under ThreadSanitizer (the settings lock against the helper thread).
# Run from the repository root, detached, one at a time (about 40 compiles):   python3 tests/mp/keepsafe_mutations.py [name-part]
import os, subprocess, sys, shutil, tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
def _mp_tmp():
    """MP_TMP of tests/mp/tmproot.sh: a scratch folder private to this tree (its name + a hash of its path)."""
    import hashlib
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    return os.environ.get('MP_TMP') or '/tmp/mp-%s-%s' % (os.path.basename(root), hashlib.md5(root.encode()).hexdigest()[:6])
OUT = os.environ.get('KS_OUT', os.path.join(_mp_tmp(), 'keepsafe_test'))   # the default build's output; its objects are in $OUT.d/{san,plain}
INC = ['-Iplatform/include', '-Iplatform/src/mp', '-Idecomp/include']
SAN = ['-std=c++20', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-O0', '-w']
PLAIN = ['-std=c++20', '-O1', '-w']
KS = 'platform/src/mp/mp_keepsafe.cpp'
BOOT = 'platform/src/mp/mp_boot.cpp'
CFG = 'platform/src/mp/mp_config.cpp'
POSIX = 'platform/src/mp/mp_posix_io.h'   # a header: its mutation is compiled through mp_config.cpp, which includes it
FLOW = 'platform/src/mp/mp_ui_flow.cpp'
MOD = ['platform/src/mp/mp_keepsafe.cpp', 'platform/src/mp/mp_savefile.cpp', 'platform/src/mp/mp_savecheck.cpp']
REAL = ['platform/src/mp/mp_config.cpp', 'platform/src/mp/mp_ui_flow.cpp', 'platform/src/mp/mp_core_sys.cpp', 'platform/src/mp/mp_core_vita.cpp']
UNIT_SRCS = ['tests/mp/keepsafe_test.cpp', BOOT] + MOD + REAL
CUT_SRCS = ['tests/mp/keepsafe_cut_test.cpp'] + MOD + REAL

# (guarantee undone, file, [(old text, new text), ...], kind, test)
MUT = [
    # ---- 1. copies
    ('a version copy holds the recovery checkpoints', KS, [('r == "mp/backups" || r == "mp/recovery" || Skipped(n)', 'r == "mp/backups" || Skipped(n)'), ('        } else if (!KnownFile(n, false)) {', '        } else if (false) {')], 'unit', 'Boot'),
    ('a version copy holds the single-player safety copies (mp/backups)', KS, [('r == "mp/backups" || r == "mp/recovery" || Skipped(n)', 'r == "mp/recovery" || Skipped(n)'), ('        } else if (!KnownFile(n, false)) {', '        } else if (false) {')], 'unit', 'Boot'),
    ('a copy holds the old design\'s .kstmp / .ksold files', KS, [('if (k > 6 && (n.compare(k - 6, 6, ".kstmp") == 0 || n.compare(k - 6, 6, ".ksold") == 0)) return true;', ''), ('        } else if (!KnownFile(n, false)) {', '        } else if (false) {')], 'unit', 'OldDesign'),
    ('the earlier design\'s undo snapshot folders (class 1) are read and pruned', KS, [('if (m[8] != 0 || n > 4000) return false;', 'if (m[8] > 1 || n > 4000) return false;')], 'unit', 'OldDesign'),
    ('a start with an unreadable stamp decides anyway', KS, [('    if (st.unreadable) {   // there but cannot be read now', '    if (false) {   // there but cannot be read now')], 'unit', 'CopyFailures'),
    ('a failed copy still writes the stamp (never retried)', KS, [('    if (rc == MP_KS_OK || rc == MP_KS_NOTHING) {\n        Stamp ns = st;', '    if (true) {\n        Stamp ns = st;')], 'unit', 'CopyFailures'),
    ('a copy is written over an existing name (mkdir EEXIST accepted)', KS, [('        if (mkdir((CopiesDir() + "/" + n).c_str(), 0770) == 0) s.name = n;\n        else if (errno != EEXIST)', '        if (mkdir((CopiesDir() + "/" + n).c_str(), 0770) == 0 || errno == EEXIST) s.name = n;\n        else if (false)')], 'cut', 'clash'),
    ('an existing file is unlinked when a new copy\'s create fails', KS, [('    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0660);\n    if (fd < 0) return false;',
                                                                      '    const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0660);\n    if (fd < 0) { unlink(path.c_str()); return false; }'),
                                                                          ('        if (Exists(CopiesDir() + "/" + n)) continue;\n', '')], 'cut', 'clash'),
    ('a copy file is created over an existing name (no O_EXCL, no existence check)', KS, [('O_CREAT | O_EXCL | O_WRONLY, 0660)', 'O_CREAT | O_TRUNC | O_WRONLY, 0660)'),
                                                                                    ('        if (Exists(CopiesDir() + "/" + n)) continue;\n', '')], 'cut', 'clash'),
    ('prune removes a copy it cannot parse', KS, [('} else if (st == kMarkedUnfinished) {', '} else if (st == kMarkedUnfinished || st == kUnparseable) {')], 'unit', 'PruneAndPins'),
    ('a damaged copy is listed and counted as verified', KS, [('if (Read(s->dir + "/" + f.rel, &b) != 0 || b.size() != f.size || SumOf(*s, b) != f.sum) return kDamaged;', '(void)b;')], 'unit', 'BlocksAndDamage'),
    ('the first copy of a version change is not pinned', KS, [('if (pairs.insert(good[i].from + ">" + good[i].version).second) pinned.push_back(i);', 'if (pairs.insert(good[i].from + ">" + good[i].version).second && false) pinned.push_back(i);')], 'unit', 'PruneAndPins'),
    ('a pinned copy goes without a newer copy holding the same files', KS, [('            if (SameFiles(good[i], good[j])) {', '            if (true) {')], 'unit', 'PruneAndPins'),
    ('pinned copies are not bounded', KS, [('for (size_t k = 0; k + 8 < pinned.size(); k++) {', 'for (size_t k = 0; k + 8000 < pinned.size(); k++) {')], 'unit', 'PruneAndPins'),
    ('a start of the same version makes no stamp-free retry (version copy before the stamp: stamp written first)', KS,
     [('    const int rc = MakeVersionCopy(st.valid ? st.version.c_str() : "none", &name);', '    {\n        Stamp early = st;\n        early.version = s.version;\n        early.build = s.build;\n        WriteStamp(early);\n    }\n    const int rc = MakeVersionCopy(st.valid ? st.version.c_str() : "none", &name);')],
     'cut', 'version-copy'),
    ('removing a copy folder deletes its unfinished mark first', KS, [('if (n == kUnfinished) marked = true;', 'if (false) marked = true;')], 'cut', 'prune'),
    ('removing a copy folder without writing its mark', KS, [('    if (!Exists(s.dir + "/" + kUnfinished) && !WriteNew(s.dir + "/" + kUnfinished, Bytes())) return;\n', '')], 'cut', 'prune'),
    # ---- 2. bring back a Team File slot
    ('a slot in use is replaced without asking', KS, [('            if (!confirmed) return MP_KS_CONFIRM;\n            std::string before;', '            std::string before;')], 'unit', 'BringBackSlot'),
    ('no "before" copy of the file about to be replaced', KS, [('            if ((rc = WriteCopyFile("before", std::vector<Item>{Item{"mp/saves/GameData.bin", live}}, &before)) != 0) return rc;', '')], 'cut', 'slot-in-use'),
    ('the slot is replaced although its "before" copy could not be made', KS, [('std::vector<Item>{Item{"mp/saves/GameData.bin", live}}, &before)) != 0) return rc;', 'std::vector<Item>{Item{"mp/saves/GameData.bin", live}}, &before)) != 0) {}')], 'unit', 'BringBackSlot'),
    ('a blank slot of the copy replaces a slot', KS, [('    if (mp_save_slot_state(team->data.data(), slot) != 1) return MP_KS_NOTHING;   // a blank file never replaces anything', '')], 'unit', 'BringBackSlot'),
    ('the question does not say the live file has progress the copy lacks', KS, [('if (!SlotDominates(team->data.data(), live.data(), s)) info->slotLacks |= bit;', '')], 'unit', 'BringBackSlot'),
    ('the slot\'s third block (config) is not compared', KS, [('bool SlotDominates(const uint8_t* in, const uint8_t* cur, int slot) {\n    for (const char* k : {"mario", "luigi", "config"}) {', 'bool SlotDominates(const uint8_t* in, const uint8_t* cur, int slot) {\n    for (const char* k : {"mario", "luigi"}) {')], 'unit', 'BlocksAndDamage'),
    ('the Team File is written in place (not by the atomic writer)', KS, [('    if (!mp_save_write_atomic(P().teamFile, img.data(), (uint32_t)img.size())) return MP_KS_IO;',
                                                                      '    {\n        FILE* f = fopen(P().teamFile, "wb");\n        if (!f) return MP_KS_IO;\n        fwrite(img.data(), 1, img.size(), f);\n        fclose(f);\n    }')], 'cut', 'slot-empty'),
    ('the game\'s reload hook does not run after a slot is brought back', KS, [('    RunHooks();\n    Log("Team File %d brought back from %s", slot, name);', '    Log("Team File %d brought back from %s", slot, name);')], 'unit', 'BringBackSlot'),
    ('this module repairs (renames) a live Team File when it reads it', KS, [('int LiveTeam(Bytes* t) {\n    const int rc', 'int LiveTeam(Bytes* t) {\n    bool ch = false;\n    mp_save_recover(P().teamFile, &ch);\n    const int rc')], 'unit', 'ReadOnlyCalls'),
    ('the game\'s reload hook is not wired at start', BOOT, [('    if (mp_savegame_keepsafe_reload) mp_keepsafe_add_reload_hook(mp_savegame_keepsafe_reload);', '')], 'unit', 'BringBackSlot'),
    ('the game\'s guard is not wired at start', BOOT, [('    if (mp_savegame_keepsafe_may_replace) mp_keepsafe_set_guard(mp_savegame_keepsafe_may_replace);', '')], 'unit', 'BringBackSlot'),
    # ---- 2. the online account
    ('a copy\'s account is used without asking', KS, [('    if (!confirmed) return MP_KS_CONFIRM;\n    mp_save_mkdirs(P().onlineDir);', '    mp_save_mkdirs(P().onlineDir);')], 'unit', 'Account'),
    ('a foreign credential is rotated into .prev', KS, [('if (!listed || mine.count(Credential(cur))) {', 'if (true) {')], 'unit', 'Account'),
    ('the account is replaced without keeping the old credential', KS, [('            if (!kept) {   // (each a single atomic write', '            if (!kept && false) {   // (each a single atomic write')], 'cut', 'account-store'),
    ('the live account is written before the old credential is kept', KS, [('    if (rc == 0 && !cur.empty() && Credential(cur) != Credential(in)) {\n        bool listed = false;',
                                                                          '    if (!mp_save_write_atomic(path.c_str(), in.data(), (uint32_t)in.size())) return false;\n    if (rc == 0 && !cur.empty() && Credential(cur) != Credential(in)) {\n        bool listed = false;')], 'cut', 'account-store'),
    ('"use this copy\'s account" writes online.dat itself, outside the account store', KS, [('if (!StoreAccount(Abs(i->rel), i->data, false)) return MP_KS_IO;',
                                                                                          'if (!mp_save_write_atomic(Abs(i->rel).c_str(), i->data.data(), (uint32_t)i->data.size())) return MP_KS_IO;')], 'unit', 'Account'),
    ('the account service is not told after the account is brought back', KS, [('    if (mp_service_shutdown) mp_service_shutdown();\n', '')], 'unit', 'Account'),
    # ---- 2. settings
    ('settings come back from a copy made for sharing', KS, [('if (c->kind != MP_KS_COPY_MADE || i.rel.compare(0, 7, "online/") == 0 || i.rel == "mp/saves/GameData.bin") c->items.push_back(i);', 'c->items.push_back(i);'),
                                                           ('    if (c.kind == MP_KS_COPY_MADE) return MP_KS_NOTHING;   // never another console\'s settings\n', '')], 'unit', 'Settings'),
    ('the settings lock is taken after mp.cfg is read', KS, [('    mp_config_hold(true);   // the settings lock BEFORE anything is read or copied (a helper save waits)\n', ''),
                                                            ('    std::string before;\n    if (rc == 0 && want.empty()) rc = MP_KS_NOTHING;', '    mp_config_hold(true);\n    std::string before;\n    if (rc == 0 && want.empty()) rc = MP_KS_NOTHING;')], 'unit', 'Settings'),
    ('the settings lock is never taken', KS, [('    mp_config_hold(true);   // the settings lock BEFORE anything is read or copied (a helper save waits)\n', ''),
                                             ('    mp_config_hold(false);\n    if (beforeOut && cap) snprintf(beforeOut, cap, "%s", before.c_str());', '    if (beforeOut && cap) snprintf(beforeOut, cap, "%s", before.c_str());')], 'cfg', ''),
    ('the reload hooks run after the settings lock is released', KS, [('    if (!want.empty() && rc != MP_KS_NOTHING) RunHooks();   // (mp.cfg was loaded by its writer while the lock is held)\n    mp_config_hold(false);',
                                                                     '    mp_config_hold(false);\n    if (!want.empty() && rc != MP_KS_NOTHING) RunHooks();')], 'unit', 'Settings'),
    ('no "before" copy of the settings about to be replaced', KS, [('    if (rc == 0 && !old.empty()) rc = WriteCopyFile("before", old, &before);', '')], 'cut', 'settings'),
    ('mp.cfg brought back is not loaded into the running settings', CFG, [('    mp_log("config: replaced from a copy (keepsafe)");\n    return mp_config_load_held();', '    mp_log("config: replaced from a copy (keepsafe)");\n    return true;')], 'cfg', ''),
    # ---- keepfix 5 (the fourth outside review, share/reviews/keepsafe-4.md): one or more per finding
    ('1: an account file online_<8 hex>.dat is not recognised (18 characters, a 3-character ending)', KS, [('n.size() != 19 || n.compare(0, 7, "online_") != 0 || n.compare(15, 4, ".dat") != 0', 'n.size() != 18 || n.compare(0, 7, "online_") != 0 || n.compare(15, 3, ".dat") != 0')], 'unit', 'AccountNames'),
    ('3: a "before" copy beyond 16 is deleted although it holds something nothing else does', KS, [('if (!unique && unlink((CopiesDir() + "/" + before[i].name).c_str()) == 0) before[i].gone = true;', 'if (unlink((CopiesDir() + "/" + before[i].name).c_str()) == 0) before[i].gone = true;')], 'unit', 'BeforeCopies'),
    ('3: no limit of 64 "before" copies (the new bring-back is not refused)', KS, [('bool BeforeFull() { return Prune("") >= kBeforeCap; }', 'bool BeforeFull() { Prune(""); return false; }')], 'unit', 'BeforeCopies'),
    ('3: a slot held by the live file or a kept copy is still counted as unique (nothing beyond 16 ever goes)', KS, [('                    bool found = haveLive && HeldIn(*team, slot, live);\n                    for (size_t h = 0; h < held.size() && !found; h++) found = HeldIn(*team, slot, held[h]);', '                    bool found = false;')], 'unit', 'BeforeCopies'),
    ('4: Make a Copy applies the slot-structure check to the live file', KS, [('    const int ok = TeamFileOk(*t);   // (keepfix 5', '    int ok = TeamFileOk(*t);\n    for (int sl = 1; !ok && sl <= 6; sl++) if (!SlotStructOk(*t, sl)) ok = MP_KS_BAD_FILE;\n    // (keepfix 5')], 'unit', 'LiveStructure'),
    ('4: bringing back checks every slot of the result and of the copy (a damaged slot cannot be repaired)', KS, [('if (TeamFileOk(img) != 0 || !SlotStructOk(img, slot)) return MP_KS_BAD_FILE;', 'if (TeamFileOk(img) != 0) return MP_KS_BAD_FILE;\n    for (int sl = 1; sl <= 6; sl++) if (!SlotStructOk(img, sl)) return MP_KS_BAD_FILE;')], 'unit', 'LiveStructure'),
    # (4b, removed: the two SlotStructOk guards of bring_back_slot are belt and braces: mp_save_copy_slot refuses a damaged slot first, so no input reaches them; Make a Copy and the repair path are mutated above)
    ('5: a folder that is there but cannot be listed counts as empty (the copy lacks its files and is pinned)', KS, [('        if (IsDir(abs)) out->ok = false;   // (a folder', '        if (false) out->ok = false;   // (a folder')], 'unit', 'FailedListing'),
    ('5: an account folder that cannot be listed counts as empty', KS, [('    } else if (IsDir(P().onlineDir)) {\n        out.ok = false;\n    }', '    }')], 'unit', 'FailedListing'),
    ('5: an incomplete listing is no reason to fail the copy', KS, [('    if (!set.ok) {   // a folder that is there could not be listed: a copy of what was seen would lack files and still look complete', '    if (false) {')], 'unit', 'FailedListing'),
    ('6: mp.cfg that could not be read cannot be restored', CFG, [('(s_readOnly && s_lastVersion > 1) || !mp_sys_mkdirs(mp_config_dir())) return false;', 's_readOnly || !mp_sys_mkdirs(mp_config_dir())) return false;')], 'unit', 'ConfigRestore'),
    ('6: a newer game\'s mp.cfg is offered for replacement by a restore', KS, [('        if (r == 0 && TextVersion(cur) > 1) continue;   // a newer game\'s file here: left alone', '')], 'unit', 'ConfigRestore'),
    ('7: a stamp that is there but does not parse stops the automatic copy for ever', KS, [('    if (st.unreadable) {   // there but cannot be read now', '    if (st.unreadable || (st.present && !st.valid && !st.newer)) {   // there but cannot be read now')], 'unit', 'StampParse'),
    ('8: a .prev file that cannot be read is taken for "absent"', KS, [('            if (r != 0 && r != -ENOENT) return false;   // (a credential that cannot be read is not "absent": nothing is rotated over it)', '')], 'unit', 'AccountReads'),
    ('8: an own list that cannot be read is taken for "absent"', KS, [('    if (rc != 0 && rc != -ENOENT) return -1;\n    if (rc == 0 && OwnListOk', '    if (rc == 0 && OwnListOk')], 'unit', 'AccountReads'),
    ('8: an own list that fails its own check counts as a list (the credential replaced is not kept)', KS, [('    if (rc == 0 && OwnListOk(raw->data(), (uint32_t)raw->size())) {\n        *listed = true;', '    if (rc == 0) {\n        *listed = true;')], 'unit', 'AccountReads'),
    ('9: settings from a copy carry that copy\'s console_key and upnp_permanent', KS, [('        if (k == 0) i.data = OwnIdentityText(i.data);', '')], 'unit', 'Identity'),
    ('10: a version copy made after a cut in the save has no Team File (.mptmp / .mpbak not used)', KS, [('    if (!Find(*c, "mp/saves/GameData.bin")) {   // keepfix 5', '    if (false) {   // keepfix 5')], 'unit', 'CutSave'),
    ('10: an invalid .mptmp is taken (no check of the temporary)', KS, [('if (Read(s.dir + "/" + f.rel, &it.data) == 0 && TeamFileOk(it.data) == 0) {', 'if (Read(s.dir + "/" + f.rel, &it.data) == 0) {')], 'unit', 'CutSave'),
    ('11: a pinned copy beyond 8 goes although no newer copy holds the same files', KS, [('            if (SameFiles(good[i], good[j])) {\n                drop.insert(i);', '            if (true) {\n                drop.insert(i);')], 'unit', 'PinnedCount'),
    ('12: the next number counts every name in the folder (a stray 4294967295 saturates it)', KS, [('        if (OwnName(n, &v)) best = std::max(best, v);', '        { const size_t dash = n.find_last_of(\'-\'); if (dash != std::string::npos) best = std::max(best, (uint32_t)strtoul(n.c_str() + dash + 1, nullptr, 10)); (void)v; }')], 'unit', 'Ordering'),
    ('12: copies are ordered by the number in their name, not their recorded time', KS, [('    if (ca != cb) return ca > cb;\n    if (sa != sb) return sa > sb;', '    if (sa != sb) return sa > sb;')], 'unit', 'Ordering'),
    ('13: a file whose name is not one of the game\'s is copied (any size)', KS, [('        } else if (!KnownFile(n, false)) {', '        } else if (false) {')], 'unit', 'StrayFiles'),
    ('13: an unknown file in the account folder is copied', KS, [('            if (KnownFile(n, true)) out.rels.push_back("online/" + n);\n            else unknown++;', '            out.rels.push_back("online/" + n);')], 'unit', 'StrayFiles'),
    ('13: "no space" is reported for any failure to copy', KS, [('int IoResult() { return gErr == ENOSPC ? MP_KS_NO_SPACE : MP_KS_IO; }', 'int IoResult() { return MP_KS_NO_SPACE; }'),
                                                               ('        else if (errno != EEXIST) return errno == ENOSPC ? MP_KS_NO_SPACE : MP_KS_IO;', '        else if (errno != EEXIST) return MP_KS_NO_SPACE;')], 'unit', 'StrayFiles'),
    ('13: the "card is full" notice is used for every failure', KS, [('s.notice = rc == MP_KS_NO_SPACE ? MP_KS_NOTICE_NO_SPACE : MP_KS_NOTICE_NO_COPY;', 's.notice = MP_KS_NOTICE_NO_SPACE;')], 'unit', 'StrayFiles'),
    ('13: bringing back slot N with no Team File here writes the copy\'s other slots too', KS, [('                if (mp_save_find(img.data(), n, &off, &size)) memset(img.data() + off, 0, size);', '                (void)off; (void)size;')], 'unit', 'SlotOnly'),
    ('13: use_account runs during a session', KS, [('    if (!b.ok || (S().guard && !S().guard())) return MP_KS_BUSY;   // (keepfix 5: not during a session', '    if (!b.ok) return MP_KS_BUSY;   // (keepfix 5: not during a session')], 'unit', 'AccountGuards'),
    ('13: use_account does not wait for the save layer\'s async writer', KS, [('    mp_save_async_wait();\n    Copy c;\n    const int rc = LoadCopy(name, &c);\n    if (rc) return rc;\n    std::vector<const Item*> todo;', '    Copy c;\n    const int rc = LoadCopy(name, &c);\n    if (rc) return rc;\n    std::vector<const Item*> todo;')], 'unit', 'AccountGuards'),
    ('13: Make a Copy reads the account without the account lock', KS, [('        AcctLock al;   // (keepfix 5: the account store\'s two renames are never seen half done)\n', '')], 'unit', 'AccountLock'),
    ('14: the mp.cfg writer does not fsync its temporary before the rename', POSIX, [('    if (ok && fsync(fd) != 0) ok = false;', '')], 'unit', 'WritersSync'),
    ('14: the flow.cfg writer (now the save layer\'s atomic writer) does not fsync its temporary before the rename', 'platform/src/mp/mp_savefile.cpp', [('    ok = ok && fsync(fd) == 0;', '')], 'unit', 'WritersSync'),
    ('14: the restore writer for mp.cfg does not fsync its temporary before the rename', CFG, [('    written = fsync(fileno(f)) == 0 && written;', '')], 'unit', 'WritersSync'),
    ('removing a copy folder leaves no mark when a file could not be removed (disk full on an unlink)', KS, [('    if (!gone) return false;\n    if (marked && unlink', '    if (marked && unlink')], 'cut', 'prune'),
]


def sh(args, **kw):
    return subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def obj(kind, src):
    return os.path.join(OUT + '.d', kind, os.path.basename(src) + '.o')


def link_unit(objs, exe):
    return sh(['g++', '-fsanitize=address,undefined'] + objs + ['-pthread', '-Wl,--wrap=_Z14mp_config_holdb', '-o', exe])


def link_cut(objs, exe):
    return sh(['g++'] + objs + ['-pthread', '-ldl', '-o', exe])


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else None
    for k, srcs in (('san', UNIT_SRCS), ('plain', CUT_SRCS)):
        for s in srcs:
            if not os.path.exists(obj(k, s)):
                print('missing %s: run tests/mp/keepsafe_build.sh build first' % obj(k, s)); return 2
    tmp = tempfile.mkdtemp(prefix='ksmut')
    bad = 0
    # the unmutated build passes what the mutations are checked with
    env = dict(os.environ, KS_TMP=os.path.join(tmp, 'w0'))
    r = subprocess.run([OUT], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print('unmutated keepsafe_test: %s' % ('pass' if r.returncode == 0 else 'FAIL'))
    bad += r.returncode != 0
    r = subprocess.run([OUT + '.cut'], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    print('unmutated keepsafe_cut_test: %s' % ('pass' if r.returncode == 0 else 'FAIL'))
    bad += r.returncode != 0
    cfg_exe = None
    for (what, path, edits, kind, test) in MUT:
        if only and not any(o in what or o == test for o in only.split('|')): continue
        text = open(os.path.join(ROOT, path)).read()
        mutated = text
        ok = True
        for (old, new) in edits:
            if mutated.count(old) != 1:
                print('MUTATION NOT APPLICABLE (%d matches of %r): %s' % (mutated.count(old), old[:60], what)); ok = False; break
            mutated = mutated.replace(old, new)
        if not ok:
            bad += 1; continue
        mdir = os.path.join(tmp, 'src', os.path.dirname(path))
        os.makedirs(mdir, exist_ok=True)
        mpath = os.path.join(mdir, os.path.basename(path))
        open(mpath, 'w').write(mutated)
        target = path
        if path.endswith('.h'):   # a header: compiled through the file that includes it, copied beside the mutated header (a quoted include looks there first)
            target = CFG
            mpath = os.path.join(mdir, os.path.basename(CFG))
            shutil.copy(os.path.join(ROOT, CFG), mpath)
        mo = os.path.join(tmp, 'm.o')
        exe = os.path.join(tmp, 'mut')
        if kind == 'cfg':   # the thread-sanitized settings test, built from the mutated source (keepsafe or mp_config)
            srcs = ['tests/mp/keepsafe_cfg_test.cpp'] + MOD + ['platform/src/mp/mp_config.cpp', 'platform/src/mp/mp_core_sys.cpp', 'platform/src/mp/mp_core_vita.cpp']
            srcs = [mpath if s == path else s for s in srcs]
            r = sh(['g++', '-std=c++20', '-fsanitize=thread', '-O1', '-w'] + INC + ['-I' + os.path.join(ROOT, 'platform/src/mp')] + srcs + ['-pthread', '-o', exe])
            if r.returncode:
                print('MUTATION DOES NOT COMPILE: %s\n%s' % (what, r.stdout[-800:])); bad += 1; continue
            rr = subprocess.run([exe], env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1 exitcode=66'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)
            caught = rr.returncode != 0
            print('%-6s %-86s keepsafe_cfg_test (TSan)' % ('caught' if caught else 'MISSED', what))
            bad += not caught
            continue
        flags = SAN if kind == 'unit' else PLAIN
        r = sh(['g++'] + flags + INC + ['-I' + os.path.join(ROOT, os.path.dirname(path)), '-c', mpath, '-o', mo])
        if r.returncode:
            print('MUTATION DOES NOT COMPILE: %s\n%s' % (what, r.stdout[-800:])); bad += 1; continue
        srcs, k = (UNIT_SRCS, 'san') if kind == 'unit' else (CUT_SRCS, 'plain')
        objs = [mo if s == target else obj(k, s) for s in srcs]
        r = (link_unit if kind == 'unit' else link_cut)(objs, exe)
        if r.returncode:
            print(r.stdout); bad += 1; continue
        env = dict(os.environ, KS_TMP=os.path.join(tmp, 'w'))
        env['KS_ONLY' if kind == 'unit' else 'KS_CUT_ONLY'] = test
        rr = subprocess.run([exe], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=900)
        caught = rr.returncode != 0
        fails = [l for l in rr.stdout.splitlines() if l.startswith('FAIL') or l.startswith('VIOLATION')]
        print('%-6s %-86s %s %s (%s)' % ('caught' if caught else 'MISSED', what, kind, test, fails[0][:90] if fails else 'exit %d' % rr.returncode))
        bad += not caught
    # mp_config_load without its lock races with the helper thread's save (ThreadSanitizer)
    if not only or 'lock' in only:
        cfg = open(os.path.join(ROOT, CFG)).read()
        i = cfg.index('bool mp_config_load(void) {')
        j = cfg.index('MpLock l(CfgMu());', i)
        mdir = os.path.join(tmp, 'cfg'); os.makedirs(mdir, exist_ok=True)
        open(os.path.join(mdir, 'mp_config.cpp'), 'w').write(cfg[:j] + cfg[j + len('MpLock l(CfgMu());'):])
        exe = os.path.join(tmp, 'cfglock')
        r = sh(['g++', '-std=c++20', '-fsanitize=thread', '-O1', '-w'] + INC + ['tests/mp/config_lock_test.cpp', os.path.join(mdir, 'mp_config.cpp'),
               'platform/src/mp/mp_core_sys.cpp', 'platform/src/mp/mp_core_vita.cpp', '-pthread', '-o', exe])
        rr = subprocess.run([exe], env=dict(os.environ, TSAN_OPTIONS='halt_on_error=1 exitcode=66'), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        caught = r.returncode == 0 and rr.returncode != 0
        print('%-6s %-86s config_lock_test (TSan)' % ('caught' if caught else 'MISSED', 'mp_config_load without the settings lock'))
        bad += not caught
    shutil.rmtree(tmp, ignore_errors=True)
    print('keepsafe_mutations: %s' % ('every mutation caught' if not bad else '%d PROBLEMS' % bad))
    return 1 if bad else 0


sys.exit(main())
