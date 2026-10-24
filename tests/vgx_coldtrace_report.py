import json
import sys

with open(sys.argv[1],encoding="ascii") as stream:
    report=json.load(stream)
if len(sys.argv)>2:
    assert len(report["io"])==4096 and report["io_dropped"]==11 and report["complete"] is True
    print("PASS: capped4096worker intervals remain valid JSON with explicit overflow")
    raise SystemExit(0)
assert report["schema"]=="vgx-coldtrace-1" and report["layout_version"]==3
assert report["coverage"]=="owner_roots_and_backend_spans" and report["complete"] is True
assert report["items_seen"]==1 and len(report["items"])==1
assert report["io_coverage_known"] is False
assert len(report["phase_names"])==18 and report["phase_names"][-1]=="transfer_finish"
item=report["items"][0]
assert item["total_us"]==208000 and item["self_us"]==18000
assert sum(item["exclusive_us"])+item["self_us"]==item["total_us"]
phases={name:i for i,name in enumerate(report["phase_names"])}
assert item["inclusive_us"][phases["compile"]]==170000
assert item["exclusive_us"][phases["compile"]]==20000
assert item["first_error"][phases["seek"]]==-4294967298
assert item["shaders"][0]["source_hash"]=="FFFFFFFFFFFFFFFF"
assert item["key"]=={"frame":17,"kind":1,"a":9,"b":2,"layout":7,"blend":11}
assert item["origin"]==1 and item["execute_serial"]==1 and item["slice_serial"]==2
assert report["executions_seen"]==1 and report["slices_seen"]==1
execute=report["executions"][0];slice=report["slices"][0]
assert execute["serial"]==1 and execute["parent_serial"]==0
assert slice["serial"]==2 and slice["parent_serial"]==1
for span in [execute,slice]:
    assert span["frame"]==17 and span["total_us"]==208000 and span["root_us"]==208000 and span["unclassified_us"]==0
    assert span["self_us"]==18000 and sum(span["exclusive_us"])+span["self_us"]==span["root_us"]
    assert span["origin_us"]==[0,208000,0,0,0,0,0,0,0] and span["max_item"]==item
assert report["io"]==[{"start_us":20000,"end_us":80000,"result":700,"requested":900,"thread":2,"kind":1,"result_noted":True}]
print("PASS: standard JSON parser independently verifies 208ms phases, exact source identity and worker intervals")
