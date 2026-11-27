#!/bin/bash
# Sets up the security rules of the Cloudflare zone psvita.dev for the Galaxy service https://galaxy.psvita.dev (docs/ONLINE-DOMAIN-SETUP.md).
# THE ZONE IS SHARED: another game (Strikers, on its own host and its own Worker) lives on psvita.dev too. So: the ONE rate-limiting rule the Free
# plan allows is ONE shared rule that matches the games' PATHS zone-wide. Strikers must not add a second rate-limiting rule (it does not fit; to
# cover another game's paths use --ratelimit-extra-paths); the WAF block rule is Galaxy's alone and is always scoped to the host
# galaxy.psvita.dev, so it can never touch another host. Bot Fight Mode and Browser Integrity Check are zone-wide settings (see below).
# Same style and the same safety rules as deploy-smg-online.sh. Run AFTER the deploy (the custom domain exists). Nothing here touches the Worker.
#
#   scripts/setup-psvita-zone.sh --account-id <32 hex> --token-file ~/.cf-psvita-token [options]
#
#   --account-id ID        REQUIRED. The zone must belong to this account (32 hex characters; shown only as its last 4).
#   --token-file FILE      REQUIRED. The Cloudflare API token (one line). Read from the file, handed to curl on stdin (`curl -K -`), never printed,
#                          logged, written to a file or put on a command line. Needs: Zone > Zone > Read and Zone > Zone WAF > Edit, restricted to the
#                          zone psvita.dev. Optional, only to READ two settings (unreadable = said so, never a failure): Zone > Bot Management > Read (Bot Fight Mode) and
#                          Zone > Zone Settings > Read (Browser Integrity Check). Nothing account-wide.
#   --threshold N          rate limit: requests per 10 s per IP (default 100, allowed 10..1000: 100 is ten times a busy honest lobby, README.md)
#   --ratelimit-extra-paths LIST
#                          add paths to the shared rate-limiting rule (comma separated; /prefix/* = every path under it, anything else an exact path;
#                          default none). ONLY when the owner asks, for another game whose paths are not /v1/* and /times. The rule stays ONE rule.
#   --ratelimit-path-only  a no-op, kept for one release so an old command line still works: the path form is now the only form.
#   --print-plan           print the rules this script would manage and the calls it would make (nothing is contacted, the token file is not opened), exit 0
#   --self-check           check that curl is only called inside api() and the token is only used where it may be, then exit
#   --verify-only          read only: check that our rules exist exactly as planned (and, if the token may read it, that Bot Fight Mode is OFF; it also
#                          REPORTS whether Browser Integrity Check is on, as text only: it is zone-wide and this script never changes it); exit 1 if not
#   --bot-fight-off        OPT-IN, off by default: if the read shows Bot Fight Mode ON, switch it OFF (the only write this script ever makes to it; needs a
#                          token permission to edit it). Without this flag it only reports it.
#   --remove               rollback: delete OUR WAF rule (found by its ref) and nothing else; other rules are never touched. The shared rate-limiting rule is
#                          deleted only with --remove-shared-ratelimit (it serves the other game on the zone too); without it the script says so and keeps it
#   --remove-shared-ratelimit  with --remove: also delete the shared rate-limiting rule (psvita-shared-ratelimit)
#   --ratelimit-drop-paths when the live shared rule holds anything the plan lacks in its expression (another game's paths, or any hand-written term),
#                          the script STOPS and prints both expressions rather than PATCH it away; this flag says "drop it" (or keep paths with --ratelimit-extra-paths)
#   --ratelimit-reset      when the live shared rule's threshold, period, mitigation, characteristics or description differ from the plan, the script STOPS
#                          (they affect the other game too) and prints both; this flag says "reset them to the plan"
#   --log-dir DIR          where the (summarised, never raw) replies are kept (default zone-logs/<UTC time>)
#
# Refused up front: a token file that is not a regular file only its owner can read and write (mode 600). Raw remote text (curl diagnostics) lives only in a
# private temp dir removed on exit; the kept files hold scrubbed text only. After the first write EVERY exit prints "written: ...; not written: ...".
# A phase with more than one rule carrying our ref is refused, with --remove too.
#
# What it manages (and ONLY this; every rule is found by its fixed `ref`, other rules of the same phases are read and kept as they are):
#   phase http_request_firewall_custom   ref galaxy-psvita-block-unserved   Block every path on galaxy.psvita.dev (that host ONLY) the Worker does not serve
#   phase http_ratelimit                 ref psvita-shared-ratelimit         per IP, 10 s period, 10 s Block, on the paths /v1/* and /times of the whole zone,
#                                                                            shared by the games on psvita.dev (the Free plan's ONE rate-limiting rule)
# Bot Fight Mode is READ (GET /zones/<zone>/bot_management): if it reads ON the run is refused BEFORE any write; Browser Integrity Check is READ (settings/browser_check) and reported only.
# It changes Bot Fight Mode ONLY with --bot-fight-off and only when it is found ON (it must stay OFF while the game API lives on this zone: it cannot be skipped by rules and may challenge API clients).
# Idempotent: a second run reads the rules, finds them equal and writes nothing. Before any write it prints the exact plan.
# Rules of the Rulesets API used: GET the phase entrypoint; POST-create it when it does not exist yet; otherwise POST (add; also to CREATE a missing entrypoint, so two runs cannot overwrite each other) / PATCH (update) / DELETE
# of our single rule, so a rule that someone else added is never dropped. If the zone's one Free rate-limiting rule is another one, it stops.
set -euo pipefail
# pipefail rule: never end a pipeline with a command that can stop reading early (head, grep -q, grep -m, sed q, awk exit): the writer gets SIGPIPE (141) and pipefail turns that into a failure
# even when the answer was right. Use sed -n '1,3p' (reads everything), or grep ... >/dev/null. A test (deploy-script-test.sh) fails if one comes back.
umask 077
unset TOK   # a secret never arrives through the environment

