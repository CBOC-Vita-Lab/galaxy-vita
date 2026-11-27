#!/usr/bin/env python3
# savefix: guard-removal (mutation) suite for the save-safety guards. For every guard: copy the save code and its tests to a
# scratch tree, remove the guard there (the worktree is never changed), build save_test / savegate_test / rev_backup_test and the
# savelife harness against the copy, run them, and report which test noticed. A guard no test notices fails the suite, except the
# ones listed as redundant (another guard in the same path makes them unobservable; their pair is mutated together).
#   python3 -I tests/mp/save_guard_mutations.py [--out DIR] [--random N] [name ...]      (CPU heavy: run through tools/buildrun.sh)
# Grew out of the reviewer's tests/mp/rev_guard_mutations.sh (its four mutations are the first four here).
import os, shutil, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SF = 'platform/src/mp/mp_savefile.cpp'
SG = 'decomp/src/Game/MP/MpSaveGame.cpp'
GATE = 'decomp/include/Game/MP/MpSaveGate.hpp'
HOOK = 'decomp/include/Game/MP/MpSaveHook.hpp'
CFG = 'platform/src/mp/mp_config.cpp'
NAND = 'platform/src/nand/nand.cpp'

# name, [(file, old, new)], what it guards, expected ('caught' | 'redundant')
MUTATIONS = [
    ('verify', [(SF, 'bool ok = mp_save_read_file(tmp, check, MP_SAVE_MAX_FILE, &got) == 0 && got == len && memcmp(check, want, len) == 0;',
                 'bool ok = true; (void)got;')], 'atomic write: the temporary read back and compared before the old file is touched', 'caught'),
    ('postverify', [(SF, 'if (readValid(dstPath, w.b, &len, false) != MP_SAVE_OP_OK || len != out || memcmp(w.a, w.b, out) != 0) return MP_SAVE_OP_VERIFY;', '')],
     'put_blocks: post-write verify (alone: redundant with the atomic writer\'s final read-back)', 'redundant'),
    ('spinvalid', [(SF, '''    rc = readValid(spPath, w.b, &spLen, false);
    if (rc != MP_SAVE_OP_OK) return rc;''', '    (void)spLen;')], 'write-back refuses a missing / invalid single-player file', 'caught'),
    ('backupprune', [(SF, 'if (!mp_save_backup(spPath, backupDir, unixTime, keep, backupOut, backupCap)) return MP_SAVE_OP_BACKUP;',
                      '(void)mp_save_backup(spPath, backupDir, unixTime, keep, backupOut, backupCap);')], 'write-back requires a backup', 'caught'),
    ('final_readback', [(SF, '''    ok = mp_save_read_file(path, check, MP_SAVE_MAX_FILE, &got) == 0 && got == len && memcmp(check, want, len) == 0;
    free(check);
    return ok;''', '''    free(check);
    return true;''')], 'atomic write: the file in place read back after the rename (alone: redundant with put_blocks\' verify)', 'redundant'),
    ('final_readback+postverify', [(SF, '''    ok = mp_save_read_file(path, check, MP_SAVE_MAX_FILE, &got) == 0 && got == len && memcmp(check, want, len) == 0;
    free(check);
    return ok;''', '''    free(check);
    return true;'''), (SF, 'if (readValid(dstPath, w.b, &len, false) != MP_SAVE_OP_OK || len != out || memcmp(w.a, w.b, out) != 0) return MP_SAVE_OP_VERIFY;', '')],
     'both read-backs after a write together', 'caught'),
    ('backup_date_order', [(SF, '''    if (x->seq != y->seq) return x->seq < y->seq ? -1 : 1;
    return strcmp(x->name, y->name);''', '''    return strcmp(x->name + 18, y->name + 18);   // (by date, as before)''')], 'F3: backups pruned by sequence, not by the clock', 'caught'),
    ('backup_skip_own', [(SF, '            if (strcmp(list[i].name, name) == 0) continue;\n', '')],
     'F3: the prune never removes the backup just written (unreachable: the new one has the largest sequence)', 'redundant'),
    ('backup_recheck', [(SF, '    ok = ok && mp_save_read_file(path, check, MP_SAVE_MAX_FILE, &got) == 0 && got == len && memcmp(check, buf, len) == 0;\n', '')],
     'F3: backup re-read before success (redundant with the atomic writer\'s own read-backs)', 'redundant'),
    ('hook_precheck', [(HOOK, 'inline bool suppressed() { return MpSaveGame::savesSuppressed(); }',
                        'inline bool suppressed() { return (mp_active() || MpSaveGame::heldAfterSession()) && MpSaveGame::savesSuppressed(); }'),
                       (HOOK, '#include "Game/MP/MpSaveGame.hpp"', '#include "Game/MP/MpSaveGame.hpp"\n#include "port/mp_bridge.h"')],
     'F2: the save hook asks the self-syncing gate with no pre-check', 'caught'),
    ('fs_guard', [(SG, '        if (cachedGen != sImageGen) return FS_REBUILD;\n        return FS_USABLE;', '        (void)cachedGen;\n        return FS_USABLE;'),
                  (SG, '            if (sUnsafe || sG.onMpRoot || sG.heldAfterSession) return FS_WAIT;\n', '')],
     'F1: the file select guard (hold / rebuild)', 'caught'),
    ('fs_backstop', [(SG, 'if (sFsOpen && !sG.sessionActive && (sFsGen != sImageGen || sG.onMpRoot || sG.heldAfterSession)) {',
                      'if (false) {')], 'F1: no save while a stale file select is open', 'caught'),
    ('source_lost', [(GATE, 'inline void sourceLost(State& s) { s.spSessionSlot = 0; }', 'inline void sourceLost(State& s) { (void)s; }')],
     'F5: no single-player source after a migration / foreign snapshot', 'caught'),
    ('mp_read_error', [(SG, 'if (check == -2 || (!isMp && check > 0)) {', 'if (!isMp && (check > 0 || check == -2)) {')],
     'F6: a read error refuses on the multiplayer root', 'caught'),
    ('unsafe_latch', [(SG, '''        if (sUnsafe) {   // savefix (finding 7): an unverified image is never written''', '''        if (false) {''')],
     'F7: no save with an unverified image', 'caught'),
    ('stale_choice', [(GATE, '''            if (s.sessionSeen) {
                s.spSessionSlot = 0;''', '''            if (false) {
                s.spSessionSlot = 0;''')], 'an undecided session does not hand on its save choice', 'caught'),
    ('coop_chosen', [(GATE, 'return (s.onMpRoot && s.coopChosen) || s.spSessionSlot > 0;', 'return s.onMpRoot || s.spSessionSlot > 0;')],
     'the co-op root counts as a choice only when entered for this session', 'caught'),
    ('atomic_nand', [(NAND, '    if (!sAtomicWrites.load() || path == nullptr) return false;', '    return false;')],
     'the game\'s GameData.bin write goes through the atomic writer while multiplayer is involved', 'caught'),
    ('recover_eio', [(SF, '''    if (prc != 0 && prc != -ENOENT && prc != -EFBIG) {
        // savefix''', '''    if (false) {
        // savefix''')], 'the repair never replaces a file it cannot read', 'caught'),
    ('put_side_files', [(SF, '        if (rr == -ENOENT && exists(side)) return MP_SAVE_OP_DST_INVALID;\n        snprintf(side, sizeof side, "%s.mptmp", dstPath);\n        if (rr == -ENOENT && exists(side)) return MP_SAVE_OP_DST_INVALID;\n',
                         '        snprintf(side, sizeof side, "%s.mptmp", dstPath);\n')], 'a save never starts a fresh file over an interrupted replace', 'caught'),
    ('load_side_files', [(SG, '            if (beside) check = -2;\n', '            (void)beside;\n')], 'a root switch never loads a fresh image over an interrupted replace', 'caught'),
    ('newer_recover', [(SF, '    if (prc == 0 && valid == mp_save_is_gamedata && mp_save_is_newer(buf, plen)) {', '    if (false) {')],
     'a newer build\'s GameData.bin is never repaired', 'caught'),
    ('newer_load', [(SG, '        if (check == MP_SAVE_ERR_VERSION && mp_save_is_newer(s.a, len)) check = -2;', '')],
     'a root switch refuses a newer build\'s GameData.bin', 'caught'),
    ('restore_backup_first', [(SF, '        if (!mp_save_backup(spPath, backupDir, unixTime, keep, beforeOut, beforeCap)) return MP_SAVE_OP_BACKUP;\n', '')],
     'restore backs up the file it replaces', 'caught'),
    ('cfg_unreadable_readonly', [(CFG, '    s_readOnly = primaryBad || (ok && s_lastVersion > 1);', '    s_readOnly = ok && s_lastVersion > 1;')],
     'F8 / keepsafe K1: an mp.cfg that cannot be read is never written over', 'caught'),
    ('cfg_unknown_keys', [(CFG, '        if (!known) fprintf(f, "%s = %s\\n", s_raw[i].key, s_raw[i].val);', '        (void)known;')],
     'F8: keys this build does not know are written back', 'caught'),
    ('cfg_bak_rotation', [(CFG, '    const bool hadPrimary = Exists(path);', '    const bool hadPrimary = true;')],
     'mp.cfg.bak removed only when mp.cfg exists', 'caught'),
    ('cfg_readonly', [(CFG, '    if (s_readOnly) return false;   // keepsafe K1/K2/K4 (logged at load)', '')], 'an mp.cfg not understood / newer is never written', 'caught'),
]

