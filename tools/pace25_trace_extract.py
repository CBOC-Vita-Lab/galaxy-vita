#!/usr/bin/env python3
# tests/pace25_trace.inc from the c69 console logs: python3 tools/pace25_trace_extract.py tests/pace25_trace.inc [log folder]
# Extract per-5s-window pacing records per stage from a console log (read-only analysis).
import re, sys, json, collections
def parse(path):
    stage=None; start=None; recs=collections.OrderedDict()
    cur=None
    t_re=re.compile(r'^(\d\d:\d\d:\d\d\.\d+) (.*)$')
    for line in open(path,errors='replace'):
        m=t_re.match(line)
        if not m: continue
        ts,rest=m.groups()
        mm=re.match(r'flow: scene started: (\w+) scenario',rest)
        if mm:
            stage=mm.group(1); recs.setdefault(stage,[]); cur=None; start=ts
            continue
        if stage is None: continue
        if rest.startswith('flow: entry') and 'asking for' in rest:
            stage=None; continue
        R=recs[stage]
        if rest.startswith('pace: 1-vblank mode'):
            d=re.search(r'in ([\d.]+) s; holds 0/1/2/3/4\+ vblanks (\d+)/(\d+)/(\d+)/(\d+)/(\d+); longest (\d+)',rest)
            R.append(dict(ts=ts,kind='pace',secs=float(d.group(1)),pics=int(re.search(r'(\d+) pictures',rest).group(1)),h=[int(d.group(i)) for i in range(2,7)]))
        elif rest.startswith('present: aligned'):
            d=dict(ts=ts,kind='present')
            g=re.search(r'latency (\d+) vblanks',rest); d['L']=int(g.group(1))
            g=re.search(r'ready after 0/1/2/3/4/5/6/7\+ vblanks ([\d/]+)',rest); d['ready']=[int(x) for x in g.group(1).split('/')]
            g=re.search(r'late (\d+) \((\d+) beyond',rest); d['late']=int(g.group(1))
            g=re.search(r'game steps 1/2/3/4\+ turns ([\d/]+)',rest); d['steps']=[int(x) for x in g.group(1).split('/')]
            g=re.search(r'held-vs-step off by 0/1/2/3\+ ([\d/]+)',rest); d['off']=[int(x) for x in g.group(1).split('/')]
            g=re.search(r'waits (\d+) \(([\d.]+) ms avg',rest); d['waits']=int(g.group(1)); d['waitms']=float(g.group(2))
            d['pics']=int(re.search(r'5\.\d s, (\d+) pictures',rest).group(1)) if re.search(r'5\.\d s, (\d+) pictures',rest) else 0
            R.append(d)
        elif rest.startswith('be-split-5s'):
            g=re.search(r'in play (\d+) pictures, ([\d.]+) ms/picture \(render-thread cpu ([\d.]+)\)',rest)
            d=dict(ts=ts,kind='be')
            if g: d.update(pics=int(g.group(1)),ms=float(g.group(2)),cpu=float(g.group(3)))
            g=re.search(r'present ([\d.]+) \(display queue ([\d.]+)\)',rest)
            if g: d['present']=float(g.group(1)); d['dq']=float(g.group(2))
            g=re.search(r'sema ([\d.]+)',rest); d['sema']=float(g.group(1)) if g else None
            g=re.search(r'waits ([\d.]+)',rest); d['waits']=float(g.group(1)) if g else None
            R.append(d)
        elif rest.startswith('latency:'):
            d=dict(ts=ts,kind='lat')
            g=re.search(r'in flight max (\d+) pictures \(display queue max (\d+), back-end queue max (\d+)\)',rest)
            if g: d.update(inflight=int(g.group(1)),dqmax=int(g.group(2)),beq=int(g.group(3)))
            g=re.search(r'pad sample to flip ([\d.]+) ms mean',rest); d['padflip']=float(g.group(1)) if g else None
            R.append(d)
        elif rest.startswith('fps:'):
            g=re.search(r'shown ([\d.]+) fps; game ([\d.]+) turns/s; (\d+) skipped',rest)
            if g: R.append(dict(ts=ts,kind='fps',fps=float(g.group(1)),tps=float(g.group(2)),skipped=int(g.group(3))))
        elif rest.startswith('turn-prof:'):
            g=re.search(r'(\d+) turns \((\d+) drawn\); ms a turn: busy ([\d.]+) .*a drawn turn: busy ([\d.]+) .*an undrawn turn: busy ([\d.]+)',rest)
            if g: R.append(dict(ts=ts,kind='tp',turns=int(g.group(1)),drawn=int(g.group(2)),busy=float(g.group(3)),dbusy=float(g.group(4)),nbusy=float(g.group(5))))
        elif rest.startswith('cores-5s:'):
            g=re.search(r'core 0 ([\d.]+) .*core 1 ([\d.]+) .*core 2 ([\d.]+)',rest)
            if g: R.append(dict(ts=ts,kind='cores',c0=float(g.group(1)),c1=float(g.group(2)),c2=float(g.group(3))))
        elif rest.startswith('gpu-scenes:'):
            g=re.search(r'busy ([\d.]+) ms',rest)
            if g: R.append(dict(ts=ts,kind='gpu',busy=float(g.group(1))))
    return recs

