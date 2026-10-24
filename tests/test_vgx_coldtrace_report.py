import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest

TOOL=Path(__file__).resolve().parents[1]/"tools/analyze_vgx_coldtrace.py"

def module():
    if not TOOL.is_file():
        raise AssertionError("offline coldtrace report analyzer is not implemented")
    spec=importlib.util.spec_from_file_location("coldtrace_analysis",TOOL)
    result=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result

def fixture(api):
    zeros=[0]*17
    item={"key":{"frame":1,"kind":1,"a":2,"b":3,"layout":4,"blend":5},"start_us":100,"total_us":208000,"self_us":18000,"flags":0,"shader_marks_dropped":0,
          "inclusive_us":zeros.copy(),"exclusive_us":zeros.copy(),"calls":zeros.copy(),"bytes":zeros.copy(),"requested":zeros.copy(),"first_error":zeros.copy(),"failures":zeros.copy(),"short_results":zeros.copy(),
          "shaders":[{"shader":7,"kind":2,"source_hash":"FFFFFFFFFFFFFFFF"}]}
    for name,value in [("compile",20000),("read",50000),("shark",100000),("finish",20000)]:
        item["exclusive_us"][api.PHASES.index(name)]=value
    item["inclusive_us"]=item["exclusive_us"].copy();item["inclusive_us"][0]=170000
    fields=["counter_overflows","depth_overflows","unbalanced_scopes","inherited_items","phases_outside_items","orphan_shader_marks","io_dropped","clock_errors","foreign_dropped"]
    result={name:0 for name in fields}
    result.update({"schema":"vgx-coldtrace-1","coverage":"warm_items_only","time_unit":"microseconds","layout_version":2,"complete":True,"capture_start_us":0,
        "items_seen":1,"items_not_kept":0,"total_us":208000,"io_coverage_known":False,"phase_names":list(api.PHASES),"aggregate_exclusive_us":item["exclusive_us"].copy(),"items":[item],
        "io":[{"start_us":0,"end_us":300,"result":7,"requested":9,"thread":2,"kind":1,"result_noted":True},
              {"start_us":200,"end_us":400,"result":3,"requested":9,"thread":3,"kind":2,"result_noted":True}]})
    return result

def fixture_v3(api):
    report=fixture(api)
    report.update(layout_version=3,coverage="owner_roots_and_backend_spans",executions_seen=1,slices_seen=1,span_errors=0,
                  phase_names=list(api.PHASES)+["transfer_finish"],
                  origin_names=["unknown","queued","direct_shader","direct_program","utility","texture","stream","present","sdk"])
    report["aggregate_exclusive_us"].append(0)
    item=report["items"][0]
    for name in ["inclusive_us","exclusive_us","calls","bytes","requested","first_error","failures","short_results"]:item[name].append(0)
    item.update(origin=1,execute_serial=1,slice_serial=2)
    span={"serial":1,"parent_serial":0,"frame":1,"start_us":0,"total_us":210000,"root_us":208000,"self_us":18000,
          "unclassified_us":2000,"roots":1,"flags":0,"exclusive_us":item["exclusive_us"].copy(),
          "origin_us":[0,208000,0,0,0,0,0,0,0],"max_item":copy.deepcopy(item)}
    report["executions"]=[span];report["max_upload_execution"]=None
    report["slices"]=[copy.deepcopy(span)]
    report["slices"][0].update(serial=2,parent_serial=1,start_us=50,total_us=209000,unclassified_us=1000)
    return report

