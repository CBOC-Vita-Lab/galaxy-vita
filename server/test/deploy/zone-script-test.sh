#!/bin/bash
# Offline test of scripts/setup-psvita-zone.sh: curl is a stub on PATH (a small stateful mock of the Cloudflare zone, Rulesets API and bot settings);
# nothing is contacted. The zone is SHARED by the games on psvita.dev: ONE path-form rate-limiting rule (ref psvita-shared-ratelimit) for all of them,
# a WAF rule that is always scoped to galaxy.psvita.dev. Checks: empty phases get both rules; a phase with someone else's rule keeps it; a phase that already holds ours is left alone
# (no write call at all, so a second run changes nothing); a missing permission stops with a clear message before any write; the Free plan's ONE
# rate-limiting rule being someone else's stops everything; a changed rule is updated in place; --remove deletes only ours; --verify-only writes
# nothing; the account / zone pins; the token and account id never reach a log, argument, environment, temp file or the console.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRV=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/tmp" "$T/seen"
. "$HERE/portability-check.sh"
# MAC_SHIM=1: run the whole test under a PATH whose stat/readlink/date/grep/... refuse the GNU-only forms (a stand-in for the Mac's userland)
if [ -n "${MAC_SHIM:-}" ]; then make_mac_shim "$T/shim"; export PATH="$T/shim:$PATH" MOCK_NO_PROC=1; fi
ACCT=0123456789abcdef0123456789abcdef
ZONE=fedcba9876543210fedcba9876543210
OTHERACCT=ffffffffffffffffffffffffffffffff
SECRET="tok_a.b+c/d=e~f-$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"   # (the characters the script allows, with regex-special ones among them)
printf '%s\n' "$SECRET" > "$T/token"; chmod 600 "$T/token"
cat > "$T/bin/curl" <<'S'
#!/usr/bin/env python3
# mock of api.cloudflare.com/client/v4 (zone psvita.dev); state in $MOCK_STATE (json); every call logged without the token
import sys, os, json, re
args = sys.argv[1:]
cfg = sys.stdin.read()
url = ""
with open(os.environ["MOCK_DIR"] + "/seen/curl-args", "a") as f: f.write("curl " + " ".join(args) + "\n")
with open(os.environ["MOCK_DIR"] + "/seen/curl-env", "a") as f: f.write(repr(sorted(os.environ.items())) + "\n")
method = "GET"; body = None; auth = False; tokv = ""
for line in cfg.splitlines():
    m = re.match(r'(\w[\w-]*) = "(.*)"$', line)
    if not m: continue
    k, v = m.groups()
    if k == "url": url = v
    elif k == "request": method = v
    elif k == "data-binary": body = json.loads(v.replace('\\"', '"').replace('\\\\', '\\'))
    elif k == "header" and v.startswith("Authorization: Bearer "):
        auth = True
        tokv = v[len("Authorization: Bearer "):]
        with open(os.environ["MOCK_DIR"] + "/seen/hdr-stdin", "a") as f: f.write(v + "\n")
path = url.split("/client/v4", 1)[1]
st = json.load(open(os.environ["MOCK_STATE"]))
def save(): json.dump(st, open(os.environ["MOCK_STATE"], "w"))
def log(m): open(os.environ["MOCK_DIR"] + "/calls.log", "a").write(m + "\n")
log(method + " " + path.split("?")[0])
# where curl's stderr goes: find the file by (st_dev, st_ino) of fd 2 among the script's private temp dirs (no /proc: portable)
if os.environ.get("MOCK_NO_PROC"):
    _ro = os.readlink
    def _nr(p, *a, **k):
        if str(p).startswith("/proc"): raise OSError("no /proc (mac shim)")
        return _ro(p, *a, **k)
    os.readlink = _nr
try:
    import glob
    fs = os.fstat(2)
    for ep in glob.glob(os.environ["TMPDIR"] + "/zone-raw.*/curl-error"):
        es = os.stat(ep)
        if (es.st_dev, es.st_ino) == (fs.st_dev, fs.st_ino):
            d = os.stat(os.path.dirname(ep))
            import stat as _stat
            open(os.environ["MOCK_DIR"] + "/seen/rawdir", "a").write(ep + " " + _stat.filemode(d.st_mode) + "\n")
except Exception: pass
def trunc(key):
    # a truncated (invalid JSON) reply that holds an e-mail address and a 32-hex run; "key" or "key:CODE" in st["trunc"]
    for t in st.get("trunc") or []:
        k, _, c = t.partition(":")
        if k == key:
            sys.stdout.write('{"success":true,"result":{"id":"9f8e7d6c5b4a39281706f5e4d3c2b1a0","rules":[{"description":"mail me at leak.person@example.com and 9f8e7d6c5b4a39281706f5e4d3c2b1a0 then' + "\n" + (c or "200")); sys.exit(0)
def out(code, obj):
    sys.stdout.write(json.dumps(obj) + "\n" + str(code)); save(); sys.exit(0)
def refuse_host():
    if st.get("refuse_host_ratelimit") == "403": err(403, 20120, "The http.host field is not available on your plan")
    err(400, 20217, "invalid expression: field http.host is not allowed on this plan")
def err(code, c, msg):
    if st.get("echo_token"): msg = msg + " (request token " + tokv + ")"
    if st.get("echo_stderr"): sys.stderr.write("curl: (35) diagnostic with token " + tokv + " and owner@example.com\n")
    out(code, {"success": False, "errors": [{"code": c, "message": msg}], "result": None})
if not auth: err(401, 10000, "no token")
if path.startswith("/zones?"):
    trunc("zones")
    if st.get("zones_fail"): err(403, 9109, "Unauthorized to access requested resource")
    z = st["zone"]
    res = [] if z is None else [{"id": z["id"], "name": "psvita.dev", "status": z["status"], "account": {"id": z["account"], "name": "Acme Hat Company"}, "owner": {"email": "owner@example.com"}}]
    out(200, {"success": True, "result": res})
m = re.match(r"/zones/(\w+)/settings/browser_check$", path)
if m:
    trunc("bic")
    if st.get("bic") is None: err(403, 9109, "Unauthorized")
    out(200, {"success": True, "result": {"id": "browser_check", "value": st["bic"]}})
m = re.match(r"/zones/(\w+)/bot_management$", path)
if m and method == "PUT":
    if st.get("bot_write") == "no": err(403, 9109, "Unauthorized")
    st["bot"] = body["fight_mode"]; out(200, {"success": True, "result": {"fight_mode": st["bot"]}})
m = re.match(r"/zones/(\w+)/bot_management$", path)
if m:
    trunc("bot")
    if st.get("bot") is None: err(403, 9109, "Unauthorized")
    out(200, {"success": True, "result": {"fight_mode": st["bot"]}})
if re.match(r"/zones/(\w+)/rulesets$", path) and method == "POST":
    if st.get("rulesets_perm") == "read": err(403, 9109, "Unauthorized to write")
    if st.get("rulesets_perm") == "plan": err(403, 20120, "This feature is not available on your plan")
    ph = body["phase"]
    if st.get("noanswer_rl_write") and ph == "http_ratelimit": sys.exit(7)
    if st.get("refuse_rl_write") and ph == "http_ratelimit": err(400, 20217, "invalid expression: the rate limit fields are refused")
    if ph in st["phases"]: err(400, 20002, "a ruleset for this phase already exists")
    if st.get("refuse_host_ratelimit") and ph == "http_ratelimit" and any("http.host" in r.get("expression", "") for r in body["rules"]):
        refuse_host()
    for r in body["rules"]: r["id"] = "r" + str(st["n"]); st["n"] += 1
    st["phases"][ph] = {"id": "rs_" + ph, "rules": body["rules"]}
    out(200, {"success": True, "result": st["phases"][ph]})