TREE = ['decomp/include', 'decomp/src/Game/MP', 'decomp/libs/RVL_SDK/include', 'platform/include', 'platform/src/mp', 'platform/src/nand', 'tests/mp']


def run(cmd, env, log):
    with open(log, 'w') as f:
        r = subprocess.run(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, shell=isinstance(cmd, str))
    return r.returncode


def _mp_tmp():
    """MP_TMP of tests/mp/tmproot.sh: a scratch folder private to this tree (its name + a hash of its path)."""
    import hashlib
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
    return os.environ.get('MP_TMP') or '/tmp/mp-%s-%s' % (os.path.basename(root), hashlib.md5(root.encode()).hexdigest()[:6])

def main():
    args = sys.argv[1:]
    out = os.path.join(_mp_tmp(), 'save_guard_mutations')
    random_n = 150
    names = []
    while args:
        a = args.pop(0)
        if a == '--out': out = args.pop(0)
        elif a == '--random': random_n = int(args.pop(0))
        else: names.append(a)
    os.makedirs(out, exist_ok=True)
    rows, bad = [], 0
    for name, edits, what, expect in MUTATIONS:
        if names and name not in names: continue
        tree = os.path.join(out, name)
        shutil.rmtree(tree, ignore_errors=True)
        for d in TREE:
            shutil.copytree(os.path.join(ROOT, d), os.path.join(tree, d), ignore=shutil.ignore_patterns('*.o', '*.png'))
        for path, old, new in edits:
            p = os.path.join(tree, path)
            s = open(p).read()
            if old not in s:
                print('MUTATION %s: text not found in %s (the guard moved: update this script)' % (name, path))
                return 2
            open(p, 'w').write(s.replace(old, new, 1))
        env = dict(os.environ, MP_TEST_OUT=os.path.join(tree, 'out'), ASAN_OPTIONS='detect_leaks=0')
        caught = []
        if run(['bash', os.path.join(tree, 'tests/mp/savecheck_build.sh'), 'build'], env, os.path.join(tree, 'build1.log')) != 0:
            caught.append('build')
        else:
            for t in ['save_test', 'savegate_test']:
                if run([os.path.join(tree, 'out', t)], env, os.path.join(tree, t + '.log')) != 0: caught.append(t)
            rb = os.path.join(tree, 'out', 'rev_backup_test')
            if run('g++ -std=c++20 -O1 -fsanitize=address,undefined -I%s/platform/include -I%s/platform/src/mp -o %s %s/tests/mp/rev_backup_test.cpp '
                   '%s/platform/src/mp/mp_savefile.cpp %s/platform/src/mp/mp_savecheck.cpp -lpthread' % ((tree,) * 2 + (rb,) + (tree,) * 3),
                   env, os.path.join(tree, 'rev_backup_build.log')) == 0:
                if run([rb], env, os.path.join(tree, 'rev_backup.log')) != 0: caught.append('rev_backup_test')
            ks = os.path.join(tree, 'tests/mp/keepsafe_findings.sh')
            if os.path.exists(ks) and run(['bash', ks], env, os.path.join(tree, 'keepsafe_findings.log')) != 0:
                caught.append('keepsafe_findings')
            if run(['bash', os.path.join(tree, 'tests/mp/savelife_build.sh'), 'build'], env, os.path.join(tree, 'build2.log')) != 0:
                caught.append('savelife(build)')
            else:
                sl = os.path.join(tree, 'out', 'savelife_test')
                if run([sl, '--scripted'], env, os.path.join(tree, 'savelife_scripted.log')) != 0:
                    failed = [l.split('  ')[0] for l in open(os.path.join(tree, 'savelife_scripted.log'), errors='replace') if l.rstrip().endswith('VIOLATED')]
                    caught.append('savelife scripted [' + ', '.join(failed) + ']')
                if run([sl, '--random', str(random_n), '--steps', '60'], env, os.path.join(tree, 'savelife_random.log')) != 0:
                    caught.append('savelife random')
        ok = bool(caught) if expect == 'caught' else True
        if not ok: bad += 1
        rows.append((name, what, '; '.join(caught) if caught else 'NOT CAUGHT', expect, 'ok' if ok else 'FAIL'))
        print('%-28s %-5s %s' % (name, 'ok' if ok else 'FAIL', '; '.join(caught) if caught else 'NOT CAUGHT (%s)' % expect), flush=True)
        shutil.rmtree(os.path.join(tree, 'tests'), ignore_errors=True)
    with open(os.path.join(out, 'TABLE.md'), 'w') as f:
        f.write('| guard removed | what it guards | caught by | expected |\n|---|---|---|---|\n')
        for r in rows: f.write('| %s | %s | %s | %s |\n' % (r[0], r[1], r[2], r[3]))
    print('save_guard_mutations: %d mutations, %d not caught' % (len(rows), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