ZONE_NAME=psvita.dev
HOST=galaxy.psvita.dev
API=https://api.cloudflare.com/client/v4
WAF_PHASE=http_request_firewall_custom
RL_PHASE=http_ratelimit
WAF_REF=galaxy-psvita-block-unserved
RL_REF=psvita-shared-ratelimit
WAF_MAX=5      # Free plan: 5 custom rules
RL_MAX=1       # Free plan: ONE rate-limiting rule
ACCOUNT_ID=""
TOKEN_FILE=""
THRESHOLD=100
EXTRA_PATHS=""
PHASE_NOW=""
PRINT_PLAN=0
SELF_CHECK=0
VERIFY_ONLY=0
REMOVE=0
REMOVE_SHARED=0
RESET=0
DROP=0
BOT_OFF=0
LOG_DIR=""
# Output filter for EVERYTHING that can hold API-derived or remote text before it reaches the console, a log or a file (note, die, errs, kept
# files): the account-name line goes, e-mail addresses become [EMAIL], every run of 32+ hex digits (28 plus the last 4 that stay visible) is shown as its last 4 only, and the exact
# token value (read below; empty until then) is replaced by [TOKEN] wherever it occurs, in case an API message ever echoes it.
scrub() {
  # the exact token first (before the pattern rules could cut it into pieces), then the pattern rules. The token is replaced by an awk
  # index()/substr() loop: a LITERAL search that behaves the same on macOS /bin/bash 3.2 and on bash 5 (a bash ${var//"$pat"/x} was only tested on 5.2;
  # glob characters in the token are not special here). The token reaches awk through its ENVIRONMENT, never argv.
  local scrub_tok="${TOK:-}"   # tok-ok
  SCRUB_TOK="$scrub_tok" LC_ALL=C awk '
    BEGIN { t = ENVIRON["SCRUB_TOK"]; n = length(t) }
    { line = $0
      if (n > 0) { out = ""; while ((i = index(line, t)) > 0) { out = out substr(line, 1, i - 1) "[TOKEN]"; line = substr(line, i + n) }; line = out line }
      print line }' |
  LC_ALL=C sed -E -e '/[Aa][Cc][Cc][Oo][Uu][Nn][Tt] [Nn][Aa][Mm][Ee]/s/.*/[account name line removed]/' \
         -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/[EMAIL]/g' \
         -e 's/[0-9A-Fa-f]{28,}([0-9A-Fa-f]{4})/[id ****\1]/g'
}
# A kept file is only ever written from scrubbed text: $1 = the raw file (in the private temp dir RAWD), $2 = the kept file. Nothing raw is ever
# written under the log directory, so a kill at any moment cannot leave a raw file there. A missing source gives no kept file.
keep() { [ -e "$1" ] || return 0; scrub < "$1" > "$2"; }
TOK=""
die() { echo "setup-psvita-zone: $*" | scrub >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --account-id) ACCOUNT_ID=${2:-}; shift 2 ;;
    --token-file) TOKEN_FILE=${2:-}; shift 2 ;;
    --threshold) THRESHOLD=${2:-}; shift 2 ;;
    --ratelimit-path-only) shift ;;   # no-op (the path form is the only form), kept for one release
    --ratelimit-extra-paths) EXTRA_PATHS=${2:-}; shift 2 ;;
    --print-plan) PRINT_PLAN=1; shift ;;
    --self-check) SELF_CHECK=1; shift ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    --remove) REMOVE=1; shift ;;
    --remove-shared-ratelimit) REMOVE_SHARED=1; shift ;;
    --ratelimit-drop-paths) DROP=1; shift ;;
    --ratelimit-reset) RESET=1; shift ;;
    --bot-fight-off) BOT_OFF=1; shift ;;
    --log-dir) LOG_DIR=${2:-}; shift 2 ;;
    -h|--help) awk 'NR>1 { if ($0 ~ /^set -/) exit; print }' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