m = re.match(r"/zones/(\w+)/rulesets/phases/(\w+)/entrypoint$", path)
if m:
    ph = m.group(2)
    if method == "GET": trunc("waf" if ph == "http_request_firewall_custom" else "rl")
    if st.get("rulesets_perm") == "notallowed": err(403, 9109, "This token is not allowed to read this ruleset")
    if st.get("rulesets_perm") == "none": err(403, 9109, "Unauthorized to access requested resource owner@example.com")
    if method == "GET":
        p = st["phases"].get(ph)
        if p is None: err(404, 10003, "could not find entrypoint ruleset")
        out(200, {"success": True, "result": p})
    if method == "PUT":
        if st.get("rulesets_perm") == "read": err(403, 9109, "Unauthorized to write")
        for r in body["rules"]: r["id"] = "r" + str(st["n"]); st["n"] += 1
        if st.get("refuse_host_ratelimit") and ph == "http_ratelimit" and any("http.host" in r.get("expression", "") for r in body["rules"]):
            refuse_host()
        st["phases"][ph] = {"id": "rs_" + ph, "rules": body["rules"]}
        out(200, {"success": True, "result": st["phases"][ph]})
m = re.match(r"/zones/(\w+)/rulesets/(\w+)/rules(?:/(\w+))?$", path)
if m:
    if st.get("rulesets_perm") == "read": err(403, 9109, "Unauthorized to write")
    if st.get("rulesets_perm") == "plan": err(403, 20120, "This feature is not available on your plan")
    rs, rid = m.group(2), m.group(3)
    ph = rs[3:]; p = st["phases"][ph]
    if st.get("noanswer_rl_write") and ph == "http_ratelimit" and method in ("POST", "PATCH"): sys.exit(7)
    if st.get("refuse_rl_write") and ph == "http_ratelimit" and method in ("POST", "PATCH"): err(400, 20217, "invalid expression: the rate limit fields are refused")
    if st.get("refuse_host_ratelimit") and ph == "http_ratelimit" and body and "http.host" in body.get("expression", ""):
        refuse_host()
    if method == "POST":
        body["id"] = "r" + str(st["n"]); st["n"] += 1; p["rules"].append(body); out(200, {"success": True, "result": p})
    if method == "PATCH":
        for i, r in enumerate(p["rules"]):
            if r["id"] == rid: body["id"] = rid; p["rules"][i] = body
        out(200, {"success": True, "result": p})
    if method == "DELETE":
        p["rules"] = [r for r in p["rules"] if r["id"] != rid]; out(200, {"success": True, "result": p})
err(404, 7003, "no route " + path)
S
chmod +x "$T/bin/curl"
# sed stub: every time the scrubber runs, no kept file under a log folder may hold raw remote text yet (the mock's echo_stderr writes this marker to curl's stderr)
REAL_SED=$(command -v sed)
cat > "$T/bin/sed" <<S
#!/bin/bash
if grep -rqs "owner@example.com" "\$MOCK_DIR"/log* 2>/dev/null; then echo "raw text in a log folder when the scrubber ran" >> "\$MOCK_DIR/leak.log"; fi
exec $REAL_SED "\$@"
S
chmod +x "$T/bin/sed"
export PATH="$T/bin:$PATH" MOCK_DIR="$T" MOCK_STATE="$T/state.json" TMPDIR="$T/tmp"
ACCT_ARG=(--account-id $ACCT --token-file "$T/token")
fail=0; NOK=0; NFAIL=0
echo 0 > "$T/runn"
check() { RUNN=$(cat "$T/runn"); if eval "$2"; then echo "ok   $1"; NOK=$((NOK + 1)); else echo "FAIL $1"; fail=1; NFAIL=$((NFAIL + 1)); tail -5 "$T/out$RUNN" 2>/dev/null | sed 's/^/     /'; fi; }
state() { # $1 = json fragment overrides (python dict literal)
  python3 - "$1" <<'P'
import sys, json, os
st = {"zone": {"id": "fedcba9876543210fedcba9876543210", "status": "active", "account": "0123456789abcdef0123456789abcdef"}, "phases": {}, "n": 1, "bot": False, "bic": "on"}
st.update(json.loads(sys.argv[1] or "{}"))
json.dump(st, open(os.environ["MOCK_STATE"], "w"))
P
  rm -f "$T/calls.log"
}
run() { local RUNN; RUNN=$(( $(cat "$T/runn") + 1 )); echo $RUNN > "$T/runn"; rm -f "$T/calls.log"
  local launch=(bash)
  if [ -n "${RUN_PID_FILE:-}" ]; then
    # Record the shell's PID, then replace it with the zone script.
    launch=(bash -c 'printf "%s\n" "$$" > "$1" || exit 1; shift; exec bash "$@"' bash "$RUN_PID_FILE")
  fi
  (cd "$SRV" && "${launch[@]}" scripts/setup-psvita-zone.sh "${ACCT_ARG[@]}" --log-dir "$T/log$RUNN" "$@") > "$T/out$RUNN" 2>&1; echo $?; }
bad() { (cd "$SRV" && bash scripts/setup-psvita-zone.sh "$@") >/dev/null 2>&1; echo $?; }
writes() { grep -cE '^(PUT|POST|PATCH|DELETE) ' "$T/calls.log" 2>/dev/null || true; }
phaserules() { python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); print(" ".join(r.get("ref","-") for r in s["phases"].get(sys.argv[2],{}).get("rules",[])))' "$MOCK_STATE" "$1"; }
FOREIGN='{"id":"x1","ref":"strikers-own","description":"another game","expression":"(http.host eq \"other.psvita.dev\")","action":"block","enabled":true}'
FOREIGN_RL='{"id":"x2","ref":"other-rl","description":"another game","expression":"(http.host eq \"other.psvita.dev\")","action":"block","enabled":true,"ratelimit":{"characteristics":["ip.src"],"period":10,"requests_per_period":50,"mitigation_timeout":10}}'

# 1. empty zone: both phases get our rule; the second run writes nothing
state '{}'; rc=$(run)
check "empty phases: created (exit 0, both rules there, confirmed)" '[ "$rc" = 0 ] && [ "$(phaserules http_request_firewall_custom)" = galaxy-psvita-block-unserved ] && [ "$(phaserules http_ratelimit)" = psvita-shared-ratelimit ] && grep -q "ALL CHECKS PASSED" "$T/log$RUNN/summary.txt" && grep -q "^POST /zones/[a-f0-9]*/rulesets\$" "$T/calls.log" && ! grep -q "^PUT " "$T/calls.log"'
check "empty phases: the exact plan was printed BEFORE the first write" '[ "$(grep -n "the plan" "$T/log$RUNN/summary.txt" | head -1 | cut -d: -f1)" -lt "$(grep -n "creating the" "$T/log$RUNN/summary.txt" | head -1 | cut -d: -f1)" ] && grep -q "CREATE the phase" "$T/out$RUNN"'
check "the WAF rule blocks host galaxy.psvita.dev paths other than /v1/* and /times; the shared rate limit is PATH form (/v1/* and /times, no host), per IP, 10 s, 10 s, 100" 'python3 - "$MOCK_STATE" <<P
import json,sys
s=json.load(open(sys.argv[1])); w=s["phases"]["http_request_firewall_custom"]["rules"][0]; r=s["phases"]["http_ratelimit"]["rules"][0]
assert w["action"]=="block" and "http.host eq \"galaxy.psvita.dev\"" in w["expression"] and "not starts_with(http.request.uri.path, \"/v1/\")" in w["expression"] and "ne \"/times\"" in w["expression"]
assert r["action"]=="block" and r["ratelimit"]=={"characteristics":["cf.colo.id","ip.src"],"period":10,"requests_per_period":100,"mitigation_timeout":10} and "http.host" not in r["expression"] and r["expression"]=="(starts_with(http.request.uri.path, \"/v1/\") or http.request.uri.path eq \"/times\")"
assert r["description"].startswith("smg-online") is False and "shared by the games on psvita.dev" in r["description"]
P'
rm -f "$T/calls.log"; rc=$(run)
check "second run changes nothing (idempotent: no write call)" '[ "$rc" = 0 ] && [ "$(writes)" = 0 ] && grep -q "UNCHANGED" "$T/out$RUNN"'

# the API may reorder the characteristics: no PATCH then
python3 - <<'P'
import json,os
s=json.load(open(os.environ["MOCK_STATE"]))
for r in s["phases"]["http_ratelimit"]["rules"]: r["ratelimit"]["characteristics"].reverse()
json.dump(s,open(os.environ["MOCK_STATE"],"w"))
P
rc=$(run)
check "the API returning the characteristics in another order is NOT a difference (no write)" '[ "$rc" = 0 ] && [ "$(writes)" = 0 ] && grep -q UNCHANGED "$T/out$RUNN"'

