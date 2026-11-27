#!/usr/bin/env python3
"""Self-tests of the test SCRIPTS themselves (rigfix items 4 and 5): a suite script must not exit 0 on failure, must not link stale objects,
and must not write to a fixed /tmp/<name>. Each test builds a tiny fake tree (stub sources) around a copy of the real script and plants a failure.

  python3 tests/mp/rigfix_scripts_test.py          (about 1 min; g++ only, no game)
  RIGFIX_REF=<commit> python3 ...                  takes the scripts from that commit instead of the working tree (proves a test fails before a fix)
"""
import os, shutil, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REF = os.environ.get('RIGFIX_REF')
TMP = tempfile.mkdtemp(prefix='rigfix_scripts_')


def script_text(rel):
    if REF:
        return subprocess.run(['git', '-C', ROOT, 'show', f'{REF}:{rel}'], capture_output=True, text=True, check=True).stdout
    return open(os.path.join(ROOT, rel), encoding='utf-8').read()


def tree(name, files=(), scripts=()):
    """a fake repository root with the given real scripts and stub files {rel: text}"""
    t = os.path.join(TMP, name)
    shutil.rmtree(t, ignore_errors=True)
    for rel in scripts:
        p = os.path.join(t, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(script_text(rel))
    if scripts:   # the scripts source tests/mp/tmproot.sh and call cut_audit.py (the working tree's copies)
        os.makedirs(os.path.join(t, 'tests/mp'), exist_ok=True)
        for helper in ('tmproot.sh', 'cut_audit.py'):
            shutil.copy(os.path.join(ROOT, 'tests/mp', helper), os.path.join(t, 'tests/mp', helper))
    for rel, text in dict(files).items():
        p = os.path.join(t, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'w').write(text)
    return t


def sh(t, rel, *args, env=None, timeout=300):
    e = dict({k: v for k, v in os.environ.items() if k != 'MP_TMP'}, CXX='g++', **(env or {}))
    e.setdefault('MP_TMP', os.path.join(TMP, 'mptmp-' + os.path.basename(t)))   # (the scratch folder goes with the test's own temp folder)
    p = subprocess.run(['bash', os.path.join(t, rel), *args], capture_output=True, text=True, env=e, timeout=timeout)
    return p.returncode, (p.stdout + p.stderr)[-1500:]


OK_MAIN = 'int main(){return 0;}\n'
BAD_MAIN = 'int main(){return 3;}\n'


class NetsmoothBuild(unittest.TestCase):
    def test_failing_pose_test_fails_the_script(self):
        # perf_pose_test / puppet_pose_test were piped into `tail -1`: their failure was swallowed and the suite exited 0
        out = os.path.join(TMP, 'ns_out')
        for bad in ('puppet_pose_test', 'perf_pose_test'):
            files = {f'tests/mp/{n}.cpp': (BAD_MAIN if n == bad else OK_MAIN) for n in ('netsmooth_test', 'puppet_pose_test', 'perf_pose_test')}
            files['tests/mp/netsmooth_sim.cpp'] = OK_MAIN
            t = tree('ns_' + bad, files, ['tests/mp/netsmooth_build.sh'])
            rc, o = sh(t, 'tests/mp/netsmooth_build.sh', env={'MP_TEST_OUT': out, 'MP_TMP': out})
            self.assertNotEqual(rc, 0, bad + ' failed but the script exited 0:\n' + o)
        files = {f'tests/mp/{n}.cpp': OK_MAIN for n in ('netsmooth_test', 'puppet_pose_test', 'perf_pose_test', 'netsmooth_sim')}
        t = tree('ns_good', files, ['tests/mp/netsmooth_build.sh'])
        self.assertEqual(sh(t, 'tests/mp/netsmooth_build.sh', env={'MP_TEST_OUT': out, 'MP_TMP': out})[0], 0)   # (the sim stub takes --seeds: ignored)


KS_FILES = ['tests/mp/keepsafe_test.cpp', 'tests/mp/keepsafe_cut_test.cpp', 'platform/src/mp/mp_boot.cpp', 'platform/src/mp/mp_keepsafe.cpp',
            'platform/src/mp/mp_savefile.cpp', 'platform/src/mp/mp_savecheck.cpp', 'platform/src/mp/mp_config.cpp', 'platform/src/mp/mp_ui_flow.cpp',
            'platform/src/mp/mp_core_sys.cpp', 'platform/src/mp/mp_core_vita.cpp']


def ks_files(main_rc=0):
    f = {rel: f'int stub_{i}(){{return {i};}}\n' for i, rel in enumerate(KS_FILES)}
    f['tests/mp/keepsafe_test.cpp'] = f'int main(){{return {main_rc};}}\n'
    f['tests/mp/keepsafe_cut_test.cpp'] = 'int main(){return 0;}\n'
    for h in ('platform/include/port/mp_keepsafe.h', 'platform/src/mp/mp_savefile.h', 'platform/src/mp/mp_config.h', 'tests/mp/keepsafe_world.h'):
        f[h] = '\n'
    return f


class KeepsafeBuild(unittest.TestCase):
    def test_compile_error_stops_the_build_instead_of_linking_the_old_object(self):
        out = os.path.join(TMP, 'ks_out1')
        t = tree('ks1', ks_files(), ['tests/mp/keepsafe_build.sh'])
        self.assertEqual(sh(t, 'tests/mp/keepsafe_build.sh', 'build', env={'KS_OUT': out})[0], 0)
        import time; time.sleep(1.1)
        open(os.path.join(t, 'platform/src/mp/mp_savecheck.cpp'), 'w').write('this is not C++;\n')   # newer than its object: it must be recompiled
        rc, o = sh(t, 'tests/mp/keepsafe_build.sh', 'build', env={'KS_OUT': out})
        self.assertNotEqual(rc, 0, 'a source that no longer compiles was linked from its old object:\n' + o)

    def test_objects_of_another_tree_are_not_reused(self):
        out = os.path.join(TMP, 'ks_out2')
        a = tree('ks_a', ks_files(main_rc=0), ['tests/mp/keepsafe_build.sh'])
        self.assertEqual(sh(a, 'tests/mp/keepsafe_build.sh', env={'KS_OUT': out})[0], 0)
        b = tree('ks_b', ks_files(main_rc=1), ['tests/mp/keepsafe_build.sh'])   # a different tree whose test FAILS; its files are older than a's objects
        for rel in ks_files():
            os.utime(os.path.join(b, rel), (1, 1))
        rc, o = sh(b, 'tests/mp/keepsafe_build.sh', env={'KS_OUT': out})
        self.assertNotEqual(rc, 0, "tree b's failing test was replaced by tree a's old object:\n" + o)


TSAN = ['platform/src/mp/' + n for n in ('mp_core', 'mp_demo', 'mp_core_sys', 'mp_core_vita', 'mp_reliable', 'mp_lan', 'mp_snapshot', 'mp_config', 'mp_log',
                                         'mp_transport_loop', 'mp_transport_vlink', 'mp_conn_online', 'mp_savecheck', 'mp_savefile')] + \
       ['tests/mp/netglue_stub', 'net/src/vlink', 'net/src/vlink_udp']


class TsanBuild(unittest.TestCase):
    def test_failed_compile_fails_the_script(self):
        files = {n + '.cpp': f'int stub_{i}(){{return 0;}}\n' for i, n in enumerate(TSAN)}
        for n in ('tests/mp/perf_core_test', 'tests/mp/soak_long', 'tests/mp/core_tests', 'tools/mp/mp_bot', 'tests/mp/save_test'):
            files[n + '.cpp'] = 'int main(){return 0;}\n'
        files['tests/mp/core_tests.cpp'] = 'this does not compile;\n'
        t = tree('tsan', files, ['tests/mp/tsan_build.sh'])
        rc, o = sh(t, 'tests/mp/tsan_build.sh', env={'MP_TEST_OUT': os.path.join(TMP, 'tsan_out'), 'MP_TMP': os.path.join(TMP, 'tsan_out')})
        self.assertNotEqual(rc, 0, 'a failed compile was reported as built:\n' + o)


class PrintfCheck(unittest.TestCase):
    def test_split_literal_is_caught(self):
        for i, src in enumerate(['void f(){ printf("a %" "zu\\n", 1); }\n', 'void f(){ printf("x %z"\n /* c */ "u", 2); }\n']):
            t = tree(f'pf{i}', {'platform/src/mp/x.cpp': src}, ['tools/check_printf_vita.sh'])
            if os.path.exists(os.path.join(ROOT, 'tools/check_printf_vita_split.py')) and not REF:
                shutil.copy(os.path.join(ROOT, 'tools/check_printf_vita_split.py'), os.path.join(t, 'tools'))
            rc, o = sh(t, 'tools/check_printf_vita.sh', t)
            self.assertEqual(rc, 1, o)
        t = tree('pf_ok', {'platform/src/mp/x.cpp': 'void f(){ printf("100%%" "z"); printf("%d" "x", 1); }\n'}, ['tools/check_printf_vita.sh'])
        if not REF:
            shutil.copy(os.path.join(ROOT, 'tools/check_printf_vita_split.py'), os.path.join(t, 'tools'))
        self.assertEqual(sh(t, 'tools/check_printf_vita.sh', t)[0], 0)


class SkipAndSoak(unittest.TestCase):
    def test_scenario_data_skip_is_exit_77(self):
        t = tree('sd', {}, ['tests/mp/scenario_data_check.py'])
        p = subprocess.run([sys.executable, os.path.join(t, 'tests/mp/scenario_data_check.py'), os.path.join(TMP, 'no-such-data')], capture_output=True, text=True)
        self.assertEqual(p.returncode, 77, p.stdout + p.stderr)

    def test_soak_matrix_exit_status(self):
        t = tree('soak', {}, ['tests/mp/soak_matrix.sh'])
        bad = os.path.join(t, 'soak_bin.sh')
        open(bad, 'w').write('#!/bin/bash\n[ "$2" = 1 ] && exit 3\necho "SOAK seed=$2 shift=1 creep=0 dups=0"\n')   # seed 1 dies silently
        os.chmod(bad, 0o755)
        rc, o = sh(t, 'tests/mp/soak_matrix.sh', bad, '3', '1', '0')
        self.assertNotEqual(rc, 0, 'a soak run that died printed nothing and the matrix exited 0:\n' + o)
        self.assertIn('fail=1', o)


class PerTreeTmp(unittest.TestCase):
    """Item 5: no test script writes to a fixed /tmp/<name>; two trees get two scratch folders."""
    def test_no_script_uses_a_fixed_tmp_path(self):
        import glob, re
        pat = re.compile(r'(?<![\w$.}/])/tmp/')
        files = sorted(glob.glob(os.path.join(ROOT, 'tests/mp/*.sh')) + glob.glob(os.path.join(ROOT, 'tools/test_*.sh')) + [os.path.join(ROOT, 'tools/mp/partner/test.sh')])
        bad = []
        for f in files:
            rel = os.path.relpath(f, ROOT)
            if rel.endswith('tmproot.sh'):
                continue
            text = script_text(rel)
            if 'docker' in text and rel.startswith('tools/test_'):
                continue   # runs in a container with its own /tmp
            for n, line in enumerate(text.split('\n'), 1):
                if not line.lstrip().startswith('#') and pat.search(line):
                    bad.append(f'{rel}:{n}: {line.strip()[:80]}')
        self.assertEqual(bad, [], f'{len(bad)} fixed /tmp paths, first: {bad[:3]}')

    def test_two_trees_two_folders(self):
        outs = []
        for name in ('tree_one', 'tree_two'):
            t = tree(name, {'tests/mp/tmproot.sh': open(os.path.join(ROOT, 'tests/mp/tmproot.sh')).read(),
                            'tests/mp/probe.sh': '#!/bin/bash\n. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"\necho "$MP_TMP"\n'})
            p = subprocess.run(['bash', os.path.join(t, 'tests/mp/probe.sh')], capture_output=True, text=True, env={k: v for k, v in os.environ.items() if k != 'MP_TMP'})
            outs.append(p.stdout.strip())
        self.assertNotEqual(outs[0], outs[1])
        self.assertTrue(all(o.startswith('/tmp/mp-') for o in outs), outs)


class CutAudit(unittest.TestCase):
    """Item 6: a product call the power-cut model does not interpose fails the build (tests/mp/cut_audit.py)."""
    AUDIT = os.path.join(ROOT, 'tests', 'mp', 'cut_audit.py')

    def obj(self, name, code):
        d = os.path.join(TMP, 'audit')
        os.makedirs(d, exist_ok=True)
        src = os.path.join(d, name + '.cpp')
        open(src, 'w').write(code)
        o = os.path.join(d, name + '.o')
        subprocess.check_call(['g++', '-std=c++20', '-O1', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', '-w', '-c', src, '-o', o])
        return o

    def audit(self, test, prods, *extra):
        p = subprocess.run([sys.executable, self.AUDIT, '--test', test, *extra, *prods], capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr

    def test_uninterposed_product_call_fails(self):
        test = self.obj('t_interp', '#include <unistd.h>\nextern "C" int fsync(int fd){return 0;}\nextern "C" ssize_t write(int, const void*, size_t n){return n;}\n')
        covered = self.obj('p_ok', '#include <unistd.h>\nint f(int fd){ char c = 0; write(fd, &c, 1); return fsync(fd); }\n')
        self.assertEqual(self.audit(test, [covered])[0], 0)
        bad = self.obj('p_bad', '#include <unistd.h>\n#include <stdio.h>\nint f(int fd, FILE* o){ fprintf(o, "%d", fd); return ftruncate(fd, 0) + fdatasync(fd); }\n')
        rc, out = self.audit(test, [covered, bad])
        self.assertEqual(rc, 98, out)
        for sym in ('ftruncate', 'fdatasync', 'fprintf'):
            self.assertIn(sym + ' is called by p_bad.o', out)

    def test_wrap_flags_count_only_with_a_wrapper(self):
        test = self.obj('t_wrap', 'extern "C" int __wrap_fsync(int){return 0;}\n')
        prod = self.obj('p_fs', '#include <unistd.h>\nint f(int fd){ return fsync(fd); }\n')
        self.assertEqual(self.audit(test, [prod], '--wrap', '-Wl,--wrap=fsync')[0], 0)
        self.assertEqual(self.audit(test, [prod], '--wrap', '-Wl,--wrap=write')[0], 98)   # fsync is not in the wrap list
        self.assertEqual(self.audit(test, [prod])[0], 98)

    def test_cxx_file_streams_are_looked_at(self):
        test = self.obj('t_none', 'extern "C" int x(){return 0;}\n')
        prod = self.obj('p_ofs', '#include <fstream>\nvoid f(){ std::ofstream o("x"); o << "y"; }\n')
        self.assertEqual(self.audit(test, [prod])[0], 98)


if __name__ == '__main__':
    unittest.main(verbosity=2, warnings='ignore')