# Self-check: curl runs only inside api(); the token variable appears only inside api() and on the lines that read and check it (marked tok-ok);
# nothing prints it. (api() is the function whose first line starts with "api()" and ends at the next line starting with "}".)
self_check() {
  awk 'BEGIN { c = "(^|[^A-Za-z0-9_./-])cur" "l( |$)"; t = "[$]TOK([^A-Za-z_]|$)|[$][{]TOK" }
       /^self_check\(\) *\{/ { skip = 1 }
       /^api\(\) *\{/ { inapi = 1 }
       { if (!skip && !inapi && $0 !~ /^[ \t]*#/ && $0 !~ /(die|note|echo) / && $0 ~ c) { print FILENAME ":" NR ": curl outside api(): " $0; bad = 1 }
         if (!skip && !inapi && $0 !~ /^[ \t]*#/ && $0 ~ t && $0 !~ /# tok-ok/) { print FILENAME ":" NR ": token used outside api(): " $0; bad = 1 } }
       /^\}/ { inapi = 0; skip = 0 }
       END { exit bad }' "$0"
}
if [ $SELF_CHECK -eq 1 ]; then
  self_check || die "curl or the token is used outside api() (lines above)"
  echo "self-check ok: curl runs only inside api(), the token is used only there"; exit 0
fi

[[ "$ACCOUNT_ID" =~ ^[0-9a-fA-F]{32}$ ]] || die "--account-id <32 hex characters> is required"
ACCOUNT_ID=$(printf '%s' "$ACCOUNT_ID" | tr A-F a-f)
[[ "$THRESHOLD" =~ ^[0-9]+$ ]] && [ "$THRESHOLD" -ge 10 ] && [ "$THRESHOLD" -le 1000 ] || die "--threshold must be a number from 10 to 1000"
[ -n "$TOKEN_FILE" ] || die "--token-file is required (the file holds the API token; it is never printed)"
if [ -n "$EXTRA_PATHS" ]; then
  IFS=',' read -r -a XP <<< "$EXTRA_PATHS"
  for x in "${XP[@]}"; do
    [[ "$x" =~ ^/[A-Za-z0-9/._~-]*\*?$ ]] || die "--ratelimit-extra-paths: '$x' is not a path (it must start with /, hold only letters, digits and / . _ ~ -, and may end in one *)"
  done
fi
[ $VERIFY_ONLY -eq 1 ] && [ $REMOVE -eq 1 ] && die "--verify-only and --remove exclude each other"
last4() { local v=$1; echo "****${v: -4}"; }

# ---------------------------------------------------------------- the rules we manage (compact JSON)
WAF_EXPR="(http.host eq \"$HOST\" and not starts_with(http.request.uri.path, \"/v1/\") and http.request.uri.path ne \"/times\" and not starts_with(http.request.uri.path, \"/.well-known/acme-challenge/\") and not starts_with(http.request.uri.path, \"/.well-known/pki-validation/\"))"
# The ONE shared rate-limiting rule: paths only, no host (the Free plan documents only Path as a field), so it serves every game of the zone.
RL_EXPR="(starts_with(http.request.uri.path, \"/v1/\") or http.request.uri.path eq \"/times\""
if [ -n "$EXTRA_PATHS" ]; then
  for x in "${XP[@]}"; do
    if [[ "$x" == *'*' ]]; then RL_EXPR="$RL_EXPR or starts_with(http.request.uri.path, \"${x%\*}\")"
    else RL_EXPR="$RL_EXPR or http.request.uri.path eq \"$x\""; fi
  done
fi
RL_EXPR="$RL_EXPR)"
mkjson() { # $1 ref  $2 description  $3 expression  $4 extra (a ,"ratelimit":{...} fragment or empty)
  node -e 'const [ref,d,e,x]=process.argv.slice(1); const o={ref,description:d,expression:e,action:"block",enabled:true}; if(x) o.ratelimit=JSON.parse(x); process.stdout.write(JSON.stringify(o));' "$1" "$2" "$3" "${4:-}"
}
WAF_RULE=$(mkjson "$WAF_REF" "smg-online (galaxy.psvita.dev): block every path the Worker does not serve [v1]" "$WAF_EXPR")
RL_RULE=$(mkjson "$RL_REF" "shared by the games on psvita.dev: per-IP rate limit on the game paths, $THRESHOLD requests / 10 s, block 10 s [v1]" "$RL_EXPR" \
  "{\"characteristics\":[\"cf.colo.id\",\"ip.src\"],\"period\":10,\"requests_per_period\":$THRESHOLD,\"mitigation_timeout\":10}")

print_rules() {
  echo "  WAF custom rule   (phase $WAF_PHASE, ref $WAF_REF), action block:"
  echo "      $WAF_EXPR"
  echo "  rate-limiting rule (phase $RL_PHASE, ref $RL_REF), action block, characteristics cf.colo.id + ip.src (Cloudflare lists cf.colo.id as mandatory: parameters page), $THRESHOLD requests per 10 s, mitigation 10 s:"
  echo "      $RL_EXPR"
}

if [ $PRINT_PLAN -eq 1 ]; then
  echo "PLAN (nothing is contacted, the token file is not opened). The account id is shown as its last 4 characters: $(last4 "$ACCOUNT_ID")"
  echo "1. GET $API/zones?name=$ZONE_NAME&account.id=<account>   (exactly one active zone, owned by the account)"
  echo "2. GET the entrypoints of $WAF_PHASE and $RL_PHASE of that zone (a missing entrypoint is an empty phase); other rules are kept as they are"
  echo "3. GET $API/zones/<zone>/bot_management and GET $API/zones/<zone>/settings/browser_check (read only; with the optional permissions missing they read as 'could not be read': said on the write path too, never a failure)."
  echo "   Bot Fight Mode that reads ON REFUSES the run BEFORE any write ('Nothing was written'); only the owner's --bot-fight-off ever changes it. Browser Integrity Check is only reported."
  echo "   Then print the exact plan; stop here with --verify-only"
  if [ $REMOVE -eq 1 ]; then
    echo "4. --remove: DELETE only our WAF rule ($WAF_REF), when present; the shared rate-limiting rule ($RL_REF) only with --remove-shared-ratelimit; refused (nothing written) if more than one rule in a phase carries our ref"
  else
    echo "4. per phase: no entrypoint -> create it (POST /rulesets) with our one rule; entrypoint without our rule -> POST our rule to it; our rule differs -> PATCH it; equal -> nothing"
    echo "5. read the phases again and check our rules are there exactly as planned"
  fi
  echo "The rules:"; print_rules
  exit 0
fi

command -v curl >/dev/null || die "curl is needed"
command -v node >/dev/null || die "node is needed (JSON)"
if [ ! -r "$TOKEN_FILE" ]; then
  echo "setup-psvita-zone: cannot read the token file (~/.cf-psvita-token). Nothing was contacted. To create it (the owner, once, in the dashboard):" >&2
  echo "  1. My Profile > API Tokens > Create Token > Create Custom Token." >&2
  echo "  2. Permissions, required: Zone > Zone > Read; Zone > Zone WAF > Edit. Optional: Zone > Zone Settings > Read (only for reading Browser Integrity Check) and Zone > Bot Management > Read (only for reading Bot Fight Mode)." >&2
  echo "  3. Zone Resources: Include > Specific zone > psvita.dev ONLY (no account-wide permission, nothing for any other zone)." >&2
  echo "  4. Create, copy the token once, save it as one line in ~/.cf-psvita-token (chmod 600). Then run this script again." >&2
  exit 1
fi
# the file is documented as mode 600: refuse a token file that group or others can read or write (ls -l is the same on macOS and GNU)
tmode=$(ls -ldL -- "$TOKEN_FILE" 2>/dev/null | cut -c1-10)
{ [ -f "$TOKEN_FILE" ] && [ "${tmode:4:6}" = "------" ]; } || die "the token file (~/.cf-psvita-token) must be a regular file that only its owner can read and write (mode 600); it is '${tmode:-unknown}': run chmod 600 on it. Refusing to use it"
IFS= read -r TOK < "$TOKEN_FILE" || true   # tok-ok
[ ${#TOK} -ge 30 ] || die "the token file is shorter than 30 characters"   # tok-ok
[[ "$TOK" =~ ^[A-Za-z0-9._~+/=-]+$ ]] || die "the token file holds characters other than letters, digits and . _ ~ + / = -"   # tok-ok
LOG_DIR=${LOG_DIR:-zone-logs/$(date -u +%Y%m%dT%H%M%SZ)}
mkdir -p "$LOG_DIR"
STEP=0
note() { echo "[$(date -u +%H:%M:%S)] $*" | scrub | tee -a "$LOG_DIR/summary.txt"; }
# Raw remote text (curl's diagnostics) goes ONLY into this private (mode 700) temp dir, removed by the trap; only scrubbed text is written to the kept
# files under $LOG_DIR. (A kill that cannot run the trap, such as SIGKILL, leaves a mode-700 dir in the temp area, never a raw file in the logs.)
RAWD=$(mktemp -d "${TMPDIR:-/tmp}/zone-raw.XXXXXX") || die "cannot make a private temp directory"
chmod 700 "$RAWD"
# Exact bookkeeping of the writes: W_PLANNED = every write this run set out to make ('|' separated), W_DONE = those that the API accepted. From the first
# write on, EVERY exit path (success, a refused write, a failed confirmation, a die anywhere) prints one line "written: ...; not written: ...".
WRITE_PHASE=0; W_PLANNED=""; W_DONE=""; W_UNK=""; CUR_OP=""; FINAL_PRINTED=0; BOT_NOTE=""
add_planned() { W_PLANNED="${W_PLANNED:+$W_PLANNED|}$1"; }
writes_text() {
  local l w="" n="" u=""
  while IFS= read -r l; do
    [ -n "$l" ] || continue
    case "|$W_DONE|" in *"|$l|"*) w="${w:+$w, }$l"; continue ;; esac
    case "|$W_UNK|" in *"|$l|"*) u="${u:+$u, }$l" ;; *) n="${n:+$n, }$l" ;; esac
  done <<< "$(printf '%s' "$W_PLANNED" | tr '|' '\n')"
  echo "written: ${w:-none}; not written: ${n:-none}${u:+; no answer: unknown; a rerun reads the truth: $u}"
}
final_notes() {
  [ $FINAL_PRINTED -eq 0 ] || return 0
  FINAL_PRINTED=1
  [ -z "$BOT_NOTE" ] || note "!!! $BOT_NOTE"
  note "$(writes_text)"
}
on_exit() { local rc=$?; trap - EXIT; if [ $WRITE_PHASE -eq 1 ]; then final_notes || true; fi; rm -rf "$RAWD"; exit $rc; }
trap on_exit EXIT
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM

# THE ONLY place curl is run. $1 name, $2 method, $3 path (under /client/v4), $4 optional JSON body. Sets CODE (000 = no answer) and BODY (the raw reply,
# kept in memory only: it can hold the account id, name and e-mail; only summaries of it are ever printed or written, see errs() / jq-free node helpers).
# The token and the body reach curl as config lines on stdin (`curl -K -`), made by the printf builtin: never on a command line or in a file.
api() {
  STEP=$((STEP + 1))
  local base="$LOG_DIR/$(printf %02d $STEP)-$1" raw esc cfg rawerr="$RAWD/curl-error"
  echo "$2 $3" | scrub > "$base.request"
  cfg=$(printf 'header = "Authorization: Bearer %s"\nrequest = "%s"\nheader = "User-Agent: smg-zone-setup"\nurl = "%s%s"' "$TOK" "$2" "$API" "$3")
  if [ $# -ge 4 ]; then
    esc=$(printf '%s' "$4" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g')
    cfg="$cfg"$(printf '\nheader = "Content-Type: application/json"\ndata-binary = "%s"' "$esc")
  fi
  raw=$(printf '%s\n' "$cfg" | curl -q -sS --max-time 30 -w '\n%{http_code}' -K - 2> "$rawerr") || true
  keep "$rawerr" "$base.curl-error"; rm -f "$rawerr"   # curl's own diagnostic is remote-derived text too: only its scrubbed copy (token, ids, e-mail) is kept
  cfg=""
  CODE=${raw##*$'\n'}
  [[ "$CODE" =~ ^[0-9]{3}$ ]] || CODE=000
  BODY=${raw%$'\n'*}
  raw=""
  echo "$CODE" > "$base.status"
  LAST=$base
}

# summary of the errors of a reply: "code: message" lines (scrubbed)
# (node's own stderr goes to /dev/null: an uncaught JSON error would print the first line of the remote body)
errs() { printf '%s' "$BODY" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{try{const j=JSON.parse(s);for(const e of (j.errors||[]))console.log(e.code+": "+e.message);}catch{console.log("(no JSON in the reply)")}})' 2>/dev/null | scrub; }
# classify a refusal. 401 = the token is not valid or lacks the permission. 403 = a missing permission too, UNLESS the API's own message speaks of the
# plan / entitlement / an unavailable feature: then it is a refusal by the plan, not a permission problem. The API's message is always printed first.
perm_check() { # $1 what the call was, $2 the permission the token needs
  if [ "$CODE" = 401 ] || [ "$CODE" = 403 ]; then
    local m; m=$(errs | sed -n '1,3p' | tr '\n' ' ')
    note "$1: HTTP $CODE, the API says: $m"
    if [ "$CODE" = 403 ] && printf '%s' "$m" | grep -iE 'plan|entitle|upgrade|subscription' >/dev/null; then
      note "!!! $1: refused by the PLAN or an entitlement, not (as far as the message shows) by a missing token permission. Nothing was changed by this call."
      die "refused by the plan: $1"
    fi
    note "!!! $1: the token lacks a permission or is not valid. Needed: $2."
    note "Add the permission to the token (dashboard > My Profile > API Tokens > the token > Edit), then run this script again; nothing was changed by this call."
    die "missing permission: $2"
  fi
}
# $1 = node expression over j (the parsed reply); prints its value
# (the expression runs inside a try as well, and node's stderr goes to /dev/null: nothing of the remote body can reach the console through a node error)
jv() { printf '%s' "$BODY" | node -e 'let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{let j;try{j=JSON.parse(s)}catch{j={}};let r;try{r=('"$1"')}catch{r=""};process.stdout.write(r===undefined||r===null?"":String(r))})' 2>/dev/null; }

# ---------------------------------------------------------------- 1. the zone
api zone GET "/zones?name=$ZONE_NAME&account.id=$ACCOUNT_ID"
perm_check "zone lookup" "Zone > Zone > Read"
[ "$CODE" = 200 ] || { note "zone lookup: HTTP $CODE ($(errs | sed -n '1,2p' | tr '\n' ' ')); see $LAST.*"; die "zone lookup failed"; }
NZ=$(jv '(j.result||[]).length')
[ "$NZ" = 1 ] || die "expected exactly one zone named $ZONE_NAME in account $(last4 "$ACCOUNT_ID"), found ${NZ:-0}: is the domain added to this account?"
ZID=$(jv 'j.result[0].id')
ZSTATUS=$(jv 'j.result[0].status')
ZACC=$(jv 'j.result[0].account && j.result[0].account.id')
[[ "$ZID" =~ ^[0-9a-f]{32}$ ]] || die "the zone id has an unexpected shape"
[ "$ZACC" = "$ACCOUNT_ID" ] || die "the zone $ZONE_NAME does not belong to account $(last4 "$ACCOUNT_ID"): refusing"
[ "$ZSTATUS" = active ] || die "the zone $ZONE_NAME is not active (status: ${ZSTATUS:-unknown}): finish the domain's setup first"
note "zone $ZONE_NAME found: active, id $(last4 "$ZID"), belongs to account $(last4 "$ACCOUNT_ID")"

# ---------------------------------------------------------------- 2. read the two phases
# state per phase: missing (no entrypoint yet) | absent (entrypoint, none of ours) | same | differs | ambiguous | foreign-full (no room for ours)
# RSET = ruleset id, RRULE = our rule's id, ROTHERS = number of other rules
read_phase() { # $1 phase $2 our ref $3 desired rule json $4 max rules
  api "read-$1" GET "/zones/$ZID/rulesets/phases/$1/entrypoint"
  perm_check "read of the $1 rules" "Zone > Zone WAF > Edit (or Read)"
  PSTATE=""; RSET=""; RRULE=""; ROTHERS=0; LIVE_EXPR=""; LIVE_FLAGS=""; LIVE_FIELDS=""; LIVE_FOREIGN=""
  if [ "$CODE" = 404 ]; then PSTATE=missing; return 0; fi
  [ "$CODE" = 200 ] || { note "read of $1: HTTP $CODE ($(errs | sed -n '1,2p' | tr '\n' ' ')); see $LAST.*"; die "cannot read the $1 rules"; }
  local out
  out=$(printf '%s' "$BODY" | node -e '
    const ref=process.argv[1], want=JSON.parse(process.argv[2]), max=+process.argv[3];
    let s="";process.stdin.on("data",d=>s+=d).on("end",()=>{
     const L=[];
     try{
      const j=JSON.parse(s), r=j.result||{}, rules=r.rules||[];
      const mine=rules.filter(x=>x.ref===ref), others=rules.length-mine.length;
      const norm=e=>String(e||"").replace(/\s+/g,"").replace(/^\((.*)\)$/,"$1");
      const pick=x=>JSON.stringify({d:x.description,e:norm(x.expression),a:x.action,on:x.enabled!==false,
        rl:x.ratelimit?{c:(x.ratelimit.characteristics||[]).slice().sort(),p:x.ratelimit.period,n:x.ratelimit.requests_per_period,m:x.ratelimit.mitigation_timeout}:null});
      const terms=e=>new Set([...String(e||"").matchAll(/starts_with\(http\.request\.uri\.path, *"([^"]*)"\)|http\.request\.uri\.path eq *"([^"]*)"/g)].map(m=>m[1]!==undefined?"sw:"+m[1]:"eq:"+m[2]));
      let st, live="", flags="0 0", fields="";
      if(mine.length>1) st="ambiguous";
      else if(mine.length===1) {
        st=pick(mine[0])===pick(want)?"same":"differs";
        if(st==="differs"&&ref==="psvita-shared-ratelimit"){
          live=String(mine[0].expression||"").replace(/\s+/g," ");
          // D: the live expression holds anything the plan lacks: a recognised path term the plan does not have, or ANY residue once the recognised terms,
          // "or", parentheses and spaces are removed (a hand-written contains / wildcard / in {} / == / lower() / full_uri term of another game)
          const w=terms(want.expression);
          const residue=live.replace(/starts_with\(http\.request\.uri\.path, *"[^"]*"\)|http\.request\.uri\.path eq *"[^"]*"/g,"").replace(/\bor\b/g,"").replace(/[()\s]/g,"");
          const D=residue!==""||[...terms(mine[0].expression)].some(t=>!w.has(t));
          // R: a field other than the expression differs (threshold, period, mitigation, characteristics, description, action, enabled): it affects the other game too
          const nx=x=>{const o=JSON.parse(pick(x)); delete o.e; return JSON.stringify(o)};
          const R=nx(mine[0])!==nx(want);
          flags=(D?1:0)+" "+(R?1:0);
          const rl=x=>x.ratelimit?x.ratelimit.requests_per_period+" requests / "+x.ratelimit.period+" s, mitigation "+x.ratelimit.mitigation_timeout+" s, by "+(x.ratelimit.characteristics||[]).join("+"):"no rate limit";
          fields="live fields: "+rl(mine[0])+"; action "+mine[0].action+"; "+(mine[0].enabled===false?"disabled":"enabled")+"; description: "+mine[0].description+"\nplan fields: "+rl(want)+"; action "+want.action+"; enabled; description: "+want.description;
        }
      }
      else st=rules.length>=max?"foreign-full":"absent";
      L.push([st,r.id||"-",mine.length?mine[0].id:"-",others].join(" "));
      L.push(live);
      L.push(flags);
      L.push(fields.replace(/\n/g,"\u0001"));
      // every rule that is not ours: ref and description (shown scrubbed when the script stops because of them, and in the plan)
      L.push(rules.filter(x=>x.ref!==ref).map(x=>"ref="+x.ref+" description="+String(x.description||"").replace(/\s+/g," ")).join("\u0001"));
     }catch(e){ process.exitCode=2; return; }   // a bad or truncated reply: NOTHING of it is printed (the shell prints a fixed message)
     process.stdout.write(L.join("\n")+"\n");
    });' "$2" "$3" "$4" 2>/dev/null) || die "cannot read the reply of the $1 phase: the answer (HTTP $CODE) is not the JSON the API documents (truncated or malformed); nothing of it is shown. Nothing was written by this read"
  read -r PSTATE RSET RRULE ROTHERS <<< "$(printf '%s\n' "$out" | sed -n 1p)"
  LIVE_EXPR=$(printf '%s\n' "$out" | sed -n 2p)
  LIVE_FLAGS=$(printf '%s\n' "$out" | sed -n 3p); LIVE_FIELDS=$(printf '%s\n' "$out" | sed -n 4p | tr '\001' '\n')
  LIVE_FOREIGN=$(printf '%s\n' "$out" | sed -n 5p | tr '\001' '\n')
  # the ids and the state come from the remote reply: only their documented shapes are used
  case "$PSTATE" in missing|absent|same|differs|ambiguous|foreign-full) ;; *) die "the reply of the $1 phase has an unexpected shape; nothing of it is shown" ;; esac
  [[ "$RSET" =~ ^[A-Za-z0-9_-]+$ ]] && [[ "$RRULE" =~ ^[A-Za-z0-9_-]+$ ]] && [[ "${ROTHERS:-}" =~ ^[0-9]+$ ]] || die "the reply of the $1 phase has an unexpected shape (ids); nothing of it is shown"
  [ "$RSET" = - ] && RSET=""; [ "$RRULE" = - ] && RRULE=""; return 0
}
read_phase "$WAF_PHASE" "$WAF_REF" "$WAF_RULE" "$WAF_MAX"; WAF_STATE=$PSTATE; WAF_SET=$RSET; WAF_RID=$RRULE; WAF_OTH=$ROTHERS; WAF_FOREIGN=$LIVE_FOREIGN
read_phase "$RL_PHASE" "$RL_REF" "$RL_RULE" "$RL_MAX"; RL_STATE=$PSTATE; RL_SET=$RSET; RL_RID=$RRULE; RL_OTH=$ROTHERS; RL_LIVE=$LIVE_EXPR; RL_FLAGS=${LIVE_FLAGS:-"0 0"}; RL_FIELDS=$LIVE_FIELDS; RL_FOREIGN=$LIVE_FOREIGN

describe() { # state, others, name
  case "$1" in
    missing)      echo "$3: no rules in this phase yet -> CREATE the phase's entrypoint with our rule" ;;
    absent)       echo "$3: $2 other rule(s) there, kept as they are -> ADD our rule" ;;
    same)         echo "$3: our rule is there exactly as planned -> UNCHANGED ($2 other rule(s) kept)" ;;
    differs)      echo "$3: our rule is there but differs -> UPDATE it ($2 other rule(s) kept)" ;;
    ambiguous)    echo "$3: MORE THAN ONE rule with our ref -> STOP" ;;
    foreign-full) echo "$3: the phase is full of rules that are not ours ($2) and the plan allows no more -> STOP (nothing is dropped)" ;;
  esac
}
if [ "$RL_STATE" = differs ]; then
  read -r RL_D RL_R <<< "$RL_FLAGS"
  if [ "$RL_D" = 1 ] || [ "$RL_R" = 1 ]; then
    note "!!! the live shared rate-limiting rule differs from the plan in a way that may matter to the other game on the zone. Both expressions:"
    note "  live: $(printf '%s' "$RL_LIVE" | scrub)"
    note "  plan: $(printf '%s' "$RL_EXPR" | scrub)"
    [ "$RL_R" = 1 ] && printf '%s\n' "$RL_FIELDS" | scrub | sed 's/^/  /' | tee -a "$LOG_DIR/summary.txt"
    HOLD=""
    [ "$RL_D" = 1 ] && [ $DROP -eq 0 ] && HOLD="$HOLD it holds terms the plan lacks (another game's paths or a hand-written term): Owner's decision only: to keep them use --ratelimit-extra-paths '<their paths>', to drop them --ratelimit-drop-paths;"
    [ "$RL_R" = 1 ] && [ $RESET -eq 0 ] && HOLD="$HOLD its threshold, period, mitigation, characteristics or description differ. Owner's decision only: --ratelimit-reset resets them to the plan;"
    if [ -n "$HOLD" ]; then
      if [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ]; then die "refusing to PATCH the shared rate-limiting rule; nothing was written.$HOLD"; fi
    else note "--ratelimit-drop-paths / --ratelimit-reset given: the rule will be PATCHed to the plan"; fi
  fi
fi
list_foreign() { local l; [ -n "$2" ] || return 0; while IFS= read -r l; do note "  not ours, in the $1 phase, kept as it is: $l"; done <<< "$2"; }
note "the plan (nothing has been written yet):"
if [ $REMOVE -eq 1 ]; then
  note "  --remove: delete only our WAF rule ($WAF_PHASE): $([ -n "$WAF_RID" ] && echo "present -> DELETE" || echo "not there -> nothing"). Shared rate-limiting rule ($RL_PHASE): $([ $REMOVE_SHARED -eq 1 ] && { [ -n "$RL_RID" ] && echo "present -> DELETE (--remove-shared-ratelimit)" || echo "not there -> nothing"; } || echo "KEPT: it serves the other game on the zone too (add --remove-shared-ratelimit to delete it)")"
else
  note "  $(describe "$WAF_STATE" "$WAF_OTH" "WAF custom rule")"
  note "  $(describe "$RL_STATE" "$RL_OTH" "rate-limiting rule")"
  list_foreign "$WAF_PHASE" "$WAF_FOREIGN"; list_foreign "$RL_PHASE" "$RL_FOREIGN"
  print_rules | sed 's/^/  /' | tee -a "$LOG_DIR/summary.txt"
fi

# Refusals that must come BEFORE any write (Bot Fight Mode below can be written by --bot-fight-off): a phase this plan cannot join stops everything.
if [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 1 ]; then
  # --remove deletes by rule id: with more than one rule carrying our ref it cannot know which is ours to delete, so it deletes nothing
  case "$WAF_STATE" in ambiguous) die "--remove: WAF custom rule:$(describe ambiguous "$WAF_OTH" x | cut -d: -f2-). --remove never guesses which of several rules to delete. Nothing was written" ;; esac
  case "$RL_STATE" in ambiguous) die "--remove: rate-limiting rule:$(describe ambiguous "$RL_OTH" x | cut -d: -f2-). --remove never guesses which of several rules to delete. Nothing was written" ;; esac
