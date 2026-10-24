#!/usr/bin/env python3
"""Validate bounded owner-root/backend-span reports; wall time is not FPS."""
import argparse
import json
from pathlib import Path
import re
import sys

MAX_BYTES=2*1024*1024
PHASES=("compile","link","shark","mkdir","open","seek","read","write","close",
        "register","patch_vertex","patch_fragment","begin_scene","end_scene","finish","notification","draw")
PHASES_V3=PHASES+("transfer_finish",)
ORIGINS=("unknown","queued","direct_shader","direct_program","utility","texture","stream","present","sdk")
U64=(1<<64)-1
U32=(1<<32)-1

def _get(mapping,key):
    if not isinstance(mapping,dict) or key not in mapping:
        raise ValueError(f"missing report field: {key}")
    return mapping[key]

def _integer(value,low,high,label):
    if type(value) is not int or not low<=value<=high:
        raise ValueError(f"invalid integer field: {label}")
    return value

def _array(mapping,key,limit,exact=False):
    values=_get(mapping,key)
    if not isinstance(values,list) or (len(values)!=limit if exact else len(values)>limit):
        raise ValueError(f"invalid array bounds: {key}")
    return values

def _validate_item(item,report,phases):
    key=_get(item,"key")
    for field in ("frame","kind","a","b","layout","blend"):_integer(_get(key,field),0,U64 if field=="frame" else U32,field)
    for field in ("start_us","total_us","self_us"):_integer(_get(item,field),0,U64,field)
    _integer(_get(item,"flags"),0,7,"flags");_integer(_get(item,"shader_marks_dropped"),0,U32,"shader_marks_dropped")
    if item["start_us"]+item["total_us"]>U64:raise ValueError("item interval exceeds the clock range")
    if item["start_us"]<report["capture_start_us"]:raise ValueError("item clock precedes capture start")
    for field in ("inclusive_us","exclusive_us","bytes","requested","calls","first_error","failures","short_results"):
        low,high=(-(1<<63),(1<<63)-1) if field=="first_error" else (0,U32 if field in ("calls","failures","short_results") else U64)
        for value in _array(item,field,len(phases),True):_integer(value,low,high,field)
    if not item["flags"]&3 and not report["counter_overflows"] and not report["clock_errors"] and item["self_us"]+sum(item["exclusive_us"])!=item["total_us"]:
        raise ValueError("exclusive phases do not partition the item wall time")
    for shader in _array(item,"shaders",2):
        _integer(_get(shader,"shader"),0,U32,"shader");_integer(_get(shader,"kind"),0,U32,"shader kind")
        value=_get(shader,"source_hash")
        if not isinstance(value,str) or not re.fullmatch(r"[0-9A-F]{16}",value):raise ValueError("invalid exact shader hash")
    if report["layout_version"]==3:
        _integer(_get(item,"origin"),0,len(ORIGINS)-1,"origin")
        for field in ("execute_serial","slice_serial"):
            _integer(_get(item,field),0,report["executions_seen"]+report["slices_seen"],field)

def _contained(child,parent):
    return parent["start_us"]<=child["start_us"] and child["start_us"]+child["total_us"]<=parent["start_us"]+parent["total_us"]

