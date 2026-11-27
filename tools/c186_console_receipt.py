#!/usr/bin/env python3
"""Extract allowlisted C186 observations from copied logs. Never contacts a device."""

import argparse
import json
import re
from pathlib import Path


STAMP = re.compile(r"^[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3} ")
BANNER = "Super Mario Galaxy Vita, basic log:"
PLATFORMS = ("unknown", "console", "emulator", "host")
STAGES = {"17bab031": "HoneyBeeKingdomGalaxy", "4903d024": "AstroGalaxy"}
POSE_MODELS = {"Tico", "LotusLeaf", "Flower", "FlowerBlue"}
D = r"([0-9]{1,20})"
FRAME = re.compile(
    rf"first-use: frame {D} in play, (?:{D} ms after a pad input|no pad input in the last {D} ms), "
    r"([0-9]{1,12}\.[0-9]) s after the screen opened: "
    rf"{D} ms in the back end: programs linked by draws {D} \({D} ms; shader compiler {D} runs, {D} ms\), "
    rf"render targets made {D}(?: [0-9]+x[0-9]+ \([^()]*\)){{0,4}}, texture uploads {D} \({D} KiB, {D} ms\)"
    r"(?:; recorded in the player list \(GXV_PLAYERWARM\))?"
)
SUMMARY = re.compile(
    rf"first-use: stage ([0-9a-fA-F]{{8}}) (periodic|left), {D} s after the screen opened: still paid in play: {D} frames; "
    rf"programs linked by draws {D} \({D} ms; {D} of them within {D} ms of a pad input = {D} ms; "
    rf"shader compiler {D} runs {D} ms\), render targets made {D}, texture uploads {D} \({D} KiB, {D} ms\), "
    rf"worst such frame {D} ms; player list: {D} links after a pad input, {D} records added this stage"
)
NET = re.compile(
    rf"\[net-https\] [A-Z]+ \S+:[0-9]+ -> ([A-Z_]+)(?: \((.*)\))? status={D} tls=(?:1\.2|1\.3|-)"
    rf"(?: reused)? {D}ms [0-9]+B"
)
SOCKETS = re.compile(
    rf"\[net-https\] sockets: {D} open \({D} epoll\), {D} opened and {D} closed so far, most at once {D}, refused {D}"
)
RESTART = {
    "[mp] settings: restart asked (pause)": "asked_from_pause",
    "[mp] settings: restart requested": "requested",
    "[mp] settings: restart requested (Back after the question)": "requested_via_back",
    "[mp] settings: restart declined (pause)": "declined_from_pause",
    "[mp] settings: restart now": "attempted",
}


def numbers(keys, values):
    return {key: int(value) if value is not None else None for key, value in zip(keys.split(), values)}


def session():
    return {
        "boot_banner_seen": False, "build": None, "protocol": None, "picture_modes": [],
        "stage_changes": [], "far_levels": [], "restored_pose_markers": [],
        "first_use_frames": [], "first_use_stage_snapshots": [],
        "unparsed_first_use_rows": 0, "restart_events": [], "https_requests": [], "socket_snapshots": [],
    }