fi
if [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ]; then
  case "$WAF_STATE" in ambiguous|foreign-full) die "WAF custom rule:$(describe "$WAF_STATE" "$WAF_OTH" x | cut -d: -f2-). Nothing was written" ;; esac
  case "$RL_STATE" in ambiguous|foreign-full) die "rate-limiting rule:$(describe "$RL_STATE" "$RL_OTH" x | cut -d: -f2-). The Free plan has ONE rate-limiting rule and it is not ours; this script never replaces it. Nothing was written" ;; esac
fi

# Bot Fight Mode: read only, if the token may read it. It must be OFF; only --bot-fight-off changes it.
BOT=unknown
api bot GET "/zones/$ZID/bot_management"
if [ "$CODE" = 200 ]; then
  case "$(jv 'j.result && j.result.fight_mode')" in true) BOT=ON ;; false) BOT=off ;; esac
fi

# Browser Integrity Check: a zone-wide setting, read only and reported as text; this script never changes it.
BIC=unknown
api bic GET "/zones/$ZID/settings/browser_check"
if [ "$CODE" = 200 ]; then
  case "$(jv 'j.result && j.result.value')" in on) BIC=ON ;; off) BIC=off ;; esac
fi

FAIL=0
if [ "$BOT" = unknown ] && [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ]; then
  # (the Bot Management Read permission of the token is optional: without it the script cannot see Bot Fight Mode, and says so on the write path too)
  BOT_NOTE="Bot Fight Mode could not be read with this token; check it in the dashboard (Security > Bots): it must be OFF"
  note "!!! $BOT_NOTE"