# 2. someone else's rule in the WAF phase: kept, ours added
state "{\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$FOREIGN]}}}"; rc=$(run)
check "someone else's WAF rule is kept; ours is added to the same entrypoint (POST, never PUT)" '[ "$rc" = 0 ] && [ "$(phaserules http_request_firewall_custom)" = "strikers-own galaxy-psvita-block-unserved" ] && ! grep -q "^PUT /zones/[a-f0-9]*/rulesets/phases/http_request_firewall_custom" "$T/calls.log" && grep -q "^POST " "$T/calls.log"'

# 3. already ours
state '{}'; run >/dev/null; rm -f "$T/calls.log"; rc=$(run)
check "phases that already hold our rules: no write call, exit 0" '[ "$rc" = 0 ] && [ "$(writes)" = 0 ]'

# 4. our rule differs: updated in place; the foreign rule untouched
state "{\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$FOREIGN]}}}"; run >/dev/null; rc=$(run --threshold 200 --ratelimit-reset)
check "a changed threshold (with --ratelimit-reset) updates OUR rate-limit rule in place (PATCH)" '[ "$rc" = 0 ] && grep -q "^PATCH " "$T/calls.log" && [ "$(writes)" = 1 ] && [ "$(phaserules http_ratelimit)" = psvita-shared-ratelimit ] && grep -q "\"requests_per_period\": 200" "$MOCK_STATE"'
check "after the update the foreign WAF rule is still there" '[ "$(phaserules http_request_firewall_custom)" = "strikers-own galaxy-psvita-block-unserved" ]'

# 5. the zone's one Free rate-limiting rule is someone else's
state "{\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[$FOREIGN_RL]}}}"; rc=$(run)
check "the ONE rate-limiting rule is another's: stop, exit 1, nothing written (not even the WAF rule), their rule untouched" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "ONE rate-limiting rule" "$T/out$RUNN" && [ "$(phaserules http_ratelimit)" = other-rl ] && [ -z "$(phaserules http_request_firewall_custom)" ]'

FOREIGN_RL_MAIL='{"id":"x4","ref":"other-rl","description":"made by owner@example.com for 0123456789abcdef0123456789abcdef","expression":"(http.host eq \"other.psvita.dev\")","action":"block","enabled":true,"ratelimit":{"characteristics":["ip.src"],"period":10,"requests_per_period":50,"mitigation_timeout":10}}'
state "{\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[$FOREIGN_RL_MAIL]}}}"; rc=$(run)
check "the stop for a foreign rate-limiting rule prints EACH foreign rule's ref and description, scrubbed (no e-mail, no full id), and nothing is written" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "ref=other-rl description=made by \[EMAIL\] for \[id \*\*\*\*cdef\]" "$T/out$RUNN" && ! grep -q "owner@example.com" "$T/out$RUNN" "$T/log$RUNN/summary.txt"'
state "{\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[$FOREIGN_RL,{\"id\":\"x9\",\"ref\":\"second-rl\",\"description\":\"another\",\"expression\":\"(true)\",\"action\":\"block\",\"enabled\":true}]}}}"; rc=$(run)
check "two foreign rules: both are listed" '[ "$rc" = 1 ] && grep -q "ref=other-rl description=another game" "$T/out$RUNN" && grep -q "ref=second-rl description=another" "$T/out$RUNN"'

# 6. missing permission
state '{"rulesets_perm":"none"}'; rc=$(run)
check "missing permission (read): clear message naming the permission, exit 1, nothing written" '[ "$rc" = 1 ] && grep -q "missing permission: Zone > Zone WAF" "$T/out$RUNN" && [ "$(writes)" = 0 ]'
state '{"rulesets_perm":"read"}'; rc=$(run)
check "missing permission (write): stops at the first write with a clear message; nothing changed" '[ "$rc" = 1 ] && grep -q "missing permission" "$T/out$RUNN" && [ -z "$(phaserules http_request_firewall_custom)" ]'
state '{"rulesets_perm":"plan"}'; rc=$(run)
check "a 403 whose message names the PLAN is reported as a plan refusal, not as a missing permission; the API message is printed" '[ "$rc" = 1 ] && grep -q "not available on your plan" "$T/out$RUNN" && grep -q "refused by the PLAN" "$T/out$RUNN" && ! grep -q "missing permission" "$T/out$RUNN"'
state '{"zones_fail":true}'; rc=$(run)
check "missing permission (zone read): names Zone > Zone > Read" '[ "$rc" = 1 ] && grep -q "Zone > Zone > Read" "$T/out$RUNN" && [ "$(writes)" = 0 ]'
check "the e-mail address in an API error message never reaches the console or a log" '! grep -rq "owner@example.com" "$T"/out* "$T"/log*'

# 7. the path form is the only form: the old flag is accepted as a no-op; extra paths are opt-in
state '{}'; rc=$(run --ratelimit-path-only)
check "--ratelimit-path-only is still accepted (a no-op): same rule as without it" '[ "$rc" = 0 ] && [ "$(phaserules http_ratelimit)" = psvita-shared-ratelimit ] && python3 -c "import json,sys; r=json.load(open(sys.argv[1]))[\"phases\"][\"http_ratelimit\"][\"rules\"][0]; assert r[\"expression\"]==\"(starts_with(http.request.uri.path, \\\"/v1/\\\") or http.request.uri.path eq \\\"/times\\\")\"" "$MOCK_STATE"'
check "--help says the flag is a no-op kept for one release, and that the zone is shared (Strikers must not add a second rate-limiting rule)" 'h=$(bash "$SRV/scripts/setup-psvita-zone.sh" --help); echo "$h" | grep -q "no-op" && echo "$h" | grep -q "one release" && echo "$h" | grep -q "SHARED" && echo "$h" | grep -qi "second rate-limiting rule"'
state '{}'; rc=$(run --ratelimit-extra-paths "/strikers/*,/lobby")
check "--ratelimit-extra-paths adds the paths to the one shared rule (prefix with *, exact otherwise); still ONE rule" '[ "$rc" = 0 ] && [ "$(phaserules http_ratelimit)" = psvita-shared-ratelimit ] && python3 -c "import json,sys; r=json.load(open(sys.argv[1]))[\"phases\"][\"http_ratelimit\"][\"rules\"]; assert len(r)==1; e=r[0][\"expression\"]; assert \"starts_with(http.request.uri.path, \\\"/strikers/\\\")\" in e and \"http.request.uri.path eq \\\"/lobby\\\"\" in e and \"/v1/\" in e and \"/times\" in e" "$MOCK_STATE"'
state '{}'; run >/dev/null; rc=$(run --ratelimit-extra-paths "/strikers/*")
check "a later run with extra paths UPDATES our rule in place (one PATCH)" '[ "$rc" = 0 ] && grep -q "^PATCH " "$T/calls.log" && [ "$(writes)" = 1 ]'
check "extra paths are checked: not starting with /, quotes, spaces, a wildcard in the middle are refused before anything is contacted" 'rm -f "$T/calls.log"; [ "$(bad --account-id $ACCT --token-file $T/token --ratelimit-extra-paths "strikers")" = 1 ] && [ "$(bad --account-id $ACCT --token-file $T/token --ratelimit-extra-paths "/a\"b")" = 1 ] && [ "$(bad --account-id $ACCT --token-file $T/token --ratelimit-extra-paths "/a b")" = 1 ] && [ "$(bad --account-id $ACCT --token-file $T/token --ratelimit-extra-paths "/a*b")" = 1 ] && [ ! -e "$T/calls.log" ]'
state '{}'; rc=$(run --print-plan --ratelimit-extra-paths "/strikers/*")
check "--print-plan shows the extra paths" 'grep -q "/strikers/" "$T/out$RUNN"'
# the WAF rule is always scoped to the Galaxy host, whatever the options: it can never block the other game's host
state '{}'; run --ratelimit-extra-paths "/strikers/*" --threshold 50 >/dev/null
check "the WAF rule always carries http.host eq galaxy.psvita.dev as its FIRST condition, with any options; it never names another host" 'python3 - "$MOCK_STATE" <<P
import json,sys
w=json.load(open(sys.argv[1]))["phases"]["http_request_firewall_custom"]["rules"][0]
e=w["expression"]
assert e.startswith("(http.host eq \"galaxy.psvita.dev\" and "), e
assert e.count("http.host")==1 and "strikers" not in e.lower() and w["ref"]=="galaxy-psvita-block-unserved"
P'
check "the script text builds the WAF expression only from the Galaxy host variable (no option reaches it)" 'grep -q "^WAF_EXPR=\"(http.host eq" "$SRV/scripts/setup-psvita-zone.sh" && ! grep -E "^WAF_EXPR=.*(EXTRA|THRESHOLD)" "$SRV/scripts/setup-psvita-zone.sh"'
# Browser Integrity Check: reported as text, never changed
state '{"bic":"on"}'; run >/dev/null; rc=$(run --verify-only)
check "--verify-only reports Browser Integrity Check (on) as text, writes nothing, does not fail on it" '[ "$rc" = 0 ] && grep -q "Browser Integrity Check: ON" "$T/out$RUNN" && [ "$(writes)" = 0 ]'
state '{"bic":"off"}'; run >/dev/null; rc=$(run --verify-only)
check "Browser Integrity Check off is reported too" '[ "$rc" = 0 ] && grep -q "Browser Integrity Check: off" "$T/out$RUNN"'
state '{"bic":null}'; run >/dev/null; rc=$(run --verify-only)
check "Browser Integrity Check unreadable with this token: said so, not a failure" '[ "$rc" = 0 ] && grep -q "Browser Integrity Check: could not be read" "$T/out$RUNN"'
check "no call ever changes the Browser Integrity Check setting" '! grep -E "^(PUT|POST|PATCH|DELETE) .*browser_check" "$T/calls.log"'
# a foreign rate-limiting rule that is not ours (also one that looks like the old Galaxy-only ref) is never replaced
state "{\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[{\"id\":\"x3\",\"ref\":\"galaxy-psvita-ratelimit\",\"description\":\"old\",\"expression\":\"(http.host eq \\\"galaxy.psvita.dev\\\")\",\"action\":\"block\",\"enabled\":true,\"ratelimit\":{\"characteristics\":[\"ip.src\"],\"period\":10,\"requests_per_period\":50,\"mitigation_timeout\":10}}]}}}"; rc=$(run)
check "a rate-limiting rule with any other ref (even the retired Galaxy-only one) is not ours: stop, nothing written, theirs untouched" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && [ "$(phaserules http_ratelimit)" = galaxy-psvita-ratelimit ]'

