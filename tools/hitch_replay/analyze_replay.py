"""Read matched Honeyhive arm logs. Only allowlisted, non-identifying metrics are output."""
import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent
ANCHOR = "script: clock started: t = 0 at the first play turn in HoneyBeeKingdomGalaxy scenario 1"
FPS = re.compile(r"fps: ([\d.]+) s: shown ([\d.]+) fps; game ([\d.]+) turns/s; \d+ skipped; (\d+) hitches over 100 ms \(longest (\d+) ms\); picture (\d+x\d+)")
PRESENT = re.compile(r"stutter: frame (\d+) took (\d+) ms in the back end.*?present (\d+) ms, other.*?in the whole frame: shader compiler (\d+) runs")
HITCH = re.compile(r"hitch: frame (\d+) (\d+) ms \(back end (\d+)\):")
TURN = re.compile(r"hitch25: game turn (\d+) ms \((screen [^;]+); ([^;]+) (\d+) ms; stage key [\da-f]+\)")
DRIVER = Path("portable/cache/shaders/SMGV00001/eboot.bin/pipeline-cache-vk15.dat")
FINISHED = "second controllable scene plus 18 seconds"
MOVING_FINISHED = "first controllable scene plus 85 seconds"
EMU_RELEASE = "[main]: Vita3K v0.2.1 4232-92bda548"
STAMP = re.compile(r"^\[?(\d\d:\d\d:\d\d\.\d{3})\]? (.*)$")
OPENED = "[export_sceIoOpen]: Opening file: ux0:/data/smg/game/files/LayoutData/_bk01/MiiSelect.arc"
LOADED = "[mp] mem ui archive request MiiSelect (/LayoutData/MiiSelect.arc) loaded-now=1"


def clock(s):
    h,m,v=s.split(":")
    if int(h)>23 or int(m)>59 or float(v)>=60: raise ValueError("invalid log timestamp")
    return 3600*int(h)+60*int(m)+float(v)


def near(t, origin):
    # The route is capped at six minutes. Map times of day to the nearest day
    # relative to an independent event in this log, including across midnight.
    return round(origin+(t-origin+43200)%86400-43200,3)


def stamped(lines):
    return [(n,clock(m[1]),m[2]) for n,s in enumerate(lines,1) if (m:=STAMP.match(s))]


def ordered_pairs(events):
    pairs=[]; opened=None
    for n,t,began in events:
        if began:
            if opened is not None: return None
            opened=(n,t)
        else:
            if opened is None or not 0<=t-opened[1]<60: return None
            pairs.append((*opened,n,t))
            opened=None
    return pairs if opened is None else None


def match_counts(guest,host,moving):
    if not moving: return len(guest)==len(host)==2
    if not 2<=len(host)<=len(guest): return False
    # On the moving route the host stops logging this archive open after its
    # first respawn even though the guest continues reporting loads. Accept a
    # prefix only when the observed first-pair span identifies it uniquely.
    span=host[1][1]-host[0][1]
    options=[(i,j) for i,(_,a) in enumerate(guest) for j,(_,b) in enumerate(guest) if j>i and abs(b-a-span)<.03]
    return options==[(0,1)]


def stats(rows):
    sec=sum(float(r[1]) for r in rows)
    if not sec: return {"windows":0}
    return {"windows":len(rows),"seconds":round(sec,2),
            "shown_fps":round(sum(float(r[1])*float(r[2]) for r in rows)/sec,2),
            "turns_per_s":round(sum(float(r[1])*float(r[3]) for r in rows)/sec,2),
            "over100":sum(int(r[4]) for r in rows),"longest_ms":max(int(r[5]) for r in rows),
            "line_numbers":[r[0] for r in rows]}