fi
if [ "$BOT" = ON ] && [ $BOT_OFF -eq 1 ] && [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ]; then
  WRITE_PHASE=1; add_planned "Bot Fight Mode (switch off)"
  api bot-off PUT "/zones/$ZID/bot_management" '{"fight_mode":false}'
  if [ "$CODE" = 200 ]; then note "Bot Fight Mode switched OFF (--bot-fight-off)"; BOT=off; W_DONE="${W_DONE:+$W_DONE|}Bot Fight Mode (switch off)"
  else note "switching Bot Fight Mode off: HTTP $CODE, the API says: $(errs | sed -n '1,2p' | tr '\n' ' '): turn it off by hand"; fi
fi
if [ "$BOT" = ON ] && [ $VERIFY_ONLY -eq 0 ] && [ $REMOVE -eq 0 ]; then
  die "Bot Fight Mode is ON for $ZONE_NAME (it may challenge the games' API calls; it cannot be skipped by rules). Nothing was written. Owner's decision only: turning it off by hand (Security > Settings > Bot traffic) or with --bot-fight-off; this script does not do it unasked"
fi
if [ $VERIFY_ONLY -eq 1 ]; then
  if [ $REMOVE -eq 0 ]; then
    [ "$WAF_STATE" = same ] && note "WAF custom rule: as planned" || { note "WAF custom rule: NOT as planned ($WAF_STATE)"; FAIL=1; }
    [ "$RL_STATE" = same ] && note "rate-limiting rule: as planned" || { note "rate-limiting rule: NOT as planned ($RL_STATE)"; FAIL=1; }
  fi
  case "$BOT" in
    ON) note "!!! Bot Fight Mode is ON for $ZONE_NAME: it may challenge the game's API calls. Owner's decision only: turning it off by hand (Security > Settings > Bot traffic) or with --bot-fight-off (the script never changes it otherwise)."; FAIL=1 ;;
    off) note "Bot Fight Mode: off, as required" ;;
    *) note "Bot Fight Mode: could not be read with this token (needs a Bot Management read permission): check by hand that it is OFF" ;;
  esac
  case "$BIC" in
    ON) note "Browser Integrity Check: ON for $ZONE_NAME (zone-wide, so for every game on it; reported only, this script never changes it. It can refuse requests without a normal User-Agent header: if a game's console client is refused with a 403 from Cloudflare, look here)" ;;
    off) note "Browser Integrity Check: off (zone-wide; reported only)" ;;
    *) note "Browser Integrity Check: could not be read with this token (it needs a Zone Settings read permission): not changed, check by hand if clients are refused" ;;
  esac
  [ $FAIL -eq 0 ] && note "ALL CHECKS PASSED" || note "SOME CHECKS FAILED: read $LOG_DIR/summary.txt"
  exit $FAIL