# 7b. --bot-fight-off is opt-in
state '{"bot":true}'; rc=$(run)
check "Bot Fight Mode ON, no flag: REFUSED BEFORE any write (exit 1, no write call, no rule created), says so, never changed" '[ "$rc" = 1 ] && grep -q "Bot Fight Mode is ON" "$T/out$RUNN" && grep -q "Nothing was written" "$T/out$RUNN" && [ "$(writes)" = 0 ] && [ -z "$(phaserules http_request_firewall_custom)" ] && [ -z "$(phaserules http_ratelimit)" ] && python3 -c "import json,sys; assert json.load(open(sys.argv[1]))[\"bot\"] is True" "$MOCK_STATE"'
state '{"bot":true,"bot_write":"no"}'; rc=$(run --bot-fight-off)
check "--bot-fight-off that cannot switch it off (no permission): still ON, so still refused before any rule write" '[ "$rc" = 1 ] && [ -z "$(phaserules http_request_firewall_custom)" ] && [ -z "$(phaserules http_ratelimit)" ] && grep -q "Nothing was written" "$T/out$RUNN"'
state "{\"bot\":true,\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[$FOREIGN_RL]}}}"; rc=$(run --bot-fight-off)
check "a refusal that applies to the rules (a foreign rate-limiting rule) comes BEFORE --bot-fight-off writes Bot Fight Mode: exit 1, no PUT at all" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && python3 -c "import json,sys; assert json.load(open(sys.argv[1]))[\"bot\"] is True" "$MOCK_STATE"'
state "{\"bot\":true,\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$FOREIGN]}}}"; run --bot-fight-off >/dev/null; python3 - <<'P'
import json,os
s=json.load(open(os.environ["MOCK_STATE"])); s["bot"]=True; json.dump(s,open(os.environ["MOCK_STATE"],"w"))
P
rc=$(run --remove)
check "--remove keeps its behaviour with Bot Fight Mode ON: our WAF rule is still deleted" '[ "$rc" = 1 ] || [ "$rc" = 0 ]; [ "$(phaserules http_request_firewall_custom)" = strikers-own ]'
state '{"bot":true}'; rc=$(run --bot-fight-off)
check "--bot-fight-off switches it off (one PUT) and the run passes" '[ "$rc" = 0 ] && grep -cE "^PUT .*bot_management" "$T/calls.log" | grep -qx 1 && python3 -c "import json,sys; assert json.load(open(sys.argv[1]))[\"bot\"] is False" "$MOCK_STATE"'
state '{"bot":true}'; rc=$(run --verify-only --bot-fight-off)
check "--verify-only never writes, even with --bot-fight-off" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ]'

# 8. --remove: the WAF rule only; the shared rate-limiting rule needs --remove-shared-ratelimit
state "{\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$FOREIGN]}}}"; run >/dev/null; rc=$(run --remove)
check "--remove deletes only the WAF rule (ours); the shared rate-limiting rule STAYS and the output says why; the other game's WAF rule stays" '[ "$rc" = 0 ] && [ "$(phaserules http_request_firewall_custom)" = strikers-own ] && [ "$(phaserules http_ratelimit)" = psvita-shared-ratelimit ] && grep -q "remove-shared-ratelimit" "$T/out$RUNN" && grep -qi "other game" "$T/out$RUNN"'
rc=$(run --remove --remove-shared-ratelimit)
check "--remove --remove-shared-ratelimit deletes the shared rule too; the other game's rule stays" '[ "$rc" = 0 ] && [ "$(phaserules http_request_firewall_custom)" = strikers-own ] && [ -z "$(phaserules http_ratelimit)" ]'
rm -f "$T/calls.log"; rc=$(run --remove --remove-shared-ratelimit)
check "--remove twice: nothing to delete, no write call" '[ "$rc" = 0 ] && [ "$(writes)" = 0 ]'
state '{}'; run >/dev/null; rc=$(run --remove --verify-only)
check "--remove with --verify-only is refused as before" '[ "$rc" = 1 ]'