D=(sys.argv[2] if len(sys.argv)>2 else '/home/claude/g23/agents/logs/console-1111')+'/c69-tour%s-log.txt'
STAGES=[('FactoryGalaxy','Toy Time'),('HoneyBeeKingdomGalaxy','Honeyhive'),('OceanRingGalaxy','Ocean Ring'),('HellProminenceGalaxy','Melty Molten'),
        ('EggStarGalaxy','Good Egg (control 54 fps)'),('TriLegLv1Galaxy','Tri-Leg (control 60 fps)')]
rows=[]
for run in ['K','E','EU']:
    P=parse(D%run)
    for st,nm in STAGES:
        R=P.get(st,[])
        wins=[];cur={}
        for x in R:
            if x['kind'] in cur: wins.append(cur);cur={}
            cur[x['kind']]=x
        wins.append(cur)
        k=0
        for w in wins[3:]:
            if not all(q in w for q in ('tp','pace','present','gpu','be','cores')): continue
            h=w['pace']['h']; n=sum(h)
            rows.append(dict(run=run,stage=st,win=k,d=w['tp']['dbusy'],n=w['tp']['nbusy'],gpu=w['gpu']['busy'],be=w['be'].get('cpu',0),fe=w['cores']['c2'],
                             pics=w['pace']['pics'],h1=h[1],h2=h[2],h3=h[3],h4=h[4],L=w['present']['L'],sema=w['be'].get('sema') or 0,
                             steps=w['present']['steps']))
            k+=1
if __name__=='__main__':
    out=open(sys.argv[1],'w')
    out.write('// Generated by agents/pacer25/tools/extract.py from /home/claude/g23/agents/logs/console-1111/c69-tour{K,E,EU}-log.txt (5 s windows after the first three of each stage).\n')
    out.write('// run, stage, window, drawn-turn ms, undrawn-turn ms, GPU busy ms/picture, render-thread cpu ms/picture, core 2 (front end) ms/picture,\n')
    out.write('// pictures, holds 1/2/3/4+ (vblanks), present latency L, sema wait ms/picture\n')
    for r in rows:
        out.write('{"%s","%s",%d,%.2f,%.2f,%.1f,%.1f,%.1f,%d,%d,%d,%d,%d,%d,%.2f},\n'%(r['run'],r['stage'],r['win'],r['d'],r['n'],r['gpu'],r['be'],r['fe'],r['pics'],r['h1'],r['h2'],r['h3'],r['h4'],r['L'],r['sema']))
    print(len(rows),'rows')