fi

# ---------------------------------------------------------------- 3. write (only what the plan says)
write_check() { # $1 name  (after a write call)
  case "$CODE" in
    200) note "$1: done"; W_DONE="${W_DONE:+$W_DONE|}$CUR_OP" ;;
    000) note "$1: no answer (HTTP 000): whether the write happened is unknown; a rerun reads the truth"; W_UNK="${W_UNK:+$W_UNK|}$CUR_OP"; die "$1: no answer" ;;
    *) note "$1: HTTP $CODE, the API says: $(errs | sed -n '1,3p' | tr '\n' ' ')(see $LAST.*)"
       perm_check "$1" "Zone > Zone WAF > Edit"
       note "(a refusal of the expression or the rate-limit fields means this plan does not fit the zone's plan; read the message above. What was and was not written is in the 'written: ...; not written: ...' line that follows)"
       die "$1 failed" ;;
  esac
}
WRITE_PHASE=1
if [ $REMOVE -eq 1 ]; then
  [ -z "$WAF_RID" ] || add_planned "WAF rule $WAF_REF (delete)"
  { [ $REMOVE_SHARED -eq 1 ] && [ -n "$RL_RID" ]; } && add_planned "rate-limit rule $RL_REF (delete)"
  if [ -n "$WAF_RID" ]; then CUR_OP="WAF rule $WAF_REF (delete)"; api del-waf DELETE "/zones/$ZID/rulesets/$WAF_SET/rules/$WAF_RID"; write_check "delete of the WAF rule"; fi
  if [ $REMOVE_SHARED -eq 0 ]; then note "the shared rate-limiting rule was NOT removed: the other game on psvita.dev uses it too (owner's decision only: --remove-shared-ratelimit deletes it)"; fi
  if [ $REMOVE_SHARED -eq 1 ] && [ -n "$RL_RID" ]; then CUR_OP="rate-limit rule $RL_REF (delete)"; api del-rl DELETE "/zones/$ZID/rulesets/$RL_SET/rules/$RL_RID"; write_check "delete of the rate-limiting rule"; fi
