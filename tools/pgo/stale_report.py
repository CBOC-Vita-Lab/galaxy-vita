#!/usr/bin/env python3
"""Staleness of a profile against the current tree, by weight.  Needs scan_units.py's output.

    python3 -I tools/pgo/scan_units.py <profile.profdata with the build's source paths> scan.jsonl      (about 25 min, 2 jobs; CPATH=<build>/case-shim)
    llvm-profdata-18 merge --text -o profile.txt <that same profile>
    python3 -I tools/pgo/stale_report.py profile.txt scan.jsonl stale.txt

Weight of a function = the sum of its counters (so a loop body counts once per trip); "match" = defined in the current tree with the
same CFG hash (no Clang hash-mismatch warning), "mismatch" = its counts are discarded, "gone" = no longer defined under that name.
The list in stale.txt: the hottest functions that lost their counts, then the functions of the current tree with no record at all."""
import json,re,subprocess,sys,collections
prof,scan,top=sys.argv[1:4]
prof_txt=prof
P={}
raw=open(prof_txt).read().split('\n'); i=0
if raw[0].startswith('# IR'): i=2
while i<len(raw):
    if raw[i]=='' : i+=1; continue
    n=raw[i]; assert raw[i+1].startswith('# Func Hash'),(i,raw[i:i+3])
    h=int(raw[i+2]); assert raw[i+3].startswith('# Num Counters'); k=int(raw[i+4]); assert raw[i+5].startswith('# Counter')
    cnt=[int(x) for x in raw[i+6:i+6+k]]
    P[n]=(h,sum(cnt),cnt[0] if cnt else 0); i+=6+k
    while i<len(raw) and raw[i]!='': i+=1

units=[json.loads(l) for l in open(scan)]
defs=collections.defaultdict(list)
for u in units:
    for n,(sz,intern) in u['defs'].items():
        defs[(u['file']+';'+n) if intern else n].append((u['kind'],u['file'],sz))
mism=collections.defaultdict(set)
for u in units:
    for n,h,c in u['mism']: mism[n].add(u['file'])
def alts(name):
    if ';' not in name: return [name]
    a1=re.sub(r'^/source/(decomp|platform)/','/source/source/\\1/',name)
    return [name,a1,re.sub(r'^/source/source/platform/','/home/claude/g23/source/source/platform/',a1)]
allnames=list(P)
dem=subprocess.run(['c++filt'],input='\n'.join(n.split(';',1)[-1] for n in allnames),capture_output=True,text=True).stdout.split('\n')
D=dict(zip(allnames,dem))
def side(name,d):
    if d: return 'render' if {k for k,_,_ in d}=={'render'} else 'game'
    return 'render' if (name.startswith('/source/platform') or D[name].startswith('vgx::') or D[name].startswith('(anonymous namespace)::') and False) else 'game'
tot=sum(v[1] for v in P.values())
cat=collections.defaultdict(float); n=collections.Counter(); rows=[]
for name,(h,w,e) in P.items():
    d=[]
    for a in alts(name):
        if a in defs: d=defs[a]; break
    s=side(name,d)
    if not d: c='gone'
    else:
        fn=name.split(';',1)[-1]; files={f for _,f,_ in d}
        c='mismatch' if files<=mism.get(fn,set()) else 'match'
    cat[(s,c)]+=w; n[(s,c)]+=1; rows.append((w,name,c,s,e))
for s in ('game','render'):
    t=sum(v for (a,b),v in cat.items() if a==s)
    print(s,'total %.2f%% of profile'%(100*t/tot),' '.join('%s %d fn %.2f%% (%.1f%% of side)'%(c,n[(s,c)],100*cat[(s,c)]/tot,100*cat[(s,c)]/t) for c in ('match','mismatch','gone')))
# statics lost in the as-built game profile
lost=sum(v[1] for k,v in P.items() if ';' in k and k.startswith('/source/source/decomp'))
print('game statics (path;name) weight %.2f%% of profile, %d functions'%(100*lost/tot,sum(1 for k in P if ';' in k and k.startswith('/source/source/decomp'))))
rs=sum(v[1] for k,v in P.items() if ';' in k and 'platform' in k)
print('renderer statics weight %.2f%%'%(100*rs/tot))
rows.sort(reverse=True)
out=open(top,'w')
out.write('# hottest functions of the old profile that no longer match (weight = sum of the function\'s counters, %% of the whole profile)\n')
k=0
for w,name,c,s,e in rows:
    if c!='match':
        out.write('%6.2f%% %-8s %-6s entry=%-12d %s\n'%(100*w/tot,c,s,e,D[name][:170])); k+=1
        if k>=150: break
# unprofiled new functions
pn=set(P)
new=[]
for key,d in defs.items():
    nm=key
    cand=[nm] if ';' not in nm else [nm,re.sub(r'^/source/source/(decomp|platform)/','/source/\\1/',nm),re.sub(r'^/home/claude/g23/source/source/','/source/',nm)]
    if not any(c in pn for c in cand):
        new.append((max(x[2] for x in d),key,d[0][0],d[0][1]))
new.sort(reverse=True)
dn=subprocess.run(['c++filt'],input='\n'.join(x[1].split(';',1)[-1] for x in new),capture_output=True,text=True).stdout.split('\n')
out.write('\n# functions of the current tree with no record at all in the old profile (IR size in instructions before optimisation), renderer first\n')
for (sz,key,kind,f),d in zip(new,dn):
    if kind=='render': out.write('%6d render %-28s %s\n'%(sz,f.split('/')[-1],d[:150]))
out.write('\n# same, game units (top 150 by size)\n'); k=0
for (sz,key,kind,f),d in zip(new,dn):
    if kind=='game':
        out.write('%6d game   %-28s %s\n'%(sz,f.split('/')[-1],d[:150])); k+=1
        if k>=150: break
print('unprofiled defined functions: render',sum(1 for x in new if x[2]=='render'),'game',sum(1 for x in new if x[2]=='game'),' of defined: render',sum(1 for k,d in defs.items() if d[0][0]=='render'),'game',sum(1 for k,d in defs.items() if d[0][0]=='game'))