def summarize(lines):
    """Return independent sessions. Source line numbers are local to the supplied log."""
    result = []
    current = session()
    context = None
    for n, raw in enumerate(lines, 1):
        line = STAMP.sub("", raw.rstrip("\r\n"), count=1)
        if line.startswith(BANNER):
            if n != 1 and (current["boot_banner_seen"] or any(current.values())):
                result.append(current)
                current = session()
                context = None
            current["boot_banner_seen"] = True
        elif m := re.fullmatch(r"\[mp\] core ready build=(\S+) proto=([0-9]{1,4})", line):
            if current["build"] is not None:
                raise ValueError("more than one core boot without a separating boot banner; supply separate launch logs")
            current["build"] = m[1] if m[1] in {"3.0-C186", "3.0-C186.1", "3.0-C186.2"} else "other"
            current["protocol"] = int(m[2])
        elif m := re.match(r"settings: picture mode (Quality \(30\)|Performance \(60\)) \(GXV_PICTURE_MODE=(30|60)\):", line):
            current["picture_modes"].append({"line": n, "selected": m[1].split()[0], "effective_mode_key": int(m[2])})
        elif m := re.match(r"mode: (30|60), stage ([^:,]+)(?::(-?[0-9]{1,6}))?, preset keys applied:", line):
            stage = m[2] if m[2] in STAGES.values() else "other"
            new = {"stage": stage, "scenario": int(m[3]) if m[3] else None}
            if new != context:
                current["stage_changes"].append({"line": n, **new})
                context = new
        elif m := re.match(r"farlogic: level ([0-3])(?: now \(was -?[0-9]+\), dry |, [0-9]+\.[0-9]+ s, [0-9]+ turns:)", line):
            current["far_levels"].append({"line": n, "last_stage_context": context, "level": int(m[1])})
        elif m := re.fullmatch(r"far-pose-test: restored first level-3 half-rate pose: (.+)", line):
            current["restored_pose_markers"].append({
                "line": n, "last_stage_context": context, "model": m[1] if m[1] in POSE_MODELS else "other",
            })
        elif m := FRAME.fullmatch(line):
            current["first_use_frames"].append({
                "line": n, "last_stage_context": context, "seconds_after_open": float(m[4]),
                **numbers("frame input_age_ms no_input_window_ms", m.groups()[:3]),
                **numbers("backend_ms draw_links draw_link_ms compiler_runs compiler_ms targets uploads upload_kib upload_ms", m.groups()[4:]),
            })
        elif m := SUMMARY.fullmatch(line):
            current["first_use_stage_snapshots"].append({
                "line": n, "stage": STAGES.get(m[1].lower(), "other"), "reason": m[2],
                **numbers("seconds_after_open frames draw_links draw_link_ms links_near_input input_window_ms link_ms_near_input "
                          "compiler_runs compiler_ms targets uploads upload_kib upload_ms worst_frame_ms player_links player_records",
                          m.groups()[2:]),
            })
        elif re.match(r"first-use: (?:frame |stage [0-9a-fA-F]+ (?:periodic|left),)", line):
            current["unparsed_first_use_rows"] += 1
        elif line in RESTART:
            current["restart_events"].append({"line": n, "event": RESTART[line]})
        elif m := re.fullmatch(r"settings: restart returned 0x([0-9a-fA-F]{8}); saved choices remain next-launch only", line):
            current["restart_events"].append({"line": n, "event": "relaunch_call_returned", "return_code_u32": int(m[1], 16)})
        elif m := NET.fullmatch(line):
            # np_https logs method and host, not the application endpoint.
            current["https_requests"].append({"line": n, "status": int(m[3]), "elapsed_ms": int(m[4]), "transport_ok": m[1] == "OK",
                                              "no_data_timeout": (m[1], m[2]) == ("TIMEOUT", "no data for too long")})
        elif m := SOCKETS.fullmatch(line):
            current["socket_snapshots"].append({"line": n, **numbers("open epoll opened closed peak refused", m.groups())})
    return [*result, current]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=PLATFORMS, default="unknown",
                        help="operator-declared origin for all supplied files; never inferred")
    parser.add_argument("logs", nargs="+", type=Path, help="one or more local copies, in desired order")
    args = parser.parse_args()
    files = []
    for i, path in enumerate(args.logs, 1):
        try:
            with path.open(encoding="utf-8", errors="replace") as src:
                files.append({"input": i, "sessions": summarize(src)})
        except OSError:
            parser.exit(2, f"cannot read input {i}\n")
        except ValueError as error:
            parser.exit(2, f"input {i}: {error}\n")
    print(json.dumps({
        "operator_declared_platform": args.platform, "inputs": files,
        "scope": "Observations only. Unkeyed events use the last logged stage context. Frame lines may be capped; "
                 "stage snapshots are cumulative and are not additional events. The pose marker is once per launch. "
                 "Transport lines do not identify endpoints. A missing row is not a zero. No visual, hardware, "
                 "retail-profile, restart-completion, route, or performance verdict is inferred.",
    }, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