else
  opname() { case "$1" in missing) echo create ;; absent) echo add ;; differs) echo update ;; esac; }   # (same -> nothing to write)
  [ "$WAF_STATE" = same ] || add_planned "WAF rule $WAF_REF ($(opname "$WAF_STATE"))"
  [ "$RL_STATE" = same ] || add_planned "rate-limit rule $RL_REF ($(opname "$RL_STATE"))"
  apply() { # $1 name $2 phase $3 state $4 ruleset id $5 rule id $6 rule json $7 ref
    PHASE_NOW=$2
    [ "$3" = same ] || CUR_OP="$([ "$1" = waf ] && echo WAF || echo rate-limit) rule $7 ($(opname "$3"))"
    case "$3" in
      missing) api "new-$1" POST "/zones/$ZID/rulesets" "{\"name\":\"smg-online $2\",\"kind\":\"zone\",\"phase\":\"$2\",\"rules\":[$6]}"; write_check "creating the $1 phase entrypoint with our rule" ;;
      absent)  api "add-$1" POST "/zones/$ZID/rulesets/$4/rules" "$6"; write_check "adding the $1 rule" ;;
      differs) api "upd-$1" PATCH "/zones/$ZID/rulesets/$4/rules/$5" "$6"; write_check "updating the $1 rule" ;;
      same)    note "$1: unchanged" ;;
    esac
  }
  apply waf "$WAF_PHASE" "$WAF_STATE" "$WAF_SET" "$WAF_RID" "$WAF_RULE" "$WAF_REF"
  apply ratelimit "$RL_PHASE" "$RL_STATE" "$RL_SET" "$RL_RID" "$RL_RULE" "$RL_REF"