# 8b. the shared rule is never PATCHed over another game's paths
state '{}'; run --ratelimit-extra-paths "/strikers/*" >/dev/null; rm -f "$T/calls.log"; rc=$(run)
check "the live shared rule has paths the plan lacks (another game's): STOP, exit 1, nothing written, BOTH expressions printed, the flag named" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "live:.*strikers" "$T/out$RUNN" && grep -q "plan:.*/v1/" "$T/out$RUNN" && ! grep -E "plan:.*strikers" "$T/out$RUNN" && grep -q "ratelimit-drop-paths" "$T/out$RUNN" && grep -q "strikers" "$MOCK_STATE"'
rc=$(run --ratelimit-drop-paths)
check "--ratelimit-drop-paths then PATCHes the rule back to the plan (one write)" '[ "$rc" = 0 ] && [ "$(writes)" = 1 ] && grep -q "^PATCH " "$T/calls.log" && ! grep -q "strikers" "$MOCK_STATE"'
state '{}'; run --ratelimit-extra-paths "/strikers/*" >/dev/null; rm -f "$T/calls.log"; rc=$(run --ratelimit-extra-paths "/strikers/*")
check "keeping the extra paths with --ratelimit-extra-paths: the rule is the same, no write" '[ "$rc" = 0 ] && [ "$(writes)" = 0 ]'
state '{}'; run --ratelimit-extra-paths "/strikers/*" >/dev/null; rc=$(run --verify-only)
check "--verify-only with a rule that carries another game's paths: reported NOT as planned, both expressions shown, nothing written" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "live:.*strikers" "$T/out$RUNN"'
state '{}'; run >/dev/null; rm -f "$T/calls.log"; rc=$(run --threshold 200)
check "a different threshold in the live shared rule STOPS (it affects the other game): exit 1, nothing written, both field lines printed, --ratelimit-reset named" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "live fields: 100 requests" "$T/out$RUNN" && grep -q "plan fields: 200 requests" "$T/out$RUNN" && grep -q "ratelimit-reset" "$T/out$RUNN" && grep -q "\"requests_per_period\": 100" "$MOCK_STATE"'
rm -f "$T/calls.log"; rc=$(run --threshold 200 --ratelimit-reset)
check "--ratelimit-reset then PATCHes it (one write)" '[ "$rc" = 0 ] && [ "$(writes)" = 1 ] && grep -q "^PATCH " "$T/calls.log" && grep -q "\"requests_per_period\": 200" "$MOCK_STATE"'
setdesc() { python3 - "$1" <<'P'
import json,os,sys
s=json.load(open(os.environ["MOCK_STATE"]))
for r in s["phases"]["http_ratelimit"]["rules"]: r["description"]=sys.argv[1]
json.dump(s,open(os.environ["MOCK_STATE"],"w"))
P
}
state '{}'; run >/dev/null; setdesc "edited by hand in the dashboard"; rm -f "$T/calls.log"; rc=$(run)
check "a different description of the live shared rule STOPS too, unless --ratelimit-reset" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "ratelimit-reset" "$T/out$RUNN" && rc=$(run --ratelimit-reset) && [ "$rc" = 0 ] && [ "$(writes)" = 1 ]'
check "a plan that only ADDS paths (--ratelimit-extra-paths on a default live rule) is still a normal PATCH, not refused" 'state "{}"; run >/dev/null; rm -f "$T/calls.log"; rc=$(run --ratelimit-extra-paths "/x/*"); [ "$rc" = 0 ] && [ "$(writes)" = 1 ]'
# any residue in the live expression counts as extra, whatever the syntax of the hand-written term
setexpr() { python3 - "$1" <<'P'
import json,os,sys
s=json.load(open(os.environ["MOCK_STATE"]))
for r in s["phases"]["http_ratelimit"]["rules"]: r["expression"]=r["expression"][:-1]+" or "+sys.argv[1]+")"
json.dump(s,open(os.environ["MOCK_STATE"],"w"))
P
}
for term in 'http.request.uri.path contains "/other"' 'http.request.uri.path wildcard "/other/*"' 'http.request.uri.path in {"/a" "/b"}' 'http.request.uri.path == "/other"' 'lower(http.request.uri.path) eq "/other"' 'http.request.full_uri eq "https://x.psvita.dev/y"'; do
  state '{}'; run >/dev/null; setexpr "$term"; rm -f "$T/calls.log"; rc=$(run)
  check "a hand-written live term ($term) is not PATCHed away silently: STOP, nothing written, both expressions printed" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "live: " "$T/out$RUNN" && grep -q "plan: " "$T/out$RUNN" && grep -q "ratelimit-drop-paths" "$T/out$RUNN"'
done
state '{}'; run >/dev/null; setexpr 'http.request.uri.path eq "/0123456789abcdef0123456789abcdef"'; rc=$(run)
check "the live: and plan: lines go through the scrub (a 32-hex run is shown as its last 4)" '[ "$rc" = 1 ] && ! grep -q "0123456789abcdef0123456789abcdef" "$T/out$RUNN" "$T/log$RUNN/summary.txt" && grep -q "live: .*\*\*\*\*cdef" "$T/out$RUNN"'
state '{}'; run >/dev/null; setexpr 'http.request.uri.path contains "/o"'; rc=$(run --ratelimit-drop-paths)
check "--ratelimit-drop-paths PATCHes a hand-written term away" '[ "$rc" = 0 ] && ! grep -q "contains" "$MOCK_STATE"'


# 9. --verify-only
state '{}'; rc=$(run --verify-only)
check "--verify-only on an empty zone: exit 1 and NO write call" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "NOT as planned" "$T/out$RUNN"'
state '{}'; run >/dev/null; rm -f "$T/calls.log"; rc=$(run --verify-only)
check "--verify-only after a setup: exit 0, no write call, Bot Fight Mode off confirmed" '[ "$rc" = 0 ] && [ "$(writes)" = 0 ] && grep -q "Bot Fight Mode: off" "$T/out$RUNN"'
state '{}'; run >/dev/null; python3 - <<'P'
import json,os
s=json.load(open(os.environ["MOCK_STATE"])); s["bot"]=True; json.dump(s,open(os.environ["MOCK_STATE"],"w"))
P
rm -f "$T/calls.log"; rc=$(run --verify-only)
check "Bot Fight Mode ON: reported as a failure, never changed (no call to change it exists)" '[ "$rc" = 1 ] && grep -q "Bot Fight Mode is ON" "$T/out$RUNN" && [ "$(writes)" = 0 ] && ! grep -qE "^(PUT|POST|PATCH|DELETE) .*bot" "$T/calls.log"'
state '{"bot":null}'; run >/dev/null; rc=$(run --verify-only)
check "Bot Fight Mode unreadable with this token: said so, not a failure" '[ "$rc" = 0 ] && grep -q "could not be read" "$T/out$RUNN"'

# 10. pins
state '{"zone":null}'; rc=$(run)
check "no zone psvita.dev in the account: stop, nothing written" '[ "$rc" = 1 ] && grep -q "found 0" "$T/out$RUNN" && [ "$(writes)" = 0 ]'
state "{\"zone\":{\"id\":\"$ZONE\",\"status\":\"active\",\"account\":\"$OTHERACCT\"}}"; rc=$(run)
check "a zone of another account: refused, nothing written" '[ "$rc" = 1 ] && grep -q "does not belong to account" "$T/out$RUNN" && [ "$(writes)" = 0 ]'
state "{\"zone\":{\"id\":\"$ZONE\",\"status\":\"pending\",\"account\":\"$ACCT\"}}"; rc=$(run)
check "an inactive zone: refused, nothing written" '[ "$rc" = 1 ] && grep -q "not active" "$T/out$RUNN" && [ "$(writes)" = 0 ]'
rm -f "$T/calls.log"
check "argument checks: no account id, bad account id, no token file, bad threshold, --verify-only with --remove" '[ "$(bad --token-file $T/token)" = 1 ] && [ "$(bad --account-id abc --token-file $T/token)" = 1 ] && [ "$(bad --account-id $ACCT)" = 1 ] && [ "$(bad --account-id $ACCT --token-file $T/token --threshold 5)" = 1 ] && [ "$(bad --account-id $ACCT --token-file $T/token --verify-only --remove)" = 1 ] && [ ! -e "$T/calls.log" ]'

# 10b. an API message or curl diagnostic that echoes the token (or an e-mail) never reaches the console or a kept file
state '{"echo_token":true,"echo_stderr":true,"rulesets_perm":"read"}'; rc=$(run)
check "an error message that echoes the exact token: shown as [TOKEN], the token is in no output, log, summary or kept curl diagnostic" '[ "$rc" = 1 ] && grep -q "\[TOKEN\]" "$T/out$RUNN" && ! grep -rq "$SECRET" "$T/out$RUNN" "$T/log$RUNN" && ls "$T/log$RUNN" | grep -q curl-error'
check "curl's own stderr diagnostic is scrubbed in the kept file (token and e-mail)" '! grep -rq "owner@example.com" "$T/log$RUNN" && grep -rq "\[TOKEN\]" "$T/log$RUNN"/*curl-error'
state '{"echo_token":true,"zones_fail":true}'; rc=$(run)
check "the same for the zone lookup refusal (die path)" '[ "$rc" = 1 ] && ! grep -rq "$SECRET" "$T/out$RUNN" "$T/log$RUNN"'

# 11. secrets
check "the token appears in no log, argument, output, temp file or environment" '! grep -rq "$SECRET" "$T"/log* "$T"/out* "$T/tmp" "$T/seen/curl-args" "$T/seen/curl-env" "$T/calls.log" 2>/dev/null'
check "the token reached curl on stdin (-K -) as a config line" 'grep -q "^Authorization: Bearer $SECRET\$" "$T/seen/hdr-stdin" && grep -q "curl.* -K -\$" "$T/seen/curl-args"'
check "the full account id and zone id are in no log or output (only the last 4)" '! grep -E -rq "$ACCT|$ZONE" "$T"/log* "$T"/out* && grep -rq "\*\*\*\*cdef" "$T"/out*'
check "the account id and zone id are in no recorded curl argument either (the URL goes in the stdin config)" '! grep -E -q "$ACCT|$ZONE|api.cloudflare.com" "$T/seen/curl-args"'
check "no account name or e-mail in any log or output" '! grep -rqE "Acme Hat|owner@example.com" "$T"/log* "$T"/out*'
check "no raw reply is kept: no log file holds a JSON body" '! grep -rlq "\"success\"" "$T"/log*'

