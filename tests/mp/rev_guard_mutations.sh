#!/bin/bash
# usage: rev_mut.sh <name> ; applies mutation to a copy of mp_savefile.cpp and runs save_test
set -uo pipefail
. "$(cd "$(dirname "$0")" && pwd)/tmproot.sh"
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$MP_TMP/rev_save_out/mut; mkdir -p $out
src=$root/platform/src/mp/mp_savefile.cpp
cp $src $out/mp_savefile_$1.cpp
f=$out/mp_savefile_$1.cpp
case $1 in
  verify) python3 - "$f" <<'P'
import sys;p=sys.argv[1];s=open(p).read()
a="ok = mp_save_read_file(tmp, check, MP_SAVE_MAX_FILE, &got) == 0 && got == len && memcmp(check, data, len) == 0;"
assert a in s; s=s.replace(a,"ok = true; (void)got;"); open(p,'w').write(s)
P
  ;;
  spinvalid) python3 - "$f" <<'P'
import sys;p=sys.argv[1];s=open(p).read()
a="""    rc = readValid(spPath, w.b, &spLen, false);
    if (rc != MP_SAVE_OP_OK) return rc;"""
assert a in s; s=s.replace(a,"    (void)spLen;"); open(p,'w').write(s)
P
  ;;
  postverify) python3 - "$f" <<'P'
import sys;p=sys.argv[1];s=open(p).read()
a="if (readValid(dstPath, w.b, &len, false) != MP_SAVE_OP_OK || len != out || memcmp(w.a, w.b, out) != 0) return MP_SAVE_OP_VERIFY;"
assert a in s; s=s.replace(a,""); open(p,'w').write(s)
P
  ;;
  backupprune) python3 - "$f" <<'P'
import sys;p=sys.argv[1];s=open(p).read()
a="if (!mp_save_backup(spPath, backupDir, unixTime, keep, backupOut, backupCap)) return MP_SAVE_OP_BACKUP;"
assert a in s; s=s.replace(a,"(void)mp_save_backup(spPath, backupDir, unixTime, keep, backupOut, backupCap);"); open(p,'w').write(s)
P
  ;;
esac
g++ -std=c++20 -g -O1 -fsanitize=address,undefined -I$root/platform/include -I$root/platform/src/mp -o $out/save_test_$1 $root/tests/mp/save_test.cpp $f $root/platform/src/mp/mp_savecheck.cpp -lpthread && $out/save_test_$1 2>&1 | tail -4