fi

# ---------------------------------------------------------------- 4. read again and confirm
read_phase "$WAF_PHASE" "$WAF_REF" "$WAF_RULE" "$WAF_MAX"; W2=$PSTATE; WR2=$RRULE
read_phase "$RL_PHASE" "$RL_REF" "$RL_RULE" "$RL_MAX"; R2=$PSTATE; RR2=$RRULE
if [ $REMOVE -eq 1 ]; then
  if [ -z "$WR2" ] && { [ $REMOVE_SHARED -eq 0 ] || [ -z "$RR2" ]; }; then note "the rules asked for are gone; other rules were not touched"; else note "a rule of ours is still there"; FAIL=1; fi
else
  [ "$W2" = same ] && note "WAF custom rule: confirmed as planned" || { note "WAF custom rule after the write: $W2 (expected same)"; FAIL=1; }
  [ "$R2" = same ] && note "rate-limiting rule: confirmed as planned" || { note "rate-limiting rule after the write: $R2 (expected same)"; FAIL=1; }
fi
[ "$BOT" = ON ] && { note "!!! Bot Fight Mode is ON for $ZONE_NAME (this run did not change it). Owner's decision only: turning it off by hand or with --bot-fight-off; it may challenge the game's API calls."; FAIL=1; }
final_notes   # the exact "written: ...; not written: ..." line (and the Bot Fight Mode note when it could not be read), just before the verdict
[ $FAIL -eq 0 ] && note "ALL CHECKS PASSED" || note "SOME CHECKS FAILED: read $LOG_DIR/summary.txt"
exit $FAIL
