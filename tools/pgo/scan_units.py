#!/usr/bin/env python3
"""Compile every unit of the Vita build (read-only sources, no objects kept) with -fprofile-use and record, per unit:
the functions the frontend defines (pre-optimisation IR, with IR size), and Clang's per-function hash-mismatch warnings.
usage: scan_units.py PROFILE OUT.jsonl [--jobs N] [--only REGEX]"""
import json,shlex,subprocess,sys,re,argparse,os
from concurrent.futures import ThreadPoolExecutor
ap=argparse.ArgumentParser(); ap.add_argument('profile'); ap.add_argument('out'); ap.add_argument('--jobs',type=int,default=2); ap.add_argument('--only',default='')
ap.add_argument('--db',default='/build/vita-source/compile_commands.json'); ap.add_argument('--ninja',default='/build/vita-vgx-runtime/build.ninja')
ap.add_argument('--select',default=r'/platform/vgx/src/|/platform/src/gx/')
a=ap.parse_args()
units=[]
for e in json.load(open(a.db)):
    args=shlex.split(e['command']); units.append(('game',e['file'],args))
src=cmd=None
sel=re.compile(a.select)
for line in open(a.ninja).read().splitlines():
    if line.startswith('build ') and ': cc ' in line: src,cmd=line.split(': cc ',1)[1].strip(),None
    elif src and line.startswith('  cmd = '): cmd=shlex.split(line[8:].replace('$$','$'))
    elif src and cmd is not None and line.startswith('  extra = '):
        if sel.search(src) and src.endswith('.cpp'): units.append(('render',src,cmd+shlex.split(line[10:].replace('$$','$'))))
        src=cmd=None
if a.only: units=[u for u in units if re.search(a.only,u[1])]
def prep(args,file):
    b=[x for x in args if not x.startswith('-fprofile-use') and not x.startswith('-fprofile-generate')]
    if '-o' in b: i=b.index('-o'); del b[i:i+2]
    while file in b: b.remove(file)
    if '-c' in b: b.remove('-c')
    return b
def work(u):
    kind,file,args=u; b=prep(args,file)
    r=subprocess.run(b+['-fprofile-use='+a.profile,'-S','-Wbackend-plugin','-Wno-error','-o','/dev/null',file],capture_output=True,text=True)
    mism=re.findall(r'hash mismatch\) (\S+) Hash = (\d+) up to (\d+) count discarded',r.stderr)
    ood=re.findall(r'of (\d+) functions?, (\d+) (?:has|have) mismatched',r.stderr)
    r2=subprocess.run(b+['-S','-emit-llvm','-Xclang','-disable-llvm-passes','-w','-o','-',file],capture_output=True,text=True)
    funcs={};cur=None;n=0
    for l in r2.stdout.splitlines():
        if l.startswith('define '):
            m=re.search(r'@("[^"]+"|[^\s(]+)\(',l); cur=m.group(1).strip('"') if m else None; n=0
            intern=('internal ' in l.split('@')[0]) or ('private ' in l.split('@')[0])
            cur_i=intern
        elif cur is not None:
            if l=='}': funcs[cur]=[n,1 if cur_i else 0]; cur=None
            elif l.startswith('  ') : n+=1
    return {'kind':kind,'file':file,'rc':r.returncode,'rc2':r2.returncode,'mism':mism,'defs':funcs,'err':r.stderr[-300:] if r.returncode else ''}
with open(a.out,'w') as o:
    with ThreadPoolExecutor(a.jobs) as p:
        for i,res in enumerate(p.map(lambda u: work(u),units)):
            o.write(json.dumps(res)+'\n'); o.flush()
            if i%100==0: print(i,len(units),flush=True)