def _validate_spans(report,phases):
    by_serial={}
    maximum=_get(report,"max_upload_execution")
    tables=[("executions",_array(report,"executions",16),"executions_seen"),("slices",_array(report,"slices",16),"slices_seen")]
    if maximum is not None:tables.append(("max_upload_execution",[maximum],None))
    for table,spans,count in tables:
        if count and not report["counter_overflows"] and len(spans)!=min(report[count],16):raise ValueError("invalid bounded span retention count")
        for span in spans:
            for field in ("serial","parent_serial","frame","start_us","total_us","root_us","self_us","unclassified_us","roots"):
                _integer(_get(span,field),1 if field=="serial" else 0,U64,field)
            _integer(_get(span,"flags"),0,3,"span flags")
            serial=span["serial"]
            previous=by_serial.get(serial)
            if (previous and not (table=="max_upload_execution" and previous==("executions",span))) or serial>report["executions_seen"]+report["slices_seen"]:raise ValueError("invalid span serial")
            by_serial[serial]=("executions" if table=="max_upload_execution" else table,span)
            if span["start_us"]<report["capture_start_us"] or span["start_us"]+span["total_us"]>U64:raise ValueError("invalid span clock range")
            for field,size in [("exclusive_us",len(phases)),("origin_us",len(ORIGINS))]:
                for value in _array(span,field,size,True):_integer(value,0,U64,field)
            if span["roots"]>report["items_seen"]:raise ValueError("span has more roots than the whole capture")
            if span["self_us"]>span["root_us"]:raise ValueError("span self exceeds root wall time")
            if span["root_us"]<=span["total_us"]:
                if span["unclassified_us"]!=span["total_us"]-span["root_us"]:raise ValueError("invalid span gap arithmetic")
            elif span["unclassified_us"]!=0 or not span["flags"]&2:raise ValueError("invalid clamped malformed span gap")
            if not span["flags"] and not report["clock_errors"] and not report["counter_overflows"]:
                if span["self_us"]+sum(span["exclusive_us"])!=span["root_us"] or sum(span["origin_us"])!=span["root_us"] or span["root_us"]+span["unclassified_us"]!=span["total_us"]:
                    raise ValueError("span phases, roots, origins and gaps do not partition wall time")
            if span["parent_serial"]>=serial or (table!="slices" and span["parent_serial"]):raise ValueError("invalid span parent serial")
            if table=="slices" and not span["parent_serial"] and span["frame"]:raise ValueError("idle slice has no execution frame")
            item=_get(span,"max_item")
            if not span["roots"]:
                if item is not None:raise ValueError("empty span has a fabricated maximum root")
            else:
                _validate_item(item,report,phases)
                field="slice_serial" if table=="slices" else "execute_serial"
                if item[field]!=serial or item["total_us"]>span["root_us"]:raise ValueError("invalid maximum root owner or duration")
                if not span["flags"] and not item["flags"]&3 and not _contained(item,span):raise ValueError("maximum root escapes its complete span")
    texture=ORIGINS.index("texture")
    if maximum is not None and (not maximum["roots"] or not maximum["origin_us"][texture]):raise ValueError("maximum-upload execution has no texture time")
    if any(span["origin_us"][texture] and (maximum is None or span["origin_us"][texture]>maximum["origin_us"][texture]) for span in report["executions"]):raise ValueError("maximum-upload selection is smaller than a retained execution")
    for table in ("executions","slices"):
        for span in report[table]:
            parent=by_serial.get(span["parent_serial"])
            # Independent worst sets need not retain a child's parent.
            if parent:
                if parent[0]!="executions" or parent[1]["frame"]!=span["frame"]:raise ValueError("slice parent is not the matching execution")
                if not span["flags"] and not parent[1]["flags"] and not _contained(span,parent[1]):raise ValueError("slice escapes its complete execution")
    all_items=list(report["items"])
    all_items.extend(span["max_item"] for _,spans,_ in tables for span in spans if span["max_item"] is not None)
    for item in all_items:
        for field,table in [("execute_serial","executions"),("slice_serial","slices")]:
            parent=by_serial.get(item[field])
            if parent and (parent[0]!=table or (not item["flags"]&3 and not parent[1]["flags"] and not _contained(item,parent[1]))):
                raise ValueError("retained item has an invalid span parent")
        parent=by_serial.get(item["slice_serial"])
        if parent and (parent[0]!="slices" or parent[1]["parent_serial"]!=item["execute_serial"]):
            raise ValueError("item execution serial does not match its slice parent")