def scan(d):
    meta=json.loads((d/"start.json").read_text())
    moving=meta.get("route","stationary")=="moving"
    outcome=json.loads((d/"outcome.json").read_text())
    if outcome.get("stop")!=(MOVING_FINISHED if moving else FINISHED) or outcome.get("exit") not in (0,-15):
        raise ValueError("not a completed supervised traversal; omit this arm from timings")
    files=[d/"portable/fs/ux0/data/smg/log.txt",d/"host.log"]
    game,host=(p.read_text(errors="replace").splitlines() for p in files)
    if not any(EMU_RELEASE in line for line in host):
        raise ValueError("unverified emulator release")
    rows=stamped(game)
    ganchors=[t for _,t,s in rows if s==ANCHOR]
    if not ganchors or (not moving and len(ganchors)!=2): raise ValueError("expected two controllable starts for stationary, at least one for moving")
    rows=[(n,near(t,ganchors[0]),s) for n,t,s in rows]
    ganchors=[t for _,t,s in rows if s==ANCHOR]
    first=ganchors[0]
    cuts=[t for _,t,s in rows if t>first and s.startswith("vgx: scene destroyed:")]
    if not moving and (len(cuts)!=1 or not first<cuts[0]<ganchors[1]<first+360):
        raise ValueError("unreliable control/scene boundaries")
    if any(b<=a for a,b in zip(ganchors,ganchors[1:])): raise ValueError("unordered control markers")
    thresholds=sorted([(first,"moving controlled" if moving else "first controlled")]+
        [(t-1,"moving reload / opening" if moving else "forced re-entry / opening") for t in cuts]+
        [(t,"moving controlled" if moving else "second controlled") for t in ganchors[1:]])
    if any(a[1]==b[1] or a[0]>=b[0] for a,b in zip(thresholds,thresholds[1:])):
        raise ValueError("unmatched reload/control boundaries")
    # Last second preceding scene destruction can contain the transition to black.
    def name(t):
        return next((v for u,v in reversed(thresholds) if t>=u),"startup / opening")
    grouped={}
    hit=[]; gpu=[]; turns=[]
    for n,t,s in rows:
        if m:=FPS.match(s):
            start=t-float(m[1]); phase="boundary (excluded)" if any(start-.06<u<=t for u,_ in thresholds) else name(t)
            grouped.setdefault(phase,[]).append((n,*m.groups()))
        if (m:=HITCH.match(s)) and int(m[2])>=100:
            hit.append({"l":n,"frame":int(m[1]),"phase":name(t),"ms":int(m[2]),"backend_ms":int(m[3]),"t":round(t,3)})
        if (m:=PRESENT.match(s)) and int(m[2])>=100:
            gpu.append({"l":n,"t":round(t,3),"frame":int(m[1]),"phase":name(t),"backend_ms":int(m[2]),"present_ms":int(m[3]),"shader_compiles":int(m[4])})
        if (m:=TURN.match(s)) and int(m[1])>=100:
            turns.append({"l":n,"phase":name(t),"ms":int(m[1]),"screen":m[2],"largest":m[3],"largest_ms":int(m[4]),"t":round(t,3)})
    # Two independent synchronous archive opens, one per scene entry, observed in both clocks.
    ref_guest=[(n,t) for n,t,s in rows if LOADED in s]
    hrows=stamped(host)
    raw_hrefs=[t for _,t,s in hrows if OPENED in s]
    if raw_hrefs:
        origin=near(raw_hrefs[0],first)
        hrows=[(n,near(t,origin),s) for n,t,s in hrows]
    ref_host=[(n,t) for n,t,s in hrows if OPENED in s]
    refs=[{"guest_line":g,"host_line":h,"host_minus_guest_s":round(ht-gt,3)} for (g,gt),(h,ht) in zip(ref_guest,ref_host)]
    offsets=[r["host_minus_guest_s"] for r in refs]
    synced = (match_counts(ref_guest,ref_host,moving) and ref_guest[0][1]<first and cuts and cuts[0]<ref_guest[1][1]
              and (moving or len(ref_guest)==2 and ref_guest[1][1]<ganchors[1])
              and all(b[1]>a[1] for anchors in (ref_guest,ref_host) for a,b in zip(anchors,anchors[1:]))
              and max(offsets)-min(offsets)<.03)
    synced=bool(synced)
    begin="[save_pipeline_cache]: Saving pipeline cache"
    end="[save_pipeline_cache]: Pipeline cache saved"
    begins=sum(begin in s for s in host); ends=sum(end in s for s in host)
    events=[(n,t,begin in s) for n,t,s in hrows if begin in s or end in s]
    ordered=ordered_pairs(events) if len(events)==begins+ends else None
    saves=[]; late_host_shaders=[]; checkpoints=[]
    for n,t,s in hrows:
        m=re.search(r"Pipeline checkpoint snapshot: (\d+) ms, (\d+) driver bytes, final (true|false)",s)
        w=re.search(r"Pipeline checkpoint disk write: (\d+) ms, success (true|false)",s)
        if m or w:
            fields=({"type":"snapshot","ms":int(m[1]),"driver_bytes":int(m[2]),"final":m[3]=="true"} if m
                    else {"type":"disk_write","ms":int(w[1]),"success":w[2]=="true"})
            checkpoints.append({"host_line":n,"guest_t":round(t-sum(offsets)/len(offsets),3) if synced else None,**fields})
    if synced:
        ofs=sum(offsets)/len(offsets)
        for a,t0,b,t1 in ordered or ():
            saves.append({"host_lines":[a,b],"guest_start":round(t0-ofs,3),"guest_end":round(t1-ofs,3),"duration_ms":round(1000*(t1-t0))})
        for n,t,s in hrows:
            m=re.search(r"\[retrieve_shader\]: Generating vulkan spv shader ([0-9a-f]+)",s)
            if m and t-ofs>first:
                late_host_shaders.append({"l":n,"guest_t":round(t-ofs,3),"hash":m[1]})
    observations=[]
    if synced:
        # Checkpoint onset is not logged in the stock binary. Compare the
        # reported guest picture ending within clock uncertainty of stock write
        # completion or the patched render-thread snapshot, not worker writes.
        uncertainty=max(.003,max(offsets)-min(offsets)+.002)
        for kind,mark in ([("stock_save_end",s["guest_end"]) for s in saves]+
                          [("final_snapshot" if c["final"] else "snapshot_end",c["guest_t"])
                           for c in checkpoints if c["type"]=="snapshot"]):
            gaps=[{"line":n,"frame":int(m[1]),"picture_ms":int(m[2]),"end_offset_ms":round((t-mark)*1000)}
                  for n,t,s in rows if abs(t-mark)<=uncertainty and (m:=HITCH.match(s))]
            enclosing=[{"line":n,"max_ms":int(m[5]),"seconds":float(m[1]),"over100":int(m[4])}
                       for n,t,s in rows if (m:=FPS.match(s)) and t-float(m[1])<=mark<=t]
            observations.append({"event":kind,"guest_t":mark,
                                 "phase":name(mark) if enclosing else "outside a reported picture window",
                                 "clock_tolerance_ms":round(1000*uncertainty),"nearby_logged_gaps":gaps,
                                 "enclosing_fps_windows":enclosing})
    return {"input":meta,"result":outcome,
            "raw_sha256":[hashlib.sha256(f.read_bytes()).hexdigest() for f in files],
            "anchors":ganchors,"scene_destructions":cuts,"destroy":cuts[0] if cuts else None,"coverage":{k:stats(v) for k,v in grouped.items()},
            "hitch_intervals_ge100":hit,"stutter_backend_ge100ms":gpu,"turn_gap_ge100":turns,
            "host_guest_sync":refs,"archive_marker_counts":{"guest":len(ref_guest),"host":len(ref_host)},"independent_sync_valid":synced,
            "host_save_messages":{"began":begins,"completed":ends},"host_checkpoint_messages":checkpoints,
            "checkpoint_pictures":observations if synced else None,
            "host_spirv_generation_messages":sum("[retrieve_shader]: Generating vulkan spv shader " in s for s in host),
            "host_saves_guest_clock_s":saves if synced and ordered is not None else None,
            "host_shader_generations_after_control":late_host_shaders if synced else None,
            "final_pipeline_bytes":(d/DRIVER).stat().st_size}


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("profile",type=Path,help="completed run directory containing start.json and outcome.json")
    a=p.parse_args()
    print(json.dumps(scan(a.profile.resolve(strict=True)),indent=2,allow_nan=False))
