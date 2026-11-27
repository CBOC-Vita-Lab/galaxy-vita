#!/usr/bin/env python3
"""Symbol audit for the power-cut tests (keepsafe_cut_test, savelife): the model only cuts at the file-changing calls it interposes. A product
object that calls a file-changing libc function the test does NOT interpose would change the disk without a cut point there, and the test would
pass on a hole. This fails the build instead.

  cut_audit.py --test OBJ [--test OBJ ...] [--wrap '-Wl,--wrap=open,--wrap=write'] PRODUCT_OBJ...
  A symbol X is covered when a --test object defines X (an interposer by name), or when X is in the --wrap list and a --test object defines __wrap_X.
  Exit 0 = every file-changing symbol the product objects import is covered; 98 = at least one is not (listed); 2 = usage / nm problem.
"""
import re, subprocess, sys

# libc / libstdc++ symbols that change the file system (or the data of an open file). Reading calls are not here.
DENY = r'''open open64 openat openat64 creat creat64 __open_2 __open64_2 __openat_2 __openat64_2 fopen fopen64 freopen freopen64 fdopen tmpfile tmpfile64
mkstemp mkstemp64 mkostemp mkostemp64 mkstemps mkstemps64 mkdtemp mktemp tmpnam
write pwrite pwrite64 writev pwritev pwritev64 pwritev2 fwrite fwrite_unlocked fputs fputs_unlocked fputc fputc_unlocked putc putc_unlocked fputws fputwc putwc
fprintf vfprintf fprintf_chk __fprintf_chk __vfprintf_chk dprintf vdprintf __dprintf_chk __vdprintf_chk fflush fflush_unlocked fclose fcloseall setvbuf
fsync fdatasync syncfs sync sync_file_range ftruncate ftruncate64 truncate truncate64 posix_fallocate posix_fallocate64 fallocate fallocate64
rename renameat renameat2 unlink unlinkat remove rmdir mkdir mkdirat mkfifo mkfifoat mknod mknodat link linkat symlink symlinkat
chmod fchmod fchmodat chown fchown fchownat lchown utime utimes futimes futimens utimensat lutimes
copy_file_range sendfile sendfile64 splice vmsplice mmap msync madvise
nftw nftw64 ftw ftw64 fts_open'''.split()
# C++ file streams / std::filesystem (mangled prefixes): they reach the C calls inside libstdc++, which the model interposes only if it sees them
DENY_RE = [re.compile(p) for p in (r'^_ZNSt(?:__cxx11)?\d*(?:basic_ofstream|basic_fstream|basic_filebuf)', r'^_ZNSt\d*basic_filebuf', r'^_ZNSt10filesystem', r'^_ZNSt7__cxx1110filesystem')]
# mmap/msync/madvise only count when the product maps a file for writing; they are listed so that a new use is looked at, not silently passed
LOOKED_AT = {'mmap', 'msync', 'madvise', 'setvbuf', 'fflush_unlocked', 'fcloseall'}


def nm(path, flag):
    p = subprocess.run(['nm', flag, '--no-demangle', path], capture_output=True, text=True)
    if p.returncode != 0:
        print(f'cut_audit: nm {flag} {path} failed: {p.stderr.strip()}', file=sys.stderr)
        sys.exit(2)
    out = set()
    for line in p.stdout.splitlines():
        parts = line.split()
        if parts:
            out.add(parts[-1].split('@')[0])
    return out


def main():
    a = sys.argv[1:]
    tests, wrap, prods = [], '', []
    while a:
        x = a.pop(0)
        if x == '--test':
            tests.append(a.pop(0))
        elif x == '--wrap':
            wrap = a.pop(0)
        else:
            prods.append(x)
    if not tests or not prods:
        print(__doc__, file=sys.stderr)
        return 2
    defined = set()
    for t in tests:
        defined |= nm(t, '--defined-only')
    wrapped = {w for w in re.findall(r'--wrap=([A-Za-z0-9_]+(?:,--wrap=[A-Za-z0-9_]+)*)', wrap) for w in w.split(',--wrap=')} if wrap else set()
    covered = set(defined) | {w for w in wrapped if '__wrap_' + w in defined}
    deny = set(DENY)
    bad = {}
    for o in prods:
        for s in nm(o, '-u'):
            if (s in deny or any(r.match(s) for r in DENY_RE)) and s not in covered:
                bad.setdefault(s, []).append(o.rsplit('/', 1)[-1])
    for s, objs in sorted(bad.items()):
        print(f'cut_audit: {s} is called by {", ".join(sorted(set(objs)))} but the power-cut model does not interpose it'
              f'{" (looked-at symbol: justify and allow it)" if s in LOOKED_AT else ""}')
    if bad:
        print(f'cut_audit: FAILED, {len(bad)} file-changing symbol(s) outside the interposed set (add an interposer to the test)')
        return 98
    print(f'cut_audit: ok ({len(prods)} object(s); {len(covered & deny)} file-changing symbols interposed)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