def validate_report(report):
    for key,expected in [("schema","vgx-coldtrace-1"),("time_unit","microseconds")]:
        if _get(report,key)!=expected:raise ValueError(f"unsupported report field: {key}")
    if _get(report,"complete") is not True or type(_get(report,"layout_version")) is not int or report["layout_version"] not in (2,3):
        raise ValueError("incomplete stream or unsupported recorder layout")
    version=report["layout_version"];phases=PHASES if version==2 else PHASES_V3
    if _get(report,"coverage")!=("warm_items_only" if version==2 else "owner_roots_and_backend_spans"):raise ValueError("unsupported coverage")
    if _get(report,"phase_names")!=list(phases):raise ValueError("unsupported phase table")
    if type(_get(report,"io_coverage_known")) is not bool:raise ValueError("invalid I/O coverage flag")
    for key in ("capture_start_us","items_seen","items_not_kept","total_us","counter_overflows","depth_overflows","unbalanced_scopes","inherited_items","phases_outside_items","orphan_shader_marks"):
        _integer(_get(report,key),0,U64,key)
    for key in ("io_dropped","clock_errors","foreign_dropped"):_integer(_get(report,key),0,U32,key)
    if version==3:
        for field in ("executions_seen","slices_seen","span_errors"):_integer(_get(report,field),0,U64,field)
        if _get(report,"origin_names")!=list(ORIGINS):raise ValueError("unsupported origin table")
    for value in _array(report,"aggregate_exclusive_us",len(phases),True):_integer(value,0,U64,"aggregate_exclusive_us")
    items=_array(report,"items",64)
    if len(items)>report["items_seen"]:raise ValueError("retained items exceed observed items")
    if not report["counter_overflows"] and (len(items)!=min(report["items_seen"],64) or report["items_not_kept"]!=report["items_seen"]-len(items)):
        raise ValueError("retention counts do not match the bounded worst-item set")
    for item in items:
        _validate_item(item,report,phases)
    if version==3:_validate_spans(report,phases)
    for io in _array(report,"io",4096):
        for field in ("start_us","end_us","requested"):_integer(_get(io,field),0,U64,field)
        _integer(_get(io,"result"),-(1<<63),(1<<63)-1,"I/O result")
        _integer(_get(io,"thread"),0,U32,"I/O thread");_integer(_get(io,"kind"),1,3,"I/O class")
        if type(_get(io,"result_noted")) is not bool or io["end_us"]<io["start_us"]:raise ValueError("invalid worker interval")
        if io["start_us"]<report["capture_start_us"]:raise ValueError("worker clock precedes capture start")
    return report

def _unique(pairs):
    result={}
    for key,value in pairs:
        if key in result:raise ValueError("duplicate JSON key")
        result[key]=value
    return result

def _nonfinite(_):
    raise ValueError("non-finite JSON number")

def load_report(path):
    with Path(path).open("rb") as stream:raw=stream.read(MAX_BYTES+1)
    if len(raw)>MAX_BYTES:raise ValueError("report exceeds the 2MiB cap")
    return validate_report(json.loads(raw.decode("ascii"),object_pairs_hook=_unique,parse_constant=_nonfinite))

def _overlap(intervals,start,end):
    clipped=sorted((max(start,io["start_us"]),min(end,io["end_us"])) for io in intervals if io["end_us"]>start and io["start_us"]<end)
    covered=start;total=0
    for a,b in clipped:
        if b>covered:total+=b-max(a,covered);covered=b
    return total