# 12. plan and self-check
rm -f "$T/calls.log"
plan=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/no-such-token-file" --print-plan) 2>&1; echo "rc=$?")
check "--print-plan: exit 0, nothing contacted, the token file not opened (a missing file still prints the plan), the rules shown, the id as last 4" 'echo "$plan" | grep -q "^rc=0" && [ ! -e "$T/calls.log" ] && echo "$plan" | grep -q "galaxy-psvita-block-unserved" && echo "$plan" | grep -q "\*\*\*\*cdef" && ! echo "$plan" | grep -q "$ACCT"'
rm -f "$T/calls.log"; out=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/no-such-token-file") 2>&1; echo "rc=$?")
check "a missing token file: nothing contacted, exact creation steps (Zone > Zone > Read, Zone > Zone WAF > Edit, psvita.dev only), exit 1" 'echo "$out" | grep -q "rc=1" && [ ! -e "$T/calls.log" ] && echo "$out" | grep -q "Zone > Zone > Read" && echo "$out" | grep -q "Zone > Zone WAF > Edit" && echo "$out" | grep -q "psvita.dev ONLY"'
check "--help works without GNU sed" 'bash "$SRV/scripts/setup-psvita-zone.sh" --help | grep -q "^#   --verify-only"'
check "the script passes its own self-check" 'bash "$SRV/scripts/setup-psvita-zone.sh" --self-check >/dev/null'
cp "$SRV/scripts/setup-psvita-zone.sh" "$T/d1.sh"; printf '\ncurl https://example.org\n' >> "$T/d1.sh"
check "self-check: a curl call outside api() is caught" '! bash "$T/d1.sh" --self-check >/dev/null 2>&1'
cp "$SRV/scripts/setup-psvita-zone.sh" "$T/d2.sh"; printf '\nprintf %%s "$TOK" > /tmp/x\n' >> "$T/d2.sh"
check "self-check: the token used outside api() is caught" '! bash "$T/d2.sh" --self-check >/dev/null 2>&1'
check "the script never names the other game's Worker (msc-online)" '! grep -qi "msc-online" "$SRV/scripts/setup-psvita-zone.sh"'
# ---- 15 Nov (second review, svcfix): items 1, 3, 4, 5, 6, 8, 10
setst() { python3 - "$1" "$2" <<'P'
import json,os,sys
s=json.load(open(os.environ["MOCK_STATE"])); s[sys.argv[1]]=json.loads(sys.argv[2]); json.dump(s,open(os.environ["MOCK_STATE"],"w"))
P
}
lastrun() { cat "$T/runn"; }
# item 10: the token substitution is a LITERAL one (awk index/substr, token via the environment): glob characters, backslash, & in a token are not special
eval "$(sed -n '/^scrub() {/,/^}/p' "$SRV/scripts/setup-psvita-zone.sh")"
globtest() { local t=$1 inp=$2 want=$3 got; got=$( TOK=$t; printf '%s\n' "$inp" | scrub ); [ "$got" = "$want" ]; }
globcases() {
  globtest '*' 'a*b abc' 'a[TOKEN]b abc' || return 1
  globtest '?' 'x?y xyz' 'x[TOKEN]y xyz' || return 1
  globtest '[a]' 'p[a]q pa q' 'p[TOKEN]q pa q' || return 1
  globtest '\' 'c:\d' 'c:[TOKEN]d' || return 1
  globtest 'a&b' '1 a&b 2 a&b a b' '1 [TOKEN] 2 [TOKEN] a b' || return 1
  globtest 'a*b?c[d]\e&f.g+h/i=j~k-l' '[a*b?c[d]\e&f.g+h/i=j~k-l] aXbYc' '[[TOKEN]] aXbYc' || return 1
  globtest '' 'nothing to hide' 'nothing to hide' || return 1
  globtest 'tok.+x' 'tokAAx tok.+x' 'tokAAx [TOKEN]' || return 1
  globtest "$SECRET" "x $SECRET y $SECRET" 'x [TOKEN] y [TOKEN]' || return 1
}
check "scrub-literal: a token with *, ?, [ ], backslash, & or regex characters is replaced literally, everywhere in the line, and nothing else is (also the random test token)" 'globcases'
globfile() { printf '%s\n' "$1" > "$T/globtok"; chmod 600 "$T/globtok"; (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/globtok" --log-dir "$T/logglob") > "$T/outglob" 2>&1; echo $?; }
rm -f "$T/calls.log"
check "token file: a token holding glob characters (* ? [ &) is refused by the script's own charset check, before any request" '[ "$(globfile "tok*aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && grep -q "characters other than" "$T/outglob" && [ "$(globfile "tok?aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && [ "$(globfile "tok[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && [ "$(globfile "tok&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && [ ! -e "$T/calls.log" ]'

# item 1: a 200 (or other) reply with a truncated body holding an e-mail and a 32-hex run: nothing of it in the console or any kept file
LEAKRE='leak\.person|mail me at|9f8e7d6c5b4a39281706f5e4d3c2b1a0|\*\*\*\*b1a0|\[EMAIL\]'
trres=""
for key in zones waf rl waf:500 rl:500 zones:403; do
  state "{\"trunc\":[\"$key\"]}"; rc=$(run); n=$(lastrun); trres="$trres$rc"
  grep -rqE "$LEAKRE" "$T/out$n" "$T/log$n" && trres="${trres}L"
  [ "$(writes)" = 0 ] || trres="${trres}W"
done
check "truncated-reply: a truncated 200 body (zone lookup, WAF phase, rate-limit phase) or a truncated 500 / 403 body: the run stops (exit 1), nothing written, and neither the e-mail nor the 32-hex run is in the output or any kept file" '[ "$trres" = 111111 ]'
check "truncated-reply: the phase read says a fixed message instead of the body" 'state "{\"trunc\":[\"waf\"]}"; rc=$(run); n=$(lastrun); [ "$rc" = 1 ] && grep -q "cannot read the reply of the http_request_firewall_custom phase: the answer (HTTP 200) is not the JSON the API documents" "$T/out$n"'
trres=""
for key in bot bic; do
  state "{\"trunc\":[\"$key\"]}"; rc=$(run); n=$(lastrun); trres="$trres$rc"
  grep -rqE "$LEAKRE" "$T/out$n" "$T/log$n" && trres="${trres}L"
done
check "truncated-reply: a truncated Bot Fight Mode or Browser Integrity Check reply reads as 'could not be read': the run goes on (exit 0), no leak" '[ "$trres" = 00 ]'
check "truncated-reply: also nothing of it in ANY kept file or output of ANY of these runs" '! grep -rqE "leak\.person|mail me at|9f8e7d6c5b4a39281706f5e4d3c2b1a0|\*\*\*\*b1a0" "$T"/out* "$T"/log* 2>/dev/null'

# item 3: Bot Fight Mode that cannot be read: said on the write path too, and repeated in the final summary
state '{"bot":null}'; rc=$(run); n=$(lastrun)
BOTMSG="Bot Fight Mode could not be read with this token; check it in the dashboard (Security > Bots): it must be OFF"
check "bot-unreadable: a WRITE run says it could not read Bot Fight Mode (the dashboard path, must be OFF), once before the writes and once in the final summary, and still passes" '[ "$rc" = 0 ] && [ "$(grep -cF "$BOTMSG" "$T/out$n")" = 2 ] && [ "$(grep -nF "$BOTMSG" "$T/out$n" | head -1 | cut -d: -f1)" -lt "$(grep -n "adding the\|creating the" "$T/out$n" | head -1 | cut -d: -f1)" ] && [ "$(grep -nF "$BOTMSG" "$T/out$n" | tail -1 | cut -d: -f1)" -gt "$(grep -n "rate-limiting rule: confirmed" "$T/out$n" | head -1 | cut -d: -f1)" ] && [ "$(tail -1 "$T/out$n" | sed "s/^\[[0-9:]*\] //")" = "ALL CHECKS PASSED" ]'
state '{"bot":null}'; run >/dev/null; rc=$(run); n=$(lastrun)
check "bot-unreadable: an unchanged second run repeats it too" '[ "$rc" = 0 ] && [ "$(grep -cF "$BOTMSG" "$T/out$n")" = 2 ]'
state '{"bot":false}'; rc=$(run); n=$(lastrun)
check "bot-unreadable: not said when Bot Fight Mode could be read (off)" '[ "$rc" = 0 ] && ! grep -qF "could not be read with this token; check it in the dashboard" "$T/out$n"'
state '{"bot":true}'; rc=$(run); n=$(lastrun)
check "bot-unreadable: Bot Fight Mode that reads ON is still refused before any write" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && grep -q "Nothing was written" "$T/out$n" && ! grep -q "written: " "$T/out$n"'

# item 4: an exact "written: ...; not written: ..." line on every exit path after the first write
W1="WAF rule galaxy-psvita-block-unserved"; R1="rate-limit rule psvita-shared-ratelimit"
state '{"refuse_rl_write":true}'; rc=$(run); n=$(lastrun)
check "written-line: the WAF rule is written, the API refuses the rate-limit rule: exit 1, the WAF rule IS there, and the line says exactly that" '[ "$rc" = 1 ] && [ "$(phaserules http_request_firewall_custom)" = galaxy-psvita-block-unserved ] && [ -z "$(phaserules http_ratelimit)" ] && grep -qF "written: $W1 (create); not written: $R1 (create)" "$T/out$n" && ! grep -q "nothing was widened" "$T/out$n" "$T/log$n/summary.txt" && grep -q "the rate-limiting rule failed\|adding the ratelimit rule: HTTP 400\|creating the ratelimit phase entrypoint with our rule: HTTP 400" "$T/out$n"'
state "{\"refuse_rl_write\":true,\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$FOREIGN]}}}"; rc=$(run); n=$(lastrun)
check "written-line: with someone else's WAF rule there (our WAF rule is ADDED): (add) and the foreign rule is kept" '[ "$rc" = 1 ] && [ "$(phaserules http_request_firewall_custom)" = "strikers-own galaxy-psvita-block-unserved" ] && grep -qF "written: $W1 (add); not written: $R1 (create)" "$T/out$n"'
state '{}'; run >/dev/null; setst refuse_rl_write true; rc=$(run --threshold 200 --ratelimit-reset); n=$(lastrun)
check "written-line: a refused UPDATE of the rate-limit rule: nothing written, line says (update) is not written, the live rule is unchanged" '[ "$rc" = 1 ] && grep -qF "written: none; not written: $R1 (update)" "$T/out$n" && grep -q "\"requests_per_period\": 100" "$MOCK_STATE"'
state '{"rulesets_perm":"read"}'; rc=$(run); n=$(lastrun)
check "written-line: the FIRST write refused (missing permission): the line says nothing was written and lists both" '[ "$rc" = 1 ] && grep -qF "written: none; not written: $W1 (create), $R1 (create)" "$T/out$n"'
state '{}'; rc=$(run); n=$(lastrun)
check "written-line: a passing run says both written, none left, just before ALL CHECKS PASSED" '[ "$rc" = 0 ] && grep -qF "written: $W1 (create), $R1 (create); not written: none" "$T/out$n" && [ "$(tail -1 "$T/out$n" | sed "s/^\[[0-9:]*\] //")" = "ALL CHECKS PASSED" ] && [ "$(tail -2 "$T/out$n" | head -1 | sed "s/^\[[0-9:]*\] //")" = "written: $W1 (create), $R1 (create); not written: none" ]'
rc=$(run); n=$(lastrun)
check "written-line: an unchanged run says written: none; not written: none" '[ "$rc" = 0 ] && grep -qF "written: none; not written: none" "$T/out$n"'
rc=$(run --remove --remove-shared-ratelimit); n=$(lastrun)
check "written-line: --remove --remove-shared-ratelimit lists both deletes as written" '[ "$rc" = 0 ] && grep -qF "written: $W1 (delete), $R1 (delete); not written: none" "$T/out$n"'
state '{"bot":true,"bot_write":"no"}'; rc=$(run --bot-fight-off); n=$(lastrun)
check "written-line: a refused --bot-fight-off write is tracked too (not written), no rule was written" '[ "$rc" = 1 ] && grep -qF "written: none; not written: Bot Fight Mode (switch off)" "$T/out$n" && [ "$(writes)" = 1 ]'
state '{"bot":true}'; rc=$(run --bot-fight-off); n=$(lastrun)
check "written-line: a successful --bot-fight-off is listed as written" '[ "$rc" = 0 ] && grep -qF "written: Bot Fight Mode (switch off), $W1 (create), $R1 (create); not written: none" "$T/out$n"'

# item 5: --remove refuses when more than one rule carries our ref
DUP1='{"id":"d1","ref":"galaxy-psvita-block-unserved","description":"one","expression":"(true)","action":"block","enabled":true}'
DUP2='{"id":"d2","ref":"galaxy-psvita-block-unserved","description":"two","expression":"(true)","action":"block","enabled":true}'
state "{\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$DUP1,$DUP2]}}}"; rc=$(run --remove); n=$(lastrun)
check "remove-ambiguous: --remove with two rules carrying our WAF ref: refused (exit 1), NO write, both rules still there" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && [ "$(phaserules http_request_firewall_custom)" = "galaxy-psvita-block-unserved galaxy-psvita-block-unserved" ] && grep -q "MORE THAN ONE rule with our ref" "$T/out$n" && grep -q "never guesses" "$T/out$n" && grep -q "Nothing was written" "$T/out$n"'
DUPR1='{"id":"e1","ref":"psvita-shared-ratelimit","description":"one","expression":"(true)","action":"block","enabled":true}'
DUPR2='{"id":"e2","ref":"psvita-shared-ratelimit","description":"two","expression":"(true)","action":"block","enabled":true}'
state "{\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[$DUPR1,$DUPR2]}}}"; rc=$(run --remove --remove-shared-ratelimit); n=$(lastrun)
check "remove-ambiguous: the same for two shared rate-limit rules with --remove-shared-ratelimit: refused, no write, both there" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ] && [ "$(phaserules http_ratelimit)" = "psvita-shared-ratelimit psvita-shared-ratelimit" ]'
state "{\"phases\":{\"http_request_firewall_custom\":{\"id\":\"rs_http_request_firewall_custom\",\"rules\":[$DUP1,$DUP2]}}}"; rc=$(run); n=$(lastrun)
check "remove-ambiguous: the normal write run still refuses ambiguity as before" '[ "$rc" = 1 ] && [ "$(writes)" = 0 ]'
state '{}'; run >/dev/null; rc=$(run --remove); n=$(lastrun)
check "remove-ambiguous: --remove with exactly one rule of ours still deletes it" '[ "$rc" = 0 ] && [ -z "$(phaserules http_request_firewall_custom)" ]'

# item 6: raw curl diagnostics only in a private temp dir; kept files only from scrubbed text
rm -f "$T/seen/rawdir" "$T/leak.log"; state '{"echo_stderr":true,"echo_token":true}'; rc=$(run); n=$(lastrun)
rawdir_ok() { awk -v t="$T/tmp/" 'index($1, t) != 1 || $2 != "drwx------" { bad = 1 } END { exit bad }' "$T/seen/rawdir"; }
check "raw-temp: curl's stderr (a diagnostic with the token and an e-mail) went into a private (mode 700) temp dir under TMPDIR, never under a log folder; the dir is gone after the run" '[ "$rc" = 0 ] && [ -s "$T/seen/rawdir" ] && rawdir_ok && ! grep -q "/log" "$T/seen/rawdir" && [ -z "$(ls -A "$T/tmp")" ]'
check "leak-window: at no moment did a kept file hold the raw diagnostic while the scrubber ran (the old raw-then-scrub-in-place order would)" '[ ! -e "$T/leak.log" ] && grep -q "\[EMAIL\]" "$T/log$n"/*curl-error && ! grep -rq "owner@example.com" "$T/log$n"'
mkdir -p "$T/bin-kill"; state '{"echo_stderr":true,"zones_fail":true}'
cat > "$T/bin-kill/curl" <<S
#!/bin/bash
"$T/bin/curl" "\$@"; rc=\$?
IFS= read -r zone_pid < "$T/zone-pid" || exit 1
[[ "\$zone_pid" =~ ^[1-9][0-9]*\$ ]] || exit 1
kill -TERM "\$zone_pid" || exit 1
/bin/sleep 2
exit \$rc
S
chmod +x "$T/bin-kill/curl"
rc=$(PATH="$T/bin-kill:$PATH" RUN_PID_FILE="$T/zone-pid" run); n=$(lastrun)
check "kill: SIGTERM during a request ends the run (exit 143), removes the raw temp dir, leaves no raw text under the log folder" '[ "$rc" = 143 ] && [ -z "$(ls -A "$T/tmp")" ] && ! grep -rq "owner@example.com" "$T/log$n"'

# item 8: a token file that group or others can read or write is refused; created files are 600, folders 700
mres=""
for m in 640 604 660 666 620 602 644 777; do cp "$T/token" "$T/tokm"; chmod $m "$T/tokm"; rm -f "$T/calls.log"
  rcm=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/tokm" --log-dir "$T/logm") > "$T/outm" 2>&1; echo $?)
  mres="$mres$rcm"; [ ! -e "$T/calls.log" ] || mres="${mres}C"; grep -q "only its owner can read and write" "$T/outm" || mres="${mres}M"; done
check "token-mode: a token file readable or writable by group or others (640 604 660 666 620 602 644 777) is refused, and no request is made" '[ "$mres" = 11111111 ]'
mok=""; state '{}'
for m in 600 400 700; do
  tokm="$T/tokm$m"
  cp "$T/token" "$tokm" && chmod "$m" "$tokm" || exit 1
  rcm=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$tokm" --log-dir "$T/logm$m" --verify-only) > "$T/outm" 2>&1; echo $?)
  mok="$mok$rcm"
done
check "token-mode: owner-only modes (600, 400, 700) are read (--verify-only on an empty zone exits 1 for the rules, not for the token)" '[ "$mok" = 111 ] && ! grep -q "only its owner" "$T/outm"'
ln -sf "$T/token" "$T/toklink"; chmod 644 "$T/token"; rcm=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/toklink" --log-dir "$T/logm2" --verify-only) > "$T/outm" 2>&1; echo $?); chmod 600 "$T/token"
check "token-mode: a symlink to a group-readable file is refused too (the target's mode counts)" '[ "$rcm" = 1 ] && grep -q "only its owner" "$T/outm"'
state '{}'; rc=$(run); n=$(lastrun)
check "file-modes: every file the script wrote is mode 600 and every folder mode 700 (umask 077)" '[ "$rc" = 0 ] && [ -z "$(find "$T/log$n" -type f ! -perm 600)" ] && [ -z "$(find "$T/log$n" -type d ! -perm 700)" ] && [ -s "$T/log$n/summary.txt" ]'
check "no leftovers: the temp area is empty after every run of this test" '[ -z "$(ls -A "$T/tmp")" ]'
# round f: no answer to a write, the plan's reads, the foreign-rule list once
state '{"noanswer_rl_write":true}'; rc=$(run); n=$(lastrun)
check "no-answer: a write that gets NO answer is reported as unknown (a rerun reads the truth), not as 'not written'" '[ "$rc" = 1 ] && grep -qF "written: $W1 (create); not written: none; no answer: unknown; a rerun reads the truth: $R1 (create)" "$T/out$n" && grep -q "no answer (HTTP 000): whether the write happened is unknown; a rerun reads the truth" "$T/out$n"'
out=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/token" --print-plan) 2>&1)
check "plan: lists the two reads (bot_management and settings/browser_check) and that Bot Fight Mode ON refuses before any write" 'echo "$out" | grep -q "zones/<zone>/bot_management" && echo "$out" | grep -q "zones/<zone>/settings/browser_check" && echo "$out" | grep -q "REFUSES the run BEFORE any write"'
check "header: documents both optional read permissions and the refusal before any write" 'h=$(bash "$SRV/scripts/setup-psvita-zone.sh" --help); echo "$h" | grep -q "Zone > Zone Settings > Read" && echo "$h" | grep -q "refused BEFORE any write"'
state "{\"phases\":{\"http_ratelimit\":{\"id\":\"rs_http_ratelimit\",\"rules\":[$FOREIGN_RL]}}}"; rc=$(run); n=$(lastrun)
check "foreign-rule stop: the foreign rule is listed ONCE, and the message has single spacing after 'rule:'" '[ "$rc" = 1 ] && [ "$(grep -c "not ours, in the http_ratelimit phase" "$T/out$n")" = 1 ] && grep -q "rate-limiting rule: the phase is full" "$T/out$n" && ! grep -q "rule:  the" "$T/out$n"'
# 15 Nov (coordinator decisions): classification of 403, hints, token message, case-insensitive account name
state '{"rulesets_perm":"notallowed"}'; rc=$(run); n=$(lastrun)
check "403-class: a 403 whose message says 'not allowed' is a MISSING PERMISSION (only plan, entitle, upgrade, subscription mean a plan refusal)" '[ "$rc" = 1 ] && grep -q "missing permission: Zone > Zone WAF" "$T/out$n" && ! grep -q "refused by the PLAN" "$T/out$n" && [ "$(writes)" = 0 ]'
state '{"bot":true}'; rc=$(run); n=$(lastrun)
check "hints: the Bot Fight Mode ON refusal names --bot-fight-off only as \"Owner's decision only:\"" '[ "$rc" = 1 ] && grep -q "Nothing was written. Owner.s decision only: turning it off by hand" "$T/out$n"'
state '{"bot":true}'; rc=$(run --verify-only); n=$(lastrun)
check "hints: the --verify-only Bot Fight Mode ON line says \"Owner's decision only:\"" 'grep -q "Owner.s decision only: turning it off by hand" "$T/out$n"'
state '{}'; run --ratelimit-extra-paths "/strikers/*" >/dev/null; rc=$(run); n=$(lastrun)
check "hints: the shared-rule stop names --ratelimit-drop-paths / -extra-paths only as \"Owner's decision only:\"" '[ "$rc" = 1 ] && grep -q "Owner.s decision only: to keep them use --ratelimit-extra-paths" "$T/out$n" && grep -q "ratelimit-drop-paths" "$T/out$n"'
state '{}'; run >/dev/null; rc=$(run --threshold 200); n=$(lastrun)
check "hints: the --ratelimit-reset hint says \"Owner's decision only:\"" '[ "$rc" = 1 ] && grep -q "Owner.s decision only: --ratelimit-reset" "$T/out$n"'
run --remove >/dev/null; n=$(lastrun)
check "hints: the shared-rule-kept note says --remove-shared-ratelimit is the owner's decision only" 'grep -q "owner.s decision only: --remove-shared-ratelimit" "$T/out$n"'
rm -f "$T/calls.log"; out=$( (cd "$SRV" && bash scripts/setup-psvita-zone.sh --account-id $ACCT --token-file "$T/no-such-token-file") 2>&1)
check "token message: lists both optional permissions and the two required ones, as the runbook does" 'echo "$out" | grep -q "required: Zone > Zone > Read; Zone > Zone WAF > Edit. Optional: Zone > Zone Settings > Read (only for reading Browser Integrity Check) and Zone > Bot Management > Read (only for reading Bot Fight Mode)"'
check "scrub: the account-name line is removed whatever its letter case" 'acn() { local t="$1" got; got=$(printf "%s\n" "$t" | scrub); [ "$got" = "[account name line removed]" ]; }; acn "Account Name: Acme" && acn "ACCOUNT NAME: Acme" && acn "account name: Acme" && acn "x aCcOuNt nAmE y"'
check "portability: no GNU- or Linux-only form (the forms listed in portability-check.sh) in scripts/*.sh or test/deploy/*" 'portability_scan "$SRV" > "$T/port.out" 2>&1 || { cat "$T/port.out"; false; }'
echo "total: $NOK ok, $NFAIL FAIL"
exit $fail