class ColdtraceReportTests(unittest.TestCase):
    def setUp(self):
        self.api=module();self.report=fixture(self.api)
    def test_overlap_is_union_not_double_counted_or_added_to_phases(self):
        summary=self.api.summarize(self.report)
        item=summary["items"][0]
        self.assertEqual(item["observed_worker_overlap_us"],300)
        self.assertEqual(item["total_us"],208000)
        self.assertEqual(item["dominant_phase"],"shark")
        self.assertFalse(item["overlap_complete"])
        self.assertTrue(item["phase_partition_valid"])
    def test_partial_item_is_not_given_a_causal_phase_label(self):
        self.report["items"][0]["flags"]=1
        item=self.api.summarize(self.report)["items"][0]
        self.assertFalse(item["phase_partition_valid"])
        self.assertIsNone(item["dominant_phase"])
    def test_outside_calls_and_drops_are_visible(self):
        self.report["phases_outside_items"]=17;self.report["io_dropped"]=9
        warnings=" ".join(self.api.summarize(self.report)["warnings"])
        self.assertIn("17",warnings);self.assertIn("9",warnings);self.assertIn("not FPS",warnings)
    def test_exact_64_bit_errors_and_hashes(self):
        self.report["items"][0]["first_error"][5]=-(1<<63)
        self.report["io"][0]["result"]=-(1<<63)
        self.api.validate_report(self.report)
        self.assertEqual(self.api.summarize(self.report)["items"][0]["shaders"][0]["source_hash"],"FFFFFFFFFFFFFFFF")
    def test_bad_arrays_counts_values_and_partitions_are_rejected(self):
        variants=[]
        bad=copy.deepcopy(self.report);bad["items"][0]["exclusive_us"].pop();variants.append(bad)
        bad=copy.deepcopy(self.report);bad["items"][0]["self_us"]+=1;variants.append(bad)
        bad=copy.deepcopy(self.report);bad["io"][0]["start_us"]=401;variants.append(bad)
        bad=copy.deepcopy(self.report);bad["items"][0]["key"]["frame"]=True;variants.append(bad)
        bad=copy.deepcopy(self.report);bad["items"][0]["total_us"]=1<<64;variants.append(bad)
        bad=copy.deepcopy(self.report);bad["items"][0]["shaders"][0]["source_hash"]="../unsafe";variants.append(bad)
        bad=copy.deepcopy(self.report);bad["complete"]=False;variants.append(bad)
        bad=copy.deepcopy(self.report);bad["layout_version"]=2.0;variants.append(bad)
        for invalid in variants:
            with self.subTest(invalid=variants.index(invalid)):
                with self.assertRaises(ValueError):self.api.validate_report(invalid)
    def test_empty_capture_does_not_invent_fps_or_samples(self):
        self.report["items"]=[];self.report["items_seen"]=0;self.report["total_us"]=0;self.report["aggregate_exclusive_us"]=[0]*17
        result=self.api.summarize(self.report)
        self.assertEqual(result["items"],[]);self.assertIn("No owner roots", " ".join(result["warnings"]))
    def test_no_source_marks_does_not_claim_complete_shader_identity(self):
        self.report["items"][0]["shaders"]=[]
        item=self.api.summarize(self.report)["items"][0]
        self.assertNotIn("source_identity_complete",item)
        self.assertFalse(item["shader_marks_truncated"])
        self.assertEqual(item["shader_marks_observed"],0)
    def test_all_scope_and_identity_diagnostics_remain_visible(self):
        self.report.update(depth_overflows=5,unbalanced_scopes=3,inherited_items=2,orphan_shader_marks=4)
        result=self.api.summarize(self.report);item=result["items"][0]
        self.assertFalse(item.get("phase_partition_valid",item.get("timing_complete")))
        self.assertFalse(item.get("attribution_complete",True))
        warnings=" ".join(result["warnings"])
        for word in ["depth","unbalanced","inherited","orphan"]:self.assertIn(word,warnings)
    def test_known_start_does_not_make_worker_overlap_complete(self):
        self.report["io_coverage_known"]=True
        result=self.api.summarize(self.report)
        self.assertTrue(result.get("capture_start_quiesced",False))
        self.assertFalse(result["items"][0]["overlap_complete"])
    def test_misclassified_worker_io_drop_need_not_fill_the_ring(self):
        self.report["io"]=[];self.report["io_dropped"]=1
        self.api.validate_report(self.report)
        self.assertFalse(self.api.summarize(self.report)["items"][0]["overlap_complete"])
    def test_retention_count_mismatch_is_rejected(self):
        self.report["items_seen"]=1000
        with self.assertRaises(ValueError):self.api.validate_report(self.report)
    def test_nonzero_clock_epoch_is_shared_and_checked(self):
        offset=1000000;self.report["capture_start_us"]=offset
        self.report["items"][0]["start_us"]+=offset
        for io in self.report["io"]:io["start_us"]+=offset;io["end_us"]+=offset
        self.assertEqual(self.api.summarize(self.report)["items"][0]["observed_worker_overlap_us"],300)
        self.report["io"][0]["start_us"]=0
        with self.assertRaises(ValueError):self.api.validate_report(self.report)
    def test_tied_phases_do_not_get_an_arbitrary_dominant_label(self):
        item=self.report["items"][0];item["exclusive_us"]=[0]*17;item["exclusive_us"][0]=100000;item["exclusive_us"][2]=100000;item["self_us"]=8000
        result=self.api.summarize(self.report)["items"][0]
        self.assertIsNone(result["dominant_phase"])
        self.assertEqual(result.get("largest_exclusive_phases"),["compile","shark"])
    def test_clock_overflow_and_item_flags_are_conservative(self):
        for field in ["clock_errors","counter_overflows"]:
            report=copy.deepcopy(self.report);report[field]=1
            self.assertFalse(self.api.summarize(report)["items"][0]["phase_partition_valid"])
        for flag in [1,2]:
            report=copy.deepcopy(self.report);report["items"][0]["flags"]=flag
            self.assertFalse(self.api.summarize(report)["items"][0]["phase_partition_valid"])
        self.report["items"][0]["flags"]=4
        self.assertTrue(self.api.summarize(self.report)["items"][0]["shader_marks_truncated"])
    def test_cli_json_and_invalid_report_exit_code(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"report.json";path.write_text(json.dumps(self.report),encoding="ascii")
            result=subprocess.run([sys.executable,str(TOOL),str(path),"--json"],capture_output=True,text=True)
            self.assertEqual(result.returncode,0);self.assertFalse(json.loads(result.stdout)["attribution_complete"])
            path.write_text(json.dumps(self.report)+" trailing",encoding="ascii")
            result=subprocess.run([sys.executable,str(TOOL),str(path)],capture_output=True,text=True)
            self.assertEqual(result.returncode,2);self.assertIn("Invalid coldtrace report",result.stderr)
    def test_json_loader_rejects_duplicates_nonfinite_and_oversize(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"report.json"
            path.write_text(json.dumps(self.report),encoding="ascii")
            self.assertEqual(self.api.load_report(path)["items_seen"],1)
            for text in ["{\"schema\":1,\"schema\":2}","{\"x\":NaN}",json.dumps(self.report)+" trailing bytes"," "*(self.api.MAX_BYTES+1)]:
                path.write_text(text,encoding="ascii")
                with self.assertRaises(ValueError):self.api.load_report(path)
    def test_v3_complete_spans_include_gaps_without_claiming_cpu_or_fps(self):
        report=fixture_v3(self.api)
        result=self.api.summarize(report)
        self.assertEqual(result["slices"][0]["total_us"],209000)
        self.assertEqual(result["slices"][0]["unclassified_us"],1000)
        self.assertTrue(result["executions"][0]["phase_partition_valid"])
        self.assertFalse(result["slices"][0]["attribution_complete"])
        self.assertEqual(result["items"][0]["origin"],"queued")
        self.assertIn("transfer_finish",result["executions"][0]["exclusive_us"])
        self.assertIn("not FPS"," ".join(result["warnings"]))
    def test_v3_parent_may_be_absent_from_independent_worst_set(self):
        report=fixture_v3(self.api);report["executions_seen"]=17
        empty=copy.deepcopy(report["executions"][0])
        empty.update(roots=0,root_us=0,self_us=0,unclassified_us=210000,max_item=None,exclusive_us=[0]*18,origin_us=[0]*9)
        report["executions"]=[]
        for serial in range(3,19):
            span=copy.deepcopy(empty);span.update(serial=serial,start_us=serial*1000000)
            report["executions"].append(span)
        result=self.api.summarize(report)
        self.assertFalse(result["slices"][0]["parent_retained"])
        self.assertTrue(result["slices"][0]["phase_partition_valid"])
    def test_v3_rejects_bad_span_partitions_parents_and_max_root(self):
        report=fixture_v3(self.api);variants=[]
        for field in ["root_us","unclassified_us","self_us"]:
            bad=copy.deepcopy(report);bad["slices"][0][field]+=1;variants.append(bad)
        bad=copy.deepcopy(report);bad["slices"][0]["origin_us"][0]=1;variants.append(bad)
        bad=copy.deepcopy(report);bad["slices"][0]["parent_serial"]=99;variants.append(bad)
        bad=copy.deepcopy(report);bad["slices"][0]["max_item"]["slice_serial"]=1;variants.append(bad)
        bad=copy.deepcopy(report);bad["slices"][0]["max_item"]["total_us"]=300000;variants.append(bad)
        bad=copy.deepcopy(report);bad["items"][0]["origin"]=9;variants.append(bad)
        bad=copy.deepcopy(report);bad["slices_seen"]=17;variants.append(bad)
        bad=copy.deepcopy(report);bad["slices"][0]["serial"]=1;variants.append(bad)
        for i,invalid in enumerate(variants):
            with self.subTest(case=i):
                with self.assertRaises(ValueError):self.api.validate_report(invalid)
    def test_v3_partial_or_malformed_span_does_not_get_trusted_attribution(self):
        for field,flag in [("flags",1),("flags",2)]:
            report=fixture_v3(self.api);report["slices"][0][field]=flag
            self.assertFalse(self.api.summarize(report)["slices"][0]["phase_partition_valid"])
        report=fixture_v3(self.api);report["span_errors"]=1
        self.assertFalse(self.api.summarize(report)["slices"][0]["phase_partition_valid"])
    def test_legacy_v2_label_is_not_repeated_as_warm_only_evidence(self):
        result=self.api.summarize(self.report)
        self.assertEqual(result["coverage"],"owner_roots_legacy")
        self.assertIn("direct"," ".join(result["warnings"]))
    def test_v3_flags_do_not_excuse_impossible_gap_or_self_arithmetic(self):
        variants=[]
        bad=fixture_v3(self.api);bad["slices"][0].update(flags=1,unclassified_us=999);variants.append(bad)
        bad=fixture_v3(self.api);bad["slices"][0].update(flags=2,root_us=300000,unclassified_us=1);variants.append(bad)
        bad=fixture_v3(self.api);bad["slices"][0].update(flags=1,self_us=300000);variants.append(bad)
        for i,invalid in enumerate(variants):
            with self.subTest(case=i):
                with self.assertRaises(ValueError):self.api.validate_report(invalid)
    def test_v3_retained_slice_checks_item_execution_pair_even_for_idle(self):
        for target in ["item","maximum"]:
            report=fixture_v3(self.api)
            item=report["items"][0] if target=="item" else report["slices"][0]["max_item"]
            item["execute_serial"]=0
            with self.subTest(target=target):
                with self.assertRaises(ValueError):self.api.validate_report(report)
        report=fixture_v3(self.api);report["slices"][0].update(parent_serial=0,frame=0)
        with self.assertRaises(ValueError):self.api.validate_report(report)

if __name__=="__main__":unittest.main()