def summarize(report):
    validate_report(report)
    version=report["layout_version"];phases=PHASES if version==2 else PHASES_V3
    warnings=["Owner-root/backend execution wall times only, not FPS or complete game-turn/GPU measurements.",
              "Retained items are the slowest bounded set, not a representative timing distribution.",
              "Phase wall time does not separate CPU work, preemption and unwrapped waits.",
              "Observed worker-I/O overlap is a lower bound, not a cause or an amount to add to phases."]
    if version==2:warnings.append("Legacy v2 roots can include queued, direct and utility work; its warm-only coverage label is inaccurate.")
    else:warnings.append("Unclassified span gaps are wall time, not proof of CPU work, preemption or an idle renderer.")
    for field,description in [("phases_outside_items","wrapped owner calls fell outside explicit roots; "+("their time is unknown" if version==2 else "timed SDK roots were created")),
                              ("io_dropped","worker I/O intervals were dropped"),("clock_errors","clock errors were clamped"),
                              ("counter_overflows","counter overflows occurred"),("foreign_dropped","foreign calls or annotations were not timed"),
                              ("depth_overflows","scope depth overflows occurred"),("unbalanced_scopes","unbalanced scopes occurred"),
                              ("inherited_items","nested item requests inherited their parent key"),("orphan_shader_marks","orphan shader marks had no owning item")]:
        if report[field]:warnings.append(f"{report[field]} {description}.")
    if version==3 and report["span_errors"]:warnings.append(f"{report['span_errors']} invalid span lifetimes or nesting attempts occurred.")
    if not report["items"]:warnings.append("No owner roots were retained; this capture cannot attribute a stall to an SDK phase.")
    result=[]
    for item in sorted(report["items"],key=lambda value:value["total_us"],reverse=True):
        complete=not item["flags"]&3 and not any(report[field] for field in ("clock_errors","counter_overflows","depth_overflows","unbalanced_scopes"))
        times=dict(zip(phases,item["exclusive_us"]))
        times["unattributed_self"]=item["self_us"]
        largest=max(times.values());tied=[name for name,value in times.items() if value==largest] if complete and item["total_us"] else []
        result.append({"key":item["key"],"total_us":item["total_us"],"self_us":item["self_us"],"flags":item["flags"],
                       "phase_partition_valid":complete,"attribution_complete":False,"dominant_phase":tied[0] if len(tied)==1 else None,
                       "largest_exclusive_phases":tied,"largest_phase_share_bps":10000*largest//item["total_us"] if tied else None,
                       "exclusive_us":dict(zip(phases,item["exclusive_us"])),"shaders":item["shaders"],
                       "shader_marks_truncated":bool(item["flags"]&4 or item["shader_marks_dropped"]),"shader_marks_observed":len(item["shaders"]),
                       "observed_worker_overlap_us":_overlap(report["io"],item["start_us"],item["start_us"]+item["total_us"]),"overlap_complete":False})
        if version==3:result[-1].update(origin=ORIGINS[item["origin"]],execute_serial=item["execute_serial"],slice_serial=item["slice_serial"],
                                      execution_scope="inside_backend_execution" if item["execute_serial"] else "outside_backend_execution")
    counters={field:report[field] for field in ("io_dropped","clock_errors","counter_overflows","foreign_dropped","phases_outside_items","depth_overflows","unbalanced_scopes","inherited_items","orphan_shader_marks")}
    summary={"coverage":"owner_roots_legacy" if version==2 else report["coverage"],"attribution_complete":False,"capture_start_quiesced":report["io_coverage_known"],
            "items_seen":report["items_seen"],"items_not_kept":report["items_not_kept"],"counters":counters,"warnings":warnings,"items":result}
    if version==3:
        summary["counters"]["span_errors"]=report["span_errors"]
        parents={span["serial"] for span in report["executions"]}
        for table in ("executions","slices"):
            summary[table]=[]
            for span in sorted(report[table],key=lambda value:value["total_us"],reverse=True):
                summary[table].append({key:span[key] for key in ("serial","parent_serial","frame","total_us","root_us","self_us","unclassified_us","roots","flags","max_item")})
                summary[table][-1].update(exclusive_us=dict(zip(phases,span["exclusive_us"])),origin_us=dict(zip(ORIGINS,span["origin_us"])),
                    parent_retained=span["parent_serial"] in parents,attribution_complete=False,
                    phase_partition_valid=not span["flags"] and not any(report[field] for field in ("clock_errors","counter_overflows","depth_overflows","unbalanced_scopes","span_errors")))
        span=report["max_upload_execution"]
        summary["max_upload_execution"]=None if span is None else {
            **span,"exclusive_us":dict(zip(phases,span["exclusive_us"])),"origin_us":dict(zip(ORIGINS,span["origin_us"])),
            "attribution_complete":False,"phase_partition_valid":not span["flags"] and not any(report[field] for field in ("clock_errors","counter_overflows","depth_overflows","unbalanced_scopes","span_errors"))}
    return summary

def _ms(value):return f"{value//1000}.{value%1000:03}"

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report",type=Path);parser.add_argument("--json",action="store_true");parser.add_argument("--top",type=int,default=10)
    args=parser.parse_args()
    if not 1<=args.top<=64:parser.error("--top must be between 1 and 64")
    try:result=summarize(load_report(args.report))
    except (ValueError,OSError,RecursionError) as error:print(f"Invalid coldtrace report: {error}",file=sys.stderr);return 2
    if args.json:print(json.dumps(result,indent=2));return 0
    print(f"Owner roots: {result['items_seen']}; retained: {len(result['items'])}.")
    for warning in result["warnings"]:print(f"Note: {warning}")
    for table in ("slices","executions"):
        for span in result.get(table,[])[:args.top]:
            print(f"{table}: serial={span['serial']} parent={span['parent_serial']} wall_ms={_ms(span['total_us'])} roots_ms={_ms(span['root_us'])} unclassified_ms={_ms(span['unclassified_us'])} flags={span['flags']}")
    print("frame  kind  a:b  wall_ms  self_ms  observed_worker_overlap_ms  largest_exclusive_phase")
    for item in result["items"][:args.top]:
        key=item["key"]
        if not item["phase_partition_valid"]:phase="untrusted"
        elif not item["total_us"]:phase="zero-duration"
        else:phase=item["dominant_phase"] or "tie:"+"+".join(item["largest_exclusive_phases"])
        print(f"{key['frame']}  {key['kind']}  {key['a']}:{key['b']}  {_ms(item['total_us'])}  {_ms(item['self_us'])}  {_ms(item['observed_worker_overlap_us'])}  {phase}")
    return 0

if __name__=="__main__":raise SystemExit(main())
