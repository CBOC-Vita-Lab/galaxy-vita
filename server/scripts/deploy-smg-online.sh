#!/bin/bash
# Deploys (or only checks) the Galaxy online service, smg-online. See DEPLOY-SMG.txt "Deploying with the script".
#
#   scripts/deploy-smg-online.sh --account-id <32 hex> --admin-token-file ~/.smg-admin-token [options]
#
#   --url URL              the service's public address. Default and normal value: https://galaxy.psvita.dev (the Worker custom domain the owner attached
#                          in the Cloudflare dashboard; wrangler.smg.toml declares none). The old https://smg-online.<subdomain>.workers.dev is still accepted (it stays on while workers_dev = true
#                          in the config, until the release). There is NO fallback between the two: the checks below run against each address that is
#                          meant to answer, and each must pass.
#
#   --account-id ID        REQUIRED. The Cloudflare account to act on (32 hex characters). Exported as CLOUDFLARE_ACCOUNT_ID for every wrangler call;
#                          refused unless `wrangler whoami` lists it. This pin is what prevents acting on a wrong account.
#   --expect-subdomain S   the workers.dev subdomain the service has (default: taken from --url when that is a workers.dev address). Every
#                          workers.dev URL wrangler reports (dry run and deploy) must be exactly https://smg-online.S.workers.dev. Without S (custom
#                          --url only) a reported workers.dev URL must at least be https://smg-online.<something>.workers.dev (never another worker).
#                          wrangler must report NO psvita.dev name at all (the config has no route and no custom domain: wrangler never publishes them).
#                          The dry run only builds (it uploads and publishes
#                          nothing) and does not always print the addresses; the first check that can truly SEE them is the deploy output, and a mismatch
#                          after the real deploy stops loudly, after the fact.
#   --print-plan           print what would be run (the account id shown as its last 4 characters; the token file is not opened) and exit 0; contacts and writes nothing
#   --self-check           check that no wrangler call exists outside the wr() function, then exit
#   --secret-list          read and print the secret NAMES of the Worker smg-online (names only), then exit; deploys nothing
#   --put-secret NAME      set ONE secret of the existing Worker, then exit; the value comes on STDIN only (one line, never an argument, never printed or kept).
#                          NAME is one of TURN_USAGE_TOKEN, TURN_ACCOUNT_ID, TURN_KEY_ID, TURN_KEY_API_TOKEN; TURN_ACCOUNT_ID must equal --account-id.
#                          Refused unless `wrangler secret list` proves the Worker exists (secret put would otherwise CREATE a stub Worker).
#                          Prints `secret put: NAME ok (value not printed)` or `secret put: NAME FAILED (value not printed)`.
#                          A real deploy always runs `wrangler deploy --strict`: wrangler aborts on a conflicting remote change instead of overwriting it, and the
#                          script refuses a Worker whose current deployment is split over several versions.
#   --verify-only          deploy nothing, set no secret (the secret NAMES are read, and the relay self-check runs when the relay is on): run the checks against what is live: the custom domain (https://galaxy.psvita.dev) and,
#                          while wrangler.smg.toml has workers_dev = true and the workers.dev subdomain is known, the workers.dev address too
#   --new-admin-token      create a new admin token and set it as the secret (refused if --admin-token-file exists; the file, mode 600, is written only
#                          at the last moment, right before the secret is set, after every check that can refuse)
#   --set-admin-token      set the secret from the existing --admin-token-file (after a rotation done elsewhere)
#   --skip-tests           do not run the local test suites first (they contact nothing)
#   --static-assets        also build ./public (scripts/build-assets.mjs: the /times page) and deploy it as static assets, which the platform serves
#                          without running the Worker (free, not counted against the 100,000 requests a day). UNTESTED against a live wrangler.
#   --log-dir DIR          where every reply is kept (default deploy-logs/<UTC time>)
#
#   THE RELAY (Cloudflare Realtime TURN). The Worker secrets TURN_KEY_ID and TURN_KEY_API_TOKEN (and, for the hard usage stop, TURN_USAGE_TOKEN) are
#   normally set by the owner in the dashboard BEFORE this script runs; the script reads their NAMES (`wrangler secret list`; never a value) and decides:
#     both key secrets present   relay: on. The admin self-check (POST /v1/admin/turn-selfcheck: mint a 300 s pass, revoke it, read the usage numbers)
#                                is REQUIRED in the live checks and in --verify-only, and must pass; with TURN_USAGE_TOKEN its usage must be ok.
#     only one of the two        refused before the deploy ("relay half set up: ...")
#     neither                    "relay: off (no TURN key secrets)"; the run goes on (it is a refusal only with --require-relay)
#   --require-relay        "relay: off" becomes a refusal (before the deploy)
#   --turn-key-file FILE   the second way in: a mode-600 file of TWO lines: the TURN key id (32 hex), then the key's API token. After the deploy and after
#                          every check that can refuse, both are set as the secrets TURN_KEY_ID and TURN_KEY_API_TOKEN (on stdin only; never printed).
#   --turn-usage-token-file FILE   optional, ONE line (mode 600): the read-only analytics token; set as TURN_USAGE_TOKEN the same way.
#   Whenever TURN_USAGE_TOKEN exists (by name or from the file) the script also sets TURN_ACCOUNT_ID from --account-id, on stdin, on every run
#   (not with --verify-only). Neither file option is allowed with --verify-only.
#
# workers_dev must be true or false at the top level of the config, and agree with --expect-subdomain: true needs the flag (that address is checked too),
# false must not have one (the later release step: only the custom domain is checked).
# Refused up front: a token file that is not a regular file only its owner can read and write (mode 600); a wrangler.smg.toml whose TOP-LEVEL name is
# not smg-online (turning workers_dev off is the owner's release step; this script never does it).
# Raw remote text (replies, headers, curl diagnostics) lives only in a private temp dir removed on exit; the kept files hold scrubbed text only.
#
# NOT DONE BY THIS SCRIPT: the zone's security rules (WAF custom rule, the rate-limiting rule): scripts/setup-psvita-zone.sh does those
# (docs/ONLINE-DOMAIN-SETUP.md). Bot Fight Mode stays OFF on psvita.dev (it cannot be skipped by rules and may challenge the game's API calls).
# Nothing in the Worker can stop requests that reach it: the platform has counted them already.
#
# Rules this script keeps: every wrangler call carries --config wrangler.smg.toml (a call without it acts on the Strikers service);
# every wrangler call goes through wr() (config, account env, output filter); `wrangler whoami` output is never stored (only OK or the
# failure reason; only the account ids are read from it); every other wrangler output passes scrub() (e-mail addresses and account-name
# rows removed) before it reaches a file; no secret is ever printed, logged, written to disk or put on a command line (it goes to wrangler
# on stdin and to curl as a config line on stdin, `curl -K -`, made by a shell builtin);
# every HTTP reply is kept (status, headers, body) next to the request that produced it; the admin check is retried for up to two
# minutes while the answer is 404 (the Worker does not see the secret yet) or no answer at all, and stops at once on anything else.
set -euo pipefail
# (pipefail: never end a pipeline with a command that can stop reading early (head, grep -q, grep -m, sed q, awk exit, read): the writer takes SIGPIPE, pipefail makes the pipeline fail, and set -e exits before the die text. Use sed -n "1,20p" or grep ... >/dev/null; the only exceptions carry "# pipefail-ok: why", and a test checks it.)
umask 077
unset TOK TK_ID TK_TOKEN TU_TOKEN PUT_VALUE   # a secret never arrives through the environment
ORIG_LOG_DIR=${LOG_DIR:-}   # (the script's own LOG_DIR below is a shell variable of the same name: the ENVIRONMENT's value is read here, first)
ORIG_LOG_SANITIZE=${WRANGLER_LOG_SANITIZE:-}   # (read before the exports below overwrite it: pin_environment refuses a value of false)
# wrangler's own debug logger writes every message (whoami's account name, e-mail and full ids included) to a file under
# ~/.wrangler/logs or ./.wrangler/logs BEFORE any console filtering, and wrangler sends usage metrics: both are switched off
# for every call made from here, and the end of the script fails if a wrangler log file appeared all the same.
export WRANGLER_WRITE_LOGS=false WRANGLER_SEND_METRICS=false
# ... and, for every wrangler call: no error reports, the plain log level with sanitising on, no banner
export WRANGLER_SEND_ERROR_REPORTS=false WRANGLER_LOG=log WRANGLER_LOG_SANITIZE=true WRANGLER_HIDE_BANNER=true
PINNED_WRANGLER=4.156.0   # the only wrangler this script has been read against (its bundled wrangler-dist/cli.js)

CONFIG=wrangler.smg.toml
URL=""
CUSTOM_URL="https://galaxy.psvita.dev"   # the product address (attached in the dashboard, never by wrangler: the config has no route)
ACCOUNT_ID=""
EXPECT_SUB=""
PRINT_PLAN=0
SELF_CHECK=0
WHOAMI_IDS=""
WR_URLS=""
WR_HOSTS=""
WR_OUT=""
TOKEN_FILE=""
VERIFY_ONLY=0
NEW_TOKEN=0
SET_TOKEN=0
SKIP_TESTS=0
STATIC_ASSETS=0
TURN_KEY_FILE=""
TURN_USAGE_FILE=""
REQUIRE_RELAY=0
RELAY=unknown            # on | off | unknown (the plan cannot know: the secret names are read when the script really runs)
HAS_KEY_ID=0; HAS_KEY_TOK=0; HAS_USAGE=0; HAS_ACCT=0   # which secrets exist, by NAME
LOG_DIR=""
PUT_NAME=""; PUT_VALUE=""; SECRET_LIST=0   # --put-secret NAME (the value on stdin) and --secret-list: the only ways the runbook touches secrets
PROVED=""                                 # the addresses whose /v1/prove proof was just verified (an address gets the admin token only right after its own proof)
CUSTOM_WAIT_SEC=${DEPLOY_CUSTOM_WAIT_SEC:-600}   # a brand-new custom domain (DNS, certificate) can take minutes: "no answer" is retried this long (the offline test shortens it)
ADMIN_WAIT_SEC=${DEPLOY_ADMIN_WAIT_SEC:-120}   # (the offline test shortens it)

# Filter for everything wrangler (or a test run) prints before it reaches a file: e-mail addresses, account-name lines and account table rows
# (name | id) are removed. Reads stdin, writes stdout.
scrub() {
  # (LC_ALL=C and an alternation, not a bracket class: in a byte locale [|│] also matches any character sharing a byte with │,
  #  such as the ✘ that starts wrangler's error lines, and the error line was dropped whole.)
  # order matters: the exact secret values go first (several can be known: the admin token; the TURN key id, key API token and usage token from the files; the value
  # of --put-secret), then whole lines that carry account details (an account table row, an account-name line, wrangler's own "Membership roles in ..." line,
  # its "Credentials are stored in: <path>" line and its "`name`: `id`" account list), then home paths (/Users/<x>/ and /home/<x>/ become ~/), then every e-mail
  # address, then EVERY run of 32 or more hex digits (28 plus the last 4 that stay visible; the account id appears in wrangler's API error paths, e.g.
  # /accounts/<id>/workers/scripts/...: last 4 only), then any other token-shaped run of 40 or more of [A-Za-z0-9_-] (not lower: the 36-character dashed
  # version id must stay readable). Control characters (except newline and tab) are dropped last.
  # The exact values are replaced by an awk index()/substr() loop: a LITERAL search that behaves the same on macOS /bin/bash 3.2 and on bash 5. They reach awk
  # through its ENVIRONMENT (newline-separated in one variable), never argv, and are never exported.
  SCRUB_TOK="$(printf '%s\n%s\n%s\n%s\n%s' "${TOK:-}" "${TK_TOKEN:-}" "${TU_TOKEN:-}" "${TK_ID:-}" "${PUT_VALUE:-}")" LC_ALL=C awk '
    BEGIN { nt = split(ENVIRON["SCRUB_TOK"], tl, "\n") }
    { line = $0
      for (k = 1; k <= nt; k++) {
        t = tl[k]; n = length(t)
        if (n > 0) { out = ""; while ((i = index(line, t)) > 0) { out = out substr(line, 1, i - 1) "[TOKEN]"; line = substr(line, i + n) }; line = out line }
      }
      print line; fflush() }' |
  LC_ALL=C sed -E -e '/(\||│).*[0-9A-Fa-f]{32}/s/.*/[account table row removed]/' \
         -e '/[Aa][Cc][Cc][Oo][Uu][Nn][Tt] [Nn][Aa][Mm][Ee]/s/.*/[account name line removed]/' \
         -e '/Membership roles in/s/.*/[membership line removed]/' \
         -e '/Credentials are stored in/s/.*/[credentials path line removed]/' \
         -e '/^[[:space:]]*`[^`]*`: `[0-9A-Fa-f]{32}`/s/.*/[account list line removed]/' \
         -e 's#/(Users|home)/[^/ ]+/#~/#g' \
         -e 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}/[EMAIL]/g' \
         -e 's/([0-9]{1,3}\.){3}[0-9]{1,3}/[ip]/g' \
         -e 's/(^|[^0-9A-Za-z_:])[0-9A-Fa-f:]*::[0-9A-Fa-f:]*/\1[ip]/g' \
         -e 's/(^|[^0-9A-Za-z_:])([0-9A-Fa-f]{1,4}:){7}[0-9A-Fa-f]{1,4}/\1[ip]/g' \
         -e 's/[0-9A-Fa-f]{28,}([0-9A-Fa-f]{4})/[id ****\1]/g' \
         -e 's/[A-Za-z0-9_-]{40,}/[token]/g' |
  LC_ALL=C tr -d '\000-\010\013-\037\177'
}
# A kept file is only ever written from scrubbed text: $1 = the raw file (in the private temp dir RAWD), $2 = the kept file. Nothing raw is ever
# written under the log directory, so a kill at any moment cannot leave a raw file there. A missing source gives no kept file.
keep() { [ -e "$1" ] || return 0; scrub < "$1" > "$2"; }
TOK=""; TK_ID=""; TK_TOKEN=""; TU_TOKEN=""   # the admin token; the TURN key id, the key's API token and the usage token (only from their files)
die() { echo "deploy-smg-online: $*" | scrub >&2; exit 1; }
while [ $# -gt 0 ]; do
  case "$1" in
    --url) URL=${2:-}; shift 2 ;;
    --account-id) ACCOUNT_ID=${2:-}; shift 2 ;;
    --expect-subdomain) EXPECT_SUB=${2:-}; shift 2 ;;
    --print-plan) PRINT_PLAN=1; shift ;;
    --self-check) SELF_CHECK=1; shift ;;
    --admin-token-file) TOKEN_FILE=${2:-}; shift 2 ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    --new-admin-token) NEW_TOKEN=1; shift ;;
    --set-admin-token) SET_TOKEN=1; shift ;;
    --skip-tests) SKIP_TESTS=1; shift ;;
    --static-assets) STATIC_ASSETS=1; shift ;;
    --log-dir) LOG_DIR=${2:-}; shift 2 ;;
    --turn-key-file) TURN_KEY_FILE=${2:-}; shift 2 ;;
    --turn-usage-token-file) TURN_USAGE_FILE=${2:-}; shift 2 ;;
    --require-relay) REQUIRE_RELAY=1; shift ;;
    --put-secret) PUT_NAME=${2:-}; shift 2 ;;
    --secret-list) SECRET_LIST=1; shift ;;
    -h|--help) awk 'NR>1 { if ($0 ~ /^set -/) exit; print }' "$0"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

SECRET_MODE=0; { [ -n "$PUT_NAME" ] || [ $SECRET_LIST -eq 1 ]; } && SECRET_MODE=1

# Self-check: outside wr() no line may run wrangler. Quoted strings and trailing comments are cut from a line BEFORE it is tested (so a message cannot be
# mistaken for a call, and `echo x; npx --no wrangler ...` cannot hide behind an `echo`); what is left must not hold `npx [flags] wrangler` or a bare command word.
self_check() {
  awk 'BEGIN { p1 = "npx +((-y|--yes|--no) +)*wran" "gler"; p2 = "(^|[^A-Za-z0-9_./-])wran" "gler +[a-z]" }
       /^wr\(\) *\{/ { inwr = 1 }
       { line = $0
         gsub(/"[^"]*"/, "", line); gsub(/\047[^\047]*\047/, "", line); sub(/[ \t]#.*$/, "", line)
         if (!inwr && line !~ /^[ \t]*#/ && (line ~ p1 || line ~ p2)) { print FILENAME ":" NR ": " $0; bad = 1 } }
       /^\}/ { inwr = 0 }
       END { exit bad }' "$0"
}
if [ $SELF_CHECK -eq 1 ]; then
  self_check || die "wrangler is called outside the wr() function (lines above)"
  echo "self-check ok: every wrangler call goes through wr()"; exit 0
fi

cd "$(dirname "$0")/.."
[ -f "$CONFIG" ] || die "no $CONFIG here ($(pwd))"
# top-level keys only (before the first [table]): a `name` inside a table is not the Worker's name (the other game's config is wrangler.toml, Worker msc-online)
toml_top() { awk -v k="$1" '/^[[:space:]]*\[/ { exit } $0 ~ "^" k "[[:space:]]*=" { print; exit }' "$CONFIG"; }
CFG_NAME=$(toml_top name | sed -n 's/^name[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p')
[ "$CFG_NAME" = smg-online ] || die "$CONFIG does not name the worker smg-online (its top-level name is '${CFG_NAME:-none}'): refusing"
CFG_WDEV=$(toml_top workers_dev | sed -n 's/^workers_dev[[:space:]]*=[[:space:]]*\([A-Za-z0-9]*\).*/\1/p')
{ [ "$CFG_WDEV" = true ] || [ "$CFG_WDEV" = false ]; } || die "$CONFIG does not set workers_dev to true or false at its top level (found '${CFG_WDEV:-none}'): refusing"
CFG_PREV=$(toml_top preview_urls | sed -n 's/^preview_urls[[:space:]]*=[[:space:]]*\([A-Za-z0-9]*\).*/\1/p')
[ "$CFG_PREV" = false ] || die "$CONFIG does not set preview_urls = false at its top level (found '${CFG_PREV:-none}'): preview URLs would be a second public way to every uploaded version: refusing"
# a key inside one named table (the header line must match exactly; the value has its comment and spaces cut): the beta's Workers Logs and its D1 binding
toml_in() { awk -v h="$1" -v k="$2" '{ l = $0; sub(/[ \t]+(#.*)?$/, "", l) } l == h { on = 1; next } /^[[:space:]]*\[/ { on = 0 } on && $0 ~ "^" k "[[:space:]]*=" { print; exit }' "$CONFIG" | sed -E 's/^[^=]*=[[:space:]]*//; s/[[:space:]]*#.*$//; s/[[:space:]]+$//'; }
chk_in() { # $1 table header, $2 key, $3 required value (as written), $4 what it is for
  local v; v=$(toml_in "$1" "$2")
  [ "$v" = "$3" ] || die "$CONFIG: $1 must set $2 = $3 ($4); found '${v:-nothing}': refusing"
}
chk_in '[observability]' enabled true "Workers Logs carry the beta's problem reports and session summaries; reports.js writes them as console.log lines"
chk_in '[observability]' head_sampling_rate 1 "every report line must be kept, none sampled away"
chk_in '[observability.logs]' invocation_logs false "no per-request invocation log (wrangler 4.156.0: observability.logs.invocation_logs): only the report and summary lines are logged"
chk_in '[observability.traces]' enabled false "no tracing: only the report and summary lines are logged (wrangler 4.156.0: observability.traces.enabled)"
# wrangler PROVISIONS a binding that has no id (kv_namespaces, d1_databases, r2_buckets, queues, ai_search_namespaces, agent_memory, dispatch_namespaces, flagship: the
# HANDLERS table of wrangler-dist/cli.js) by CREATING it on the account, non-interactively too: the config may hold exactly one resource, the existing D1 database, with its id
d1n=$(grep -cE '^[[:space:]]*\[\[[[:space:]]*["'"'"']?d1_databases["'"'"']?[[:space:]]*\]\][[:space:]]*(#.*)?$' "$CONFIG" || true)
[ "$d1n" = 1 ] || die "$CONFIG must declare exactly ONE [[d1_databases]] (the existing REPORTS_DB database, with its database_id); found $d1n: refusing (a D1 binding without an id would be CREATED by the deploy)"
badres=$(grep -E '^[[:space:]]*\[{0,2}[[:space:]]*["'"'"']?(site|tail_consumers|triggers|env|kv_namespaces|r2_buckets|queues|vectorize|hyperdrive|workflows|ai_search_namespaces|agent_memory|dispatch_namespaces|flagship|pipelines|secrets_store_secrets|containers|services|analytics_engine_datasets|mtls_certificates|vpc_services|send_email|browser|ai|images|media|unsafe)["'"'"']?([][:space:].=]|$)' "$CONFIG" | sed -n '1p' | sed -E 's/^[[:space:]]*\[*[[:space:]]*["'"'"']?([a-z_0-9]+).*/\1/' || true)
[ -z "$badres" ] || die "$CONFIG declares '$badres': the only resource the config may hold is the one existing D1 database, and no [site], [[tail_consumers]], [env.*] or [triggers] (no cron, no other Worker's logs, no second environment; wrangler PROVISIONS bindings without an id by creating them on the account, and a service or container binding reaches beyond this Worker): refusing"
chk_in '[[d1_databases]]' binding '"REPORTS_DB"' "the D1 database that holds the beta's reports and summaries"
chk_in '[[d1_databases]]' database_name '"galaxy-beta-reports"' "the D1 database that holds the beta's reports and summaries"
chk_in '[[d1_databases]]' database_id '"272224ff-51e9-4ad0-8575-a5ab999e8358"' "the D1 database that holds the beta's reports and summaries"
[ -z "$(toml_top account_id)" ] || die "$CONFIG sets a top-level account_id: the account must come only from --account-id (wrangler would act on that one): refusing"
grep -q '^SERVICE_GAME = "smg"' "$CONFIG" || die "$CONFIG does not set SERVICE_GAME = \"smg\": refusing"
routeline=$(grep -vE '^[[:space:]]*#' "$CONFIG" | grep -E '^[[:space:]]*\[{0,2}[[:space:]]*["'"'"']?(routes|route)["'"'"']?([][:space:].=]|$)|custom_domain' | sed -n '1p' | sed -E 's/^[[:space:]]*\[*[[:space:]]*["'"'"']?([A-Za-z_]+).*/\1/' || true)
[ -z "$routeline" ] || die "$CONFIG declares a route or custom domain ('${routeline:-}'): the custom domain galaxy.psvita.dev is attached ONCE, by the owner, in the Cloudflare dashboard (Workers, smg-online, Settings, Domains and Routes), and wrangler must never publish routes or domains (its deploy would replace an existing DNS record or another Worker's binding without asking, and remove domains that are not in the config): refusing"
# the app versions the service must announce, from the config the package declares (no second copy here)
APP_MIN=$(sed -n 's/^MIN_APP_VERSION = "\([^"]*\)".*/\1/p' "$CONFIG" | sed -n '1p')
APP_LATEST=$(sed -n 's/^LATEST_APP_VERSION = "\([^"]*\)".*/\1/p' "$CONFIG" | sed -n '1p')
[[ "$APP_MIN" =~ ^[0-9]+(\.[0-9]+)*$ ]] && [[ "$APP_LATEST" =~ ^[0-9]+(\.[0-9]+)*$ ]] || die "$CONFIG sets no MIN_APP_VERSION / LATEST_APP_VERSION (the checks compare /v1/info against them): refusing"
URL=${URL:-$CUSTOM_URL}
[[ "$URL" =~ ^https://[A-Za-z0-9.-]+$ ]] || die "--url (https, no path) must be $CUSTOM_URL (the default) or https://smg-online.<subdomain>.workers.dev"
case "$URL" in *msc-online*) die "that is the Strikers service's address: refusing" ;; esac
[[ "$ACCOUNT_ID" =~ ^[0-9a-fA-F]{32}$ ]] || die "--account-id <32 hex characters> is required (the Cloudflare account the service belongs to; wrong format: refusing)"
ACCOUNT_ID=$(printf '%s' "$ACCOUNT_ID" | tr A-F a-f)
EXPECT_URL=""   # the workers.dev address, when known
if [ "$URL" != "$CUSTOM_URL" ]; then
  [[ "$URL" =~ ^https://smg-online\.([a-z0-9-]+)\.workers\.dev$ ]] || die "--url must be $CUSTOM_URL or https://smg-online.<subdomain>.workers.dev (not $URL)"
  [ -z "$EXPECT_SUB" ] && EXPECT_SUB=${BASH_REMATCH[1]}
fi
if [ -n "$EXPECT_SUB" ]; then
  [[ "$EXPECT_SUB" =~ ^[a-z0-9-]+$ ]] || die "--expect-subdomain must be lowercase letters, digits and dashes"
  EXPECT_URL="https://smg-online.$EXPECT_SUB.workers.dev"
  [ "$URL" = "$CUSTOM_URL" ] || [ "$URL" = "$EXPECT_URL" ] || die "--url ($URL) is not $EXPECT_URL: refusing"
fi
# the addresses every live check runs against: the custom domain always; the workers.dev address while the config keeps it on and it is known
BASES=("$CUSTOM_URL")
WORKERS_DEV_ON=0; [ "$CFG_WDEV" = true ] && WORKERS_DEV_ON=1
# the flag and the config must agree: workers.dev on needs --expect-subdomain (so that address is checked too); off must not have one
if [ $SECRET_MODE -eq 0 ] && [ $WORKERS_DEV_ON -eq 1 ] && [ -z "$EXPECT_URL" ]; then die "workers.dev is on ($CONFIG: workers_dev = true): pass --expect-subdomain <name> so that address is checked too"; fi
if [ $SECRET_MODE -eq 0 ] && [ $WORKERS_DEV_ON -eq 0 ] && [ -n "$EXPECT_URL" ]; then die "$CONFIG has workers_dev = false but a workers.dev address was given (--expect-subdomain, or --url): they contradict each other, refusing"; fi
if [ $WORKERS_DEV_ON -eq 1 ] && [ -n "$EXPECT_URL" ]; then BASES+=("$EXPECT_URL"); fi
export CLOUDFLARE_ACCOUNT_ID=$ACCOUNT_ID
if [ $SECRET_MODE -eq 1 ]; then
  # --put-secret / --secret-list stand alone: the account pin, the Worker's secret names, and (for a put) one secret on stdin; nothing is deployed or checked
  { [ -n "$PUT_NAME" ] && [ $SECRET_LIST -eq 1 ]; } && die "--put-secret and --secret-list exclude each other"
  { [ $VERIFY_ONLY -eq 1 ] || [ $NEW_TOKEN -eq 1 ] || [ $SET_TOKEN -eq 1 ] || [ $REQUIRE_RELAY -eq 1 ] || [ $STATIC_ASSETS -eq 1 ] || [ -n "$TURN_KEY_FILE" ] || [ -n "$TURN_USAGE_FILE" ] || [ -n "$TOKEN_FILE" ]; } && die "--put-secret / --secret-list take no other mode or file option (the value of a put comes on stdin)"
  case "$PUT_NAME" in ""|TURN_USAGE_TOKEN|TURN_ACCOUNT_ID|TURN_KEY_ID|TURN_KEY_API_TOKEN) ;; *) die "--put-secret allows only TURN_USAGE_TOKEN, TURN_ACCOUNT_ID, TURN_KEY_ID and TURN_KEY_API_TOKEN (not '$PUT_NAME')" ;; esac
else
[ -n "$TOKEN_FILE" ] || die "--admin-token-file is required (the file holds the admin token; it is never printed)"
[ $NEW_TOKEN -eq 1 ] && [ $SET_TOKEN -eq 1 ] && die "--new-admin-token and --set-admin-token exclude each other"
[ $VERIFY_ONLY -eq 1 ] && { [ $NEW_TOKEN -eq 1 ] || [ $SET_TOKEN -eq 1 ]; } && die "--verify-only changes nothing: no token options with it"
[ $VERIFY_ONLY -eq 1 ] && { [ -n "$TURN_KEY_FILE" ] || [ -n "$TURN_USAGE_FILE" ]; } && die "--verify-only sets no secret: no --turn-key-file / --turn-usage-token-file with it"
fi
command -v curl >/dev/null || die "curl is needed"
command -v npx >/dev/null || die "npx (Node >= 22.5) is needed"
command -v node >/dev/null || die "node is needed (the checks parse the JSON replies with it): install Node >= 22.5, then run again"
command -v openssl >/dev/null || die "openssl is needed (the proof before the admin token uses openssl rand): install it, then run again"

LOG_DIR=${LOG_DIR:-deploy-logs/$(date -u +%Y%m%dT%H%M%SZ)}
STEP=0
WR_LOG_DIRS=("${HOME:-/nonexistent}/.wrangler/logs" "./.wrangler/logs" "${HOME:-/nonexistent}/Library/Preferences/.wrangler/logs" "${XDG_CONFIG_HOME:-${HOME:-/nonexistent}/.config}/.wrangler/logs")
wr_log_files() { local d; for d in "${WR_LOG_DIRS[@]}"; do [ -d "$d" ] && find "$d" -type f 2>/dev/null; done | sort; return 0; }
WR_LOGS_BEFORE=""
note() { echo "[$(date -u +%H:%M:%S)] $*" | scrub | tee -a "$LOG_DIR/summary.txt"; }
last4() { local v=$1; echo "****${v: -4}"; }
# A secret file must be a regular file only its owner can read and write (mode 600; ls -l is the same on macOS and GNU). $1 = the file, $2 = what it is called in messages.
mode600_check() {
  local tmode
  [ -r "$1" ] || die "cannot read the $2"
  tmode=$(ls -ldL -- "$1" 2>/dev/null | cut -c1-10)
  { [ -f "$1" ] && [ "${tmode:4:6}" = "------" ]; } || die "the $2 must be a regular file that only its owner can read and write (mode 600); it is '${tmode:-unknown}': run chmod 600 on it. Refusing to use it"
}

# THE ONLY place wrangler is run (the self-check greps for any other). Always: `npx --no wrangler` (the local, pinned copy; never a download),
# CLOUDFLARE_ACCOUNT_ID (exported above) and, for EVERY call (whoami included: wrangler 4.156.0 accepts the global --config flag there),
# --config wrangler.smg.toml. $1 = a name for the log file, the rest = wrangler arguments. With PRINT_PLAN it only prints the command.
#  whoami (`whoami --json`) and deployments-status (`deployments status --json`): their output holds an e-mail address, so it is NEVER stored or printed: read in
#   memory only; the log holds OK or the reason. whoami: only accounts[].id is used (WHOAMI_IDS, matched EXACTLY); deployments-status: only the number of versions
#   of the current deployment (WR_DEPLOY_VERSIONS: none | N).
#  every other call streams through wr_filter (wrangler's own account block after "Getting User settings..." is dropped, then scrub()) into the kept file AS IT
#   RUNS (nothing is held in memory), and the workers.dev URLs, the psvita.dev names and the version id are read back from that SCRUBBED file (WR_URLS, WR_HOSTS,
#   WR_VERSION), so a kill in the middle of a deploy still leaves what wrangler had printed so far (a tool in the pipe may hold the last few lines).
wr_filter() {
  awk '{ if (skip) next; if (index($0, "Getting User settings...")) { print "[wrangler account block removed]"; skip = 1; next }; print; fflush() }' | scrub
}
wr() {
  local name=$1; shift
  local cfg=(--config "$CONFIG")
  if [ $PRINT_PLAN -eq 1 ]; then echo "   CLOUDFLARE_ACCOUNT_ID=$(last4 "$ACCOUNT_ID") npx --no wrangler $* ${cfg[*]-}"; return 0; fi
  STEP=$((STEP + 1))
  local out="$LOG_DIR/$(printf %02d $STEP)-wrangler-$name.txt" raw rc=0
  WR_URLS=""; WR_HOSTS=""; WR_VERSION=""
  if [ "$name" = whoami ]; then
    raw=$(npx --no wrangler "$@" "${cfg[@]}" </dev/null 2>/dev/null) || rc=$?
    if [ $rc -ne 0 ]; then raw=""; echo "FAILED: wrangler whoami exit code $rc (not logged in?)" > "$out"; note "wrangler whoami: FAILED ($out)"; return 1; fi
    WHOAMI_IDS=$(printf '%s' "$raw" | node -e 'let s = ""; process.stdin.on("data", (d) => { s += d; }).on("end", () => { try { for (const a of JSON.parse(s).accounts || []) if (a && typeof a.id === "string" && /^[0-9a-fA-F]{32}$/.test(a.id)) console.log(a.id.toLowerCase()); } catch { /* no ids */ } });' | sort -u || true)
    raw=""
    echo OK > "$out"; note "wrangler whoami: ok (output not stored)"; return 0
  fi
  if [ "$name" = deployments-status ]; then
    WR_DEPLOY_VERSIONS=unknown; WR_DEPLOY_SOURCE=unknown
    WR_OUT=$out   # the messages that say "see $WR_OUT" name this step's own kept file (never an earlier step's)
    # stdout alone is the JSON: wrangler's stderr (a "Proxy environment variables detected" warning, a deprecation or a Node warning) must never reach the parser;
    # stderr goes to a private temp file and is only searched for wrangler's own "no such Worker / no deployments" wording
    raw=$(npx --no wrangler "$@" "${cfg[@]}" </dev/null 2>"$RAWD/deployments.err") || rc=$?
    if [ $rc -ne 0 ]; then
      if { printf '%s\n' "$raw"; cat "$RAWD/deployments.err" 2>/dev/null; } | grep -E 'has no deployments|\[code: 10007\]|Worker "smg-online" not found|does not exist on your account' >/dev/null; then
        WR_DEPLOY_VERSIONS=none; WR_DEPLOY_SOURCE=none; raw=""; rm -f "$RAWD/deployments.err"; echo "OK: the Worker has no deployment yet" > "$out"; note "wrangler deployments-status: no deployment yet ($out)"; return 0
      fi
      raw=""; echo "FAILED: wrangler deployments status exit code $rc" > "$out"
      scrub < "$RAWD/deployments.err" 2>/dev/null | sed -n '1,5p' >> "$out" || true   # the reason, scrubbed, first 5 lines, in the kept file only
      rm -f "$RAWD/deployments.err"; note "wrangler deployments-status: FAILED ($out)"; return 1
    fi
    rm -f "$RAWD/deployments.err"
    # "<versions>|<source>": the number of versions of the current deployment and its `source` field (wrangler | dash | api | ...); nothing else leaves the parser
    ds=$(printf '%s' "$raw" | node -e 'let s = ""; process.stdin.on("data", (d) => { s += d; }).on("end", () => { try { const j = JSON.parse(s); const v = j.versions; const src = typeof j.source === "string" && /^[a-z_]{1,16}$/.test(j.source) ? j.source : "unknown"; console.log((Array.isArray(v) ? String(v.length) : "unknown") + "|" + src); } catch { console.log("unknown|unknown"); } });' 2>/dev/null || echo "unknown|unknown")
    WR_DEPLOY_VERSIONS=${ds%%|*}; WR_DEPLOY_SOURCE=${ds#*|}
    raw=""
    echo "OK: the current deployment has $WR_DEPLOY_VERSIONS version(s), source $WR_DEPLOY_SOURCE" > "$out"; note "wrangler deployments-status: ok, current deployment has $WR_DEPLOY_VERSIONS version(s), source: $WR_DEPLOY_SOURCE (output not stored)"; return 0
  fi
  echo "\$ npx --no wrangler $* ${cfg[*]}" > "$out"
  WR_OUT=$out
  case "$name" in
    secret-put*) if npx --no wrangler "$@" "${cfg[@]}" 2>&1 | wr_filter >> "$out"; then rc=0; else rc=${PIPESTATUS[0]}; fi ;;   # stdin: the secret value (the caller pipes it in)
    *) if npx --no wrangler "$@" "${cfg[@]}" </dev/null 2>&1 | wr_filter >> "$out"; then rc=0; else rc=${PIPESTATUS[0]}; fi ;;
  esac
  WR_URLS=$(grep -oE 'https://[A-Za-z0-9.-]+\.workers\.dev' "$out" | sort -u || true)
  WR_HOSTS=$(grep -oE '[A-Za-z0-9-]+\.psvita\.dev' "$out" | tr A-Z a-z | sort -u || true)
  WR_VERSION=$(grep -oE 'Current Version ID: [0-9a-fA-F-]{36}' "$out" | tail -1 | sed 's/^.*: //' || true)
  if [ $rc -eq 0 ]; then note "wrangler $name: ok ($out)${WR_VERSION:+ version id $WR_VERSION}"; return 0; fi
  note "wrangler $name: FAILED ($out)${WR_VERSION:+ version id $WR_VERSION}"; return 1
}

# Every workers.dev URL wrangler reported must be exactly $EXPECT_URL (else, with no subdomain known: smg-online.<x>.workers.dev), and wrangler must report NO psvita.dev
# name at all: it never publishes routes or domains here (the config has none), so a psvita.dev name in its output means something else is wrong. $1 = dry | deploy.
check_urls() {
  local u n=0 bad="" h
  for u in $WR_URLS; do
    n=$((n + 1))
    if [ -n "$EXPECT_URL" ]; then [ "$u" = "$EXPECT_URL" ] || bad="$bad $u"
    else [[ "$u" =~ ^https://smg-online\.[a-z0-9-]+\.workers\.dev$ ]] || bad="$bad $u"; fi
  done
  for h in $WR_HOSTS; do n=$((n + 1)); bad="$bad $h"; done
  if [ -n "$bad" ]; then
    if [ "$1" = dry ]; then die "the dry run reports$bad, not ${EXPECT_URL:-smg-online.<subdomain>.workers.dev} (and no psvita.dev name: wrangler never publishes routes or domains here): NOTHING WAS DEPLOYED"; fi
    note "!!! WRONG ADDRESS: the deploy reported$bad, expected ${EXPECT_URL:-smg-online.<subdomain>.workers.dev} and no psvita.dev name. The deploy has already happened. Stop; do not set secrets; tell the owner."
    die "wrong address after the deploy"
  fi
  if [ $n -eq 0 ]; then
    if [ "$1" = dry ]; then note "the dry run reported no address (it does not always); the addresses are checked on the real deploy"; return 0; fi
    note "the deploy reported no address (the custom domain is attached in the dashboard, not by this script; the live checks below ask each address)"; return 0
  fi
  note "wrangler reported only the expected address${WR_URLS:+ (workers.dev)} and no psvita.dev name"
}

# One HTTP request; its reply (status, headers, body) is kept under one step number with the request line. Sets CODE (000 = no answer)
# and LAST (the path of the kept files without extension). Not called in $(...): the step counter must survive.
# $1 name, $2 path, $3 "admin" to send the token: curl reads it as a config line on stdin (`-K -`), written by the printf builtin from the
# in-memory token: never on a command line or in any file. (scrub() hands the token to its own awk through that awk's environment only, never exported.) The kept request line shows [REDACTED].
BASE=$CUSTOM_URL
CURL_RC=0
req() {
  STEP=$((STEP + 1))
  local base="$LOG_DIR/$(printf %02d $STEP)-$1" raw="$RAWD/reply"
  # curl writes RAW text only into the private temp dir RAWD; the kept files are written from the scrubbed text only (keep)
  local method=${REQ_METHOD:-GET}
  CURL_RC=0   # curl's own exit code of this request (6 = the name does not resolve)
  local args=(-q -sS -o "$raw.body" -D "$raw.headers" -w '%{http_code}' --max-time 20 -H "User-Agent: ${REQ_UA:-smg-deploy-check}" -H 'X-Msc-Game: smg' -H 'X-Msc-App: 3.0.0')
  [ "$method" = GET ] || args+=(-X "$method" -H 'Content-Length: 0')   # (POST without a body)
  rm -f "$raw.body" "$raw.headers" "$raw.err"
  if [ "${3:-}" = admin ]; then
    echo "$method $2   Authorization: Bearer [REDACTED]" > "$base.request"
    CODE=$(printf 'header = "Authorization: Bearer %s"\n' "$TOK" | curl "${args[@]}" -K - "$BASE$2" 2> "$raw.err") || CURL_RC=$?
  else
    echo "$method $2" > "$base.request"
    CODE=$(curl "${args[@]}" "$BASE$2" 2> "$raw.err") || CURL_RC=$?
  fi
  [[ "$CODE" =~ ^[0-9]{3}$ ]] || CODE=000
  [ -z "${REQ_RAWCOPY:-}" ] || cp "$raw.body" "$RAWD/$REQ_RAWCOPY" 2>/dev/null || true   # (the proof check needs the unmasked body; it never leaves the private temp dir)
  echo "$CODE" > "$base.status"
  keep "$raw.body" "$base.body"; keep "$raw.headers" "$base.headers"; keep "$raw.err" "$base.curl-error"   # remote-derived text: scrubbed (token, ids, e-mail) on its way into the kept copy
  rm -f "$raw.body" "$raw.headers" "$raw.err"
  LAST=$base
}

# The live requests of one address, ONE line each: method|path|User-Agent|admin[|relay]. --print-plan prints exactly these; the offline test ties the printed plan to the
# requests a run really makes, so the two cannot drift apart. (The run's code below must do exactly this; the retries of a request are not listed again.)
# The relay self-check (custom domain only) is listed unless the relay is known to be off; the plan cannot know (the secret names are read when the script
# really runs), so it prints that line as 'check-if-relay-on:' (the 5th field says so).
check_specs() { # $1 = base address
  echo "GET|/v1/info|smg-deploy-check|no"
  if [ "$1" = "$CUSTOM_URL" ]; then echo "GET|/v1/info|GalaxyVita|no"; fi
  echo "GET|/times|smg-deploy-check|no"
  echo "GET|/v1/rank/get|smg-deploy-check|no"
  echo "GET|/v1/prove?n=<random-hex32>|smg-deploy-check|no|prove"
  echo "GET|/v1/admin/stats|smg-deploy-check|yes"
  if [ "$1" = "$CUSTOM_URL" ] && [ "$RELAY" != off ]; then echo "GET|/v1/prove?n=<random-hex32>|smg-deploy-check|no|prove-if-relay-on"; echo "POST|/v1/admin/turn-selfcheck|smg-deploy-check|yes|relay"; fi
}

if [ $PRINT_PLAN -eq 1 ] && [ $SECRET_MODE -eq 1 ]; then
  echo "PLAN (nothing is contacted, nothing is written, no value is read). Mode: $([ $SECRET_LIST -eq 1 ] && echo secret-list || echo "put-secret $PUT_NAME"), stand-alone: nothing is deployed and no live check is made. The account id is shown as its last 4 characters."
  echo "account pin: CLOUDFLARE_ACCOUNT_ID=$(last4 "$ACCOUNT_ID") exported for every wrangler call; the custom domain: $CUSTOM_URL"
  echo "1. login check (only the exit code and the account ids are used; the output is never stored or shown); refuse unless id $(last4 "$ACCOUNT_ID") is listed:"
  wr whoami whoami --json
  echo "2. the Worker's secrets by NAME (names only; it must exist, because 'secret put' would otherwise CREATE a stub Worker):"
  wr secret-list secret list
  if [ -n "$PUT_NAME" ]; then
    echo "3. set the secret $PUT_NAME; the value is read from stdin only (never an argument, never printed, scrubbed from every kept file) and goes to wrangler's stdin:"
    wr secret-put-manual secret put "$PUT_NAME"
  fi
  exit 0
fi
if [ $PRINT_PLAN -eq 1 ]; then
  [ -n "$TURN_KEY_FILE" ] && RELAY=on   # the key file makes the relay on for certain
  PTOK="[the token, read from the token file only when the script really runs]"   # the plan never opens the token file
  echo "PLAN (nothing is contacted, nothing is written, the token file is not opened). The account id is shown as its last 4 characters."
  echo "account pin: CLOUDFLARE_ACCOUNT_ID=$(last4 "$ACCOUNT_ID") exported for every wrangler call; the custom domain: $CUSTOM_URL; workers.dev address: ${EXPECT_URL:-not checked: workers_dev = false in the config} ($([ $WORKERS_DEV_ON -eq 1 ] && echo on || echo off))"
  echo "config (checked before anything runs): [observability] enabled = true, head_sampling_rate = 1, [observability.logs] invocation_logs = false, [observability.traces] enabled = false (Workers Logs carry the beta's report and summary lines only); exactly ONE [[d1_databases]] and no other resource (no kv, r2, queue, service, container, [site], [[tail_consumers]], [env.*] or [triggers], each of which wrangler would create or act on): REPORTS_DB = galaxy-beta-reports (database_id 272224ff-51e9-4ad0-8575-a5ab999e8358): the database must already exist on the account (the deploy fails, and nothing else is changed, if it does not)"
  echo "1. login check (only the exit code and the account ids are used; the output is never stored or shown); refuse unless id $(last4 "$ACCOUNT_ID") is listed:"
  wr whoami whoami --json
  echo "2. read the secret NAMES of the worker (names only; no value is ever shown or kept; the output is scrubbed) and decide about the relay:"
  wr secret-list secret list
  echo "   TURN_KEY_ID and TURN_KEY_API_TOKEN both present (by name, or from --turn-key-file): relay on, and the relay self-check below is REQUIRED (mint, revoke, usage ok when TURN_USAGE_TOKEN exists)."
  echo "   only one of the two: refused before the deploy (relay half set up). Neither: 'relay: off (no TURN key secrets)', the run goes on; with --require-relay it is refused."
  [ $REQUIRE_RELAY -eq 1 ] && echo "   --require-relay is given: relay off is a refusal."
  echo "3. current deployment (before the tests, also with --verify-only): stdout of 'deployments status --json' alone is parsed, in memory only, nothing is stored (a failure keeps its scrubbed reason, 5 lines at most, in the kept status file): its version count (a deploy needs ONE version at 100%, else refused) and its source field (api and dash: a WARNING only; the deploy's own --strict aborts by itself, with NOTHING WAS UPLOADED, if the live Worker has changes this package does not have):"
  wr deployments-status deployments status --json
  if [ $VERIFY_ONLY -eq 0 ]; then
    [ $SKIP_TESTS -eq 0 ] && echo "4. local suites: NODE_ENV=test npx --no vitest run --project core --project worker-smg   (contacts nothing)"
    echo "5. dry run (builds only: it uploads and publishes nothing); any workers.dev URL it reports must be ${EXPECT_URL:-smg-online.<subdomain>.workers.dev} and it must report no psvita.dev name at all (the config has no route and no custom domain: wrangler never publishes them), else stop before the deploy:"
    wr dry-run deploy --dry-run --outdir "<log-dir>/bundle"
    [ $STATIC_ASSETS -eq 1 ] && echo "   node scripts/build-assets.mjs public   (builds ./public)"
    echo "6. the deploy, with --strict (wrangler then ABORTS on any conflicting remote change instead of overwriting it); the addresses it reports must be as above and no psvita.dev name (the custom domain galaxy.psvita.dev was attached once in the Cloudflare dashboard; this deploy does not touch it), else stop at once:"
    wr deploy deploy --strict $([ $STATIC_ASSETS -eq 1 ] && echo --assets public)
    if [ $NEW_TOKEN -eq 1 ] || [ $SET_TOKEN -eq 1 ]; then
      [ $NEW_TOKEN -eq 1 ] && echo "   (--new-admin-token: the new token is made in memory, refused if the token file already exists; the file, mode 600, is written only right before the next step, after every check above)"
      echo "7. set the admin secret; the value ($PTOK) goes on stdin:"
      wr secret-put secret put ADMIN_TOKEN
    fi
    if [ -n "$TURN_KEY_FILE" ]; then
      echo "7b. (--turn-key-file) set the relay key; both values come from the file and go on stdin:"
      wr secret-put-turn-key-id secret put TURN_KEY_ID
      wr secret-put-turn-key-token secret put TURN_KEY_API_TOKEN
    fi
    if [ -n "$TURN_USAGE_FILE" ]; then
      echo "7c. (--turn-usage-token-file) set the usage token; the value comes from the file and goes on stdin:"
      wr secret-put-turn-usage secret put TURN_USAGE_TOKEN
    fi
    echo "7d. if TURN_USAGE_TOKEN exists (by name, or from --turn-usage-token-file): set TURN_ACCOUNT_ID = the account id above, on stdin, on every run (after every check that can refuse):"
    wr secret-put-turn-account secret put TURN_ACCOUNT_ID
  else
    echo "4-7. verify-only: no tests, no dry run, no deploy, no secret"
  fi
  echo "8. live checks, each reply kept (status, headers, body, all scrubbed) with its request; replies are read from these addresses only: ${BASES[*]}"
  echo "   These are exactly the requests the run makes, per address (a line 'check: <GET|POST> <url> User-Agent: <ua> admin-token: <yes|no>'; 'check-if-relay-on:' and 'prove-if-relay-on:' are made only when the relay is on; the admin token goes to curl as a config line on stdin, ${PTOK}):"
  for b in "${BASES[@]}"; do
    while IFS='|' read -r cmeth cpath cua cadm ccond; do
      pre="check:"; [ "$ccond" = relay ] && [ "$RELAY" = unknown ] && pre="check-if-relay-on:"
      [ "$ccond" = prove ] && pre="prove:"
      [ "$ccond" = prove-if-relay-on ] && { pre="prove-if-relay-on:"; [ "$RELAY" = unknown ] || pre="prove:"; }
      echo "   $pre $cmeth $b$cpath User-Agent: $cua admin-token: $cadm"
    done < <(check_specs "$b")
  done
  echo "   PROOF BEFORE THE TOKEN: before EVERY request that carries the admin token, to the same address, a fresh 32-hex nonce is made (openssl rand -hex 16) and 'GET /v1/prove?n=<nonce>' is asked without any token (the 'prove:' lines above). The proof it answers must equal HMAC-SHA256(key = the admin token, \"smg-online prove v1\" newline the address's host name, lower case, newline the nonce), worked out here by one helper process that gets the token only in its environment (never on a command line) and compares in constant time. Only an address whose proof matches gets the admin token (the line '<address> prove: ok (the service holds the admin token; host bound)'); a wrong proof, any other answer, 404 (no secret yet) or no answer = NO admin request to that address ('<address> prove: FAILED (...): the admin token is not sent there'). /v1/info is a public check only and never decides where the token goes."
  echo "   Retries: /v1/info on the custom domain is asked again every 10 s while there is NO answer at all (up to ${CUSTOM_WAIT_SEC} s); the proof and the admin stats check are asked again every 10 s while the proof is 404 or no answer, or the admin check is 404 or no answer (up to ${ADMIN_WAIT_SEC} s);"
  echo "   after any answer other than 200, 404 or none (000) on one address the admin check on the next address is not tried; the 'GalaxyVita' request is made only when the first /v1/info on the custom domain got any answer."
  exit 0
fi


mkdir -p "$LOG_DIR"
# Raw remote text (curl's body, headers and diagnostics) goes ONLY into this private (mode 700) temp dir, removed by the trap; only scrubbed text is written
# to the kept files under $LOG_DIR. (A kill that cannot run the trap, such as SIGKILL, leaves a mode-700 dir in the temp area, never a raw file in the logs.)
RAWD=$(mktemp -d "${TMPDIR:-/tmp}/smg-deploy-raw.XXXXXX") || die "cannot make a private temp directory"
chmod 700 "$RAWD"
trap 'rm -rf "$RAWD"' EXIT
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM

# ---------------------------------------------------------------- the environment is pinned BEFORE the first wrangler call
# wrangler reads many variables and files that change WHERE it talks to, WHICH account or credentials it uses, and where it writes: any of those set is a refusal
# (the name is shown, never the value). Proxy and CA variables are NOT refused (a sandbox may need them): only their names are noted.
pin_environment() {
  local v val names=""
  for v in CLOUDFLARE_API_BASE_URL WRANGLER_API_ENVIRONMENT WRANGLER_AUTH_DOMAIN WRANGLER_AUTH_URL WRANGLER_TOKEN_URL WRANGLER_REVOKE_URL WRANGLER_CLIENT_ID \
           CLOUDFLARE_API_TOKEN CLOUDFLARE_API_KEY CLOUDFLARE_EMAIL WRANGLER_CI_OVERRIDE_NAME CLOUDFLARE_ENV WRANGLER_OUTPUT_FILE_PATH WRANGLER_OUTPUT_FILE_DIRECTORY \
           WRANGLER_LOG_PATH NODE_OPTIONS MSC_HMAC_SOURCE \
           CF_API_TOKEN CF_API_KEY CF_EMAIL CF_API_BASE_URL CF_ACCOUNT_ID CLOUDFLARE_AUTH_USE_KEYRING CLOUDFLARE_COMPLIANCE_REGION WRANGLER_CF_AUTHORIZATION_TOKEN \
           CLOUDFLARE_ACCESS_CLIENT_ID CLOUDFLARE_ACCESS_CLIENT_SECRET WRANGLER_BUILD_CONDITIONS WRANGLER_BUILD_PLATFORM WRANGLER_CACHE_DIR WRANGLER_REGISTRY_PATH \
           WRANGLER_TRACE_ID WRANGLER_CI_MATCH_TAG \
           CLOUDFLARE_CLIENT_ID CLOUDFLARE_BASE_URL CLOUDFLARE_API_USER_SERVICE_KEY CLOUDFLARE_INCLUDE_PROCESS_ENV CLOUDFLARE_LOAD_DEV_VARS_FROM_DOT_ENV \
           CLOUDFLARE_CONTAINER_REGISTRY WRANGLER_CI_GENERATE_PREVIEW_ALIAS WORKERS_CI_BRANCH CF_PAGES_UPLOAD_JWT \
           WRANGLER_BASIN_SQL_AUTH_TOKEN; do
    val=${!v:-}
    # (20 Nov: a scoped API token instead of an interactive wrangler login, for a deploy from a session that cannot open a browser: allowed ONLY
    #  when DEPLOY_USE_API_TOKEN=1 names that choice explicitly; the account pin (whoami must list --account-id) still applies to the token.)
    if [ "$v" = CLOUDFLARE_API_TOKEN ] && [ "${DEPLOY_USE_API_TOKEN:-}" = 1 ] && [ -n "$val" ]; then names="$names CLOUDFLARE_API_TOKEN(allowed: DEPLOY_USE_API_TOKEN=1)"; continue; fi
    [ -z "$val" ] || die "the environment variable $v is set (its value is not shown): it changes what wrangler talks to, which credentials it uses, where it writes or what the local tests read. Refusing: unset $v and run again"
  done
  [ -z "$ORIG_LOG_DIR" ] || die "the environment variable LOG_DIR is set (its value is not shown): it moves where the kept logs go. Refusing: unset LOG_DIR and run again"
  case "$ORIG_LOG_SANITIZE" in false|FALSE|False|0) die "the environment variable WRANGLER_LOG_SANITIZE is set to false: wrangler would log unmasked ids. Refusing: unset it and run again" ;; esac
  [ "${NODE_TLS_REJECT_UNAUTHORIZED:-}" != 0 ] || die "the environment variable NODE_TLS_REJECT_UNAUTHORIZED is 0 (TLS checks off): refusing: unset it and run again"
  if [ "${DEPLOY_OFFLINE_TEST:-}" != 1 ]; then   # (only the offline test suite shortens the waits)
    [[ "${DEPLOY_CUSTOM_WAIT_SEC:-600}" =~ ^[0-9]+$ ]] && [ "${DEPLOY_CUSTOM_WAIT_SEC:-600}" -ge 600 ] || die "the environment variable DEPLOY_CUSTOM_WAIT_SEC is shorter than the default 600 s (or not a number): refusing: unset it and run again"
    [[ "${DEPLOY_ADMIN_WAIT_SEC:-120}" =~ ^[0-9]+$ ]] && [ "${DEPLOY_ADMIN_WAIT_SEC:-120}" -ge 120 ] || die "the environment variable DEPLOY_ADMIN_WAIT_SEC is shorter than the default 120 s (or not a number): refusing: unset it and run again"
  fi
  if [ -e .env ] || [ -e .env.local ]; then die "server/.env or server/.env.local exists: wrangler loads those files, and they can set any of the variables refused above. Refusing: move it away and run again"; fi
  WR_VER=$(sed -n 's/^  "version": "\([^"]*\)".*/\1/p' node_modules/wrangler/package.json 2>/dev/null | sed -n '1p' || true)
  [ "$WR_VER" = "$PINNED_WRANGLER" ] || die "node_modules/wrangler is version '${WR_VER:-missing}', not the pinned $PINNED_WRANGLER (this script was read against that one): run npm ci in server/ first. Nothing was run"
  for v in HTTPS_PROXY https_proxy HTTP_PROXY http_proxy ALL_PROXY all_proxy NO_PROXY no_proxy NODE_EXTRA_CA_CERTS SSL_CERT_FILE CURL_CA_BUNDLE XDG_CONFIG_HOME; do
    [ -z "${!v:-}" ] || names="$names $v"
  done
  if [ -n "$names" ]; then note "proxy / CA / config-home environment is set (names only, not refused; XDG_CONFIG_HOME decides where wrangler finds its login, the account pin below checks the login):$names"; fi
  return 0
}
pin_environment
# npx / npm must not phone home or print notices (they would break a JSON read and add a request): no update check, no fund or audit message
export npm_config_update_notifier=false npm_config_fund=false npm_config_audit=false npm_config_logs_max=0   # logs_max=0: npx writes no ~/.npm/_logs/*-debug-0.log per call
# (the names a `wrangler secret list` printed: used by --secret-list and by the relay decision)
secret_names_from_out() { grep -oE '"name"[[:space:]]*:[[:space:]]*"[A-Za-z0-9_]+"' "$WR_OUT" | sed -E 's/.*"([A-Za-z0-9_]+)"$/\1/' || true; }

# --put-secret: the value comes on stdin ONLY, is validated before anything is contacted, and from here on is scrubbed from every kept file and the console
if [ -n "$PUT_NAME" ]; then
  [ ! -t 0 ] || die "--put-secret $PUT_NAME: the value must come on stdin (pipe it in); it is never taken from an argument or typed"
  put2=""
  { IFS= read -r PUT_VALUE || true; IFS= read -r put2 || true; }
  [ -n "$PUT_VALUE" ] || die "--put-secret $PUT_NAME: the value on stdin is empty"
  [ -z "$put2" ] || die "--put-secret $PUT_NAME: the value on stdin holds more than one line (a secret value is one line)"
  case "$PUT_NAME" in
    TURN_ACCOUNT_ID) [ "$PUT_VALUE" = "$ACCOUNT_ID" ] || die "--put-secret TURN_ACCOUNT_ID: the value must equal --account-id exactly (the account id, 32 hex characters)" ;;
    TURN_KEY_ID) [[ "$PUT_VALUE" =~ ^[0-9a-fA-F]{32}$ ]] || die "--put-secret TURN_KEY_ID: the value must be the key id (32 hex characters)" ;;
    *) [[ "$PUT_VALUE" =~ ^[A-Za-z0-9._~+/=-]{20,}$ ]] || die "--put-secret $PUT_NAME: the value must be one token of at least 20 letters, digits and . _ ~ + / = -" ;;
  esac
fi

# ---------------------------------------------------------------- plan step 1. the login and the account pin (BEFORE anything is written, a new admin token file included)
if [ $SECRET_MODE -eq 1 ]; then
  if [ $SECRET_LIST -eq 1 ]; then RUN_MODE="secret-list"; else RUN_MODE="put-secret $PUT_NAME"; fi
  note "smg-online (custom domain $CUSTOM_URL); account $(last4 "$ACCOUNT_ID"); mode: $RUN_MODE; replies kept in $LOG_DIR"
else
  note "smg-online at ${BASES[*]}; account $(last4 "$ACCOUNT_ID"); mode: $([ $VERIFY_ONLY -eq 1 ] && echo verify-only || echo deploy); replies kept in $LOG_DIR"
fi
WR_LOGS_BEFORE=$(wr_log_files)
FAIL=0
# no wrangler debug log may have been written by this run (it would hold account details); run before the stand-alone secret modes exit, and at the end of a deploy
check_wr_logs() {
  WR_LOGS_AFTER=$(wr_log_files)
  if [ "$WR_LOGS_AFTER" != "$WR_LOGS_BEFORE" ]; then
    newlogs=$(comm -13 <(printf '%s\n' "$WR_LOGS_BEFORE") <(printf '%s\n' "$WR_LOGS_AFTER") | sed '/^$/d' | wc -l | tr -d ' ')
    note "wrangler wrote $newlogs debug log file(s) although WRANGLER_WRITE_LOGS=false: they may hold account details: look in ~/.wrangler/logs and ./.wrangler/logs and remove them"
    FAIL=1
  fi
}
wr whoami whoami --json || die "not logged in to Cloudflare (run the wrangler login first)"
if ! printf '%s\n' "$WHOAMI_IDS" | grep -x "$ACCOUNT_ID" >/dev/null; then
  echo "FAILED: the logged-in login does not list account id $(last4 "$ACCOUNT_ID")" > "$(ls "$LOG_DIR"/*-wrangler-whoami.txt | sed -n '1p')"
  die "wrangler whoami does not list account id $(last4 "$ACCOUNT_ID"): refusing to act on another account"
fi
note "account $(last4 "$ACCOUNT_ID") is listed by wrangler whoami"
# ---------------------------------------------------------------- --secret-list / --put-secret (stand-alone modes; nothing is deployed)
if [ $SECRET_MODE -eq 1 ]; then
  # The Worker must EXIST: wrangler's secret put would otherwise CREATE a stub Worker of that name (wrangler-dist/cli.js, the secret put command). `secret list` proves it.
  wr secret-list secret list || die "the Worker smg-online does not exist or its secrets cannot be read (see $WR_OUT): refusing; --put-secret would CREATE a stub Worker. Deploy first"
  if [ $SECRET_LIST -eq 1 ]; then
    note "secret list (names only): $(secret_names_from_out | tr '\n' ' ')"
    check_wr_logs; exit $FAIL
  fi
  if printf '%s' "$PUT_VALUE" | wr secret-put-manual secret put "$PUT_NAME"; then note "secret put: $PUT_NAME ok (value not printed)"; check_wr_logs; exit $FAIL; fi
  note "secret put: $PUT_NAME FAILED (value not printed)"; check_wr_logs; exit 1
fi
# ---------------------------------------------------------------- the admin token (only after the account pin held)
# A NEW token is made in memory only. Its file is written at the last moment, right before the secret is set, i.e. after every check that can refuse
# (the tests, the dry run and its address check, the build, the deploy and its address check): a refused run leaves no token file behind.
if [ $NEW_TOKEN -eq 1 ]; then
  { [ -e "$TOKEN_FILE" ] || [ -L "$TOKEN_FILE" ]; } && die "the admin token file exists: refusing to overwrite an admin token (move it away first)"
  command -v openssl >/dev/null || die "openssl is needed to make a token"
  TOK=$(openssl rand -base64 48 | tr '+/' '-_' | tr -d '=\n')
else
  # the file is documented as mode 600: refuse a token file that group or others can read or write
  mode600_check "$TOKEN_FILE" "admin token file"
  IFS= read -r TOK < "$TOKEN_FILE" || true
fi
[ ${#TOK} -ge 32 ] || die "the token in the admin token file is shorter than 32 characters"
[[ "$TOK" =~ ^[A-Za-z0-9._~+/=-]+$ ]] || die "the token in the admin token file holds characters other than letters, digits and . _ ~ + / = -"

# The TURN files (the second way in; normally the owner sets these secrets in the dashboard): same mode-600 rule, read into memory only. Nothing of their
# content is ever printed (the messages below name the line, never the value); the values are scrubbed from every kept file from here on (scrub()).
if [ -n "$TURN_KEY_FILE" ]; then
  mode600_check "$TURN_KEY_FILE" "TURN key file"
  tk3=""
  { IFS= read -r TK_ID || true; IFS= read -r TK_TOKEN || true; IFS= read -r tk3 || true; } < "$TURN_KEY_FILE"
  [[ "$TK_ID" =~ ^[0-9a-fA-F]{32}$ ]] || { TK_ID=""; TK_TOKEN=""; die "line 1 of the TURN key file must be the TURN key id (32 hex characters)"; }
  TK_ID=$(printf '%s' "$TK_ID" | tr A-F a-f)
  [[ "$TK_TOKEN" =~ ^[A-Za-z0-9._~+/=-]{20,}$ ]] || { TK_TOKEN=""; die "line 2 of the TURN key file must be the key's API token (letters, digits and . _ ~ + / = -, at least 20 characters)"; }
  [ -z "$tk3" ] || die "the TURN key file must hold exactly two lines: the key id, then the key's API token"
fi
if [ -n "$TURN_USAGE_FILE" ]; then
  mode600_check "$TURN_USAGE_FILE" "TURN usage token file"
  tu2=""
  { IFS= read -r TU_TOKEN || true; IFS= read -r tu2 || true; } < "$TURN_USAGE_FILE"
  [[ "$TU_TOKEN" =~ ^[A-Za-z0-9._~+/=-]{20,}$ ]] || { TU_TOKEN=""; die "the TURN usage token file must hold one line: the analytics token (letters, digits and . _ ~ + / = -, at least 20 characters)"; }
  [ -z "$tu2" ] || die "the TURN usage token file must hold exactly one line"
fi

# ---------------------------------------------------------------- plan step 2. the secret NAMES and the relay decision (before anything is tested, built or deployed)
# `wrangler secret list` prints names and types, no value. Its output is scrubbed into the log like every wrangler output; only the names are read from it.
read_secret_names() {
  local names
  if ! wr secret-list secret list; then
    # a worker that does not exist yet has no secrets (a first deploy): not an error in a deploying run; any other failure is
    # (only wrangler's own wording for a missing Worker counts: the code 10007 or `Worker "smg-online" not found`, not any "not found")
    if [ $VERIFY_ONLY -eq 0 ] && grep -qE '\[code: 10007\]|Worker "smg-online" not found' "$WR_OUT" 2>/dev/null; then
      note "secret names: the worker has no secrets yet (wrangler secret list says it was not found): treated as none"
      return 0
    fi
    die "cannot read the secret names (wrangler secret list failed; see $WR_OUT): refusing"
  fi
  names=$(secret_names_from_out)
  if printf '%s\n' "$names" | grep -x TURN_KEY_ID >/dev/null; then HAS_KEY_ID=1; fi
  if printf '%s\n' "$names" | grep -x TURN_KEY_API_TOKEN >/dev/null; then HAS_KEY_TOK=1; fi
  if printf '%s\n' "$names" | grep -x TURN_USAGE_TOKEN >/dev/null; then HAS_USAGE=1; fi
  if printf '%s\n' "$names" | grep -x TURN_ACCOUNT_ID >/dev/null; then HAS_ACCT=1; fi
  note "secrets by name: TURN_KEY_ID $([ $HAS_KEY_ID -eq 1 ] && echo present || echo absent), TURN_KEY_API_TOKEN $([ $HAS_KEY_TOK -eq 1 ] && echo present || echo absent), TURN_USAGE_TOKEN $([ $HAS_USAGE -eq 1 ] && echo present || echo absent), TURN_ACCOUNT_ID $([ $HAS_ACCT -eq 1 ] && echo present || echo absent)"
  return 0
}
relay_decide() {
  local id=$HAS_KEY_ID tok=$HAS_KEY_TOK src="as secrets"
  if [ -n "$TURN_KEY_FILE" ]; then id=1; tok=1; src="from --turn-key-file"; fi
  if [ $id -eq 1 ] && [ $tok -eq 0 ]; then die "relay half set up: TURN_KEY_ID present, TURN_KEY_API_TOKEN missing (set both secrets, or pass --turn-key-file): refusing before the deploy"; fi
  if [ $id -eq 0 ] && [ $tok -eq 1 ]; then die "relay half set up: TURN_KEY_API_TOKEN present, TURN_KEY_ID missing (set both secrets, or pass --turn-key-file): refusing before the deploy"; fi
  if [ $id -eq 1 ]; then
    RELAY=on
    note "relay: on (TURN_KEY_ID and TURN_KEY_API_TOKEN present $src): the relay self-check is required"
  else
    RELAY=off
    note "relay: off (no TURN key secrets)"
    [ $REQUIRE_RELAY -eq 0 ] || die "relay: off (no TURN key secrets) and --require-relay is given: refusing before the deploy (set TURN_KEY_ID and TURN_KEY_API_TOKEN as secrets of smg-online, or pass --turn-key-file)"
    [ -z "$TURN_USAGE_FILE" ] || die "--turn-usage-token-file without any TURN key secrets: the usage token belongs to a relay that is on (set the key secrets or pass --turn-key-file)"
  fi
  return 0
}
read_secret_names
relay_decide

# ---------------------------------------------------------------- plan step 3. the current deployment (before the tests; deploy AND --verify-only read it)
# wrangler 4.156.0 `deploy --strict` ABORTS when the Worker was last deployed from the script API, or from the dashboard with a destructive config difference
# (wrangler-dist/cli.js ~181700-181752); the `source` field of `deployments status --json` is the closest thing readable without an API token. A Worker split across
# versions (a gradual rollout) would be replaced by one version at 100%.
if [ $VERIFY_ONLY -eq 1 ]; then
  wr deployments-status deployments status --json || note "the current deployment of smg-online could not be read (see $WR_OUT): --verify-only goes on"
else
  wr deployments-status deployments status --json || die "cannot read the current deployment of smg-online (see $WR_OUT): refusing before the deploy"
fi
case "$WR_DEPLOY_SOURCE" in
  api) note "WARNING: the current deployment of smg-online was made through the script API (source: api; only a hint, wrangler's own record is last_deployed_from): wrangler asks before overwriting an API-edited Worker and --strict aborts instead of asking, so the deploy may abort (then NOTHING WAS UPLOADED); the script goes on" ;;
  dash) note "WARNING: the current deployment of smg-online was made from the Cloudflare dashboard (source: dash): if the dashboard configuration differs from wrangler.smg.toml in a destructive way, the deploy aborts under --strict; the owner decides" ;;
esac
if [ $VERIFY_ONLY -eq 0 ]; then
  case "$WR_DEPLOY_VERSIONS" in
    none|1) ;;
    *) die "the current deployment of smg-online is split or cannot be read (versions: $WR_DEPLOY_VERSIONS): a deploy would replace a gradual rollout. Refusing: the owner finishes or ends the rollout (dashboard, Workers, smg-online, Deployments) and runs again. NOTHING WAS DEPLOYED" ;;
  esac
fi

# ---------------------------------------------------------------- plan step 4. the local suites
if [ $SKIP_TESTS -eq 0 ] && [ $VERIFY_ONLY -eq 0 ]; then
  # (the exit code is taken in THIS shell: an assignment on the left of a pipe runs in a subshell and is lost)
  trc=0; traw=$(NODE_ENV=test npx --no vitest run --project core --project worker-smg 2>&1) || trc=$?
  printf '%s\n' "$traw" | scrub > "$LOG_DIR/00-tests.txt"; traw=""
  if [ $trc -eq 0 ]; then note "tests: ok ($(grep -E 'Tests ' "$LOG_DIR/00-tests.txt" | tail -1 | tr -s ' '))"
  else note "tests: FAILED ($LOG_DIR/00-tests.txt)"; die "the test suites failed: nothing deployed"; fi
fi

# (plan step 5 dry run, step 6 deploy and step 7 secrets follow)
if [ $VERIFY_ONLY -eq 0 ]; then
  wr dry-run deploy --dry-run --outdir "$LOG_DIR/bundle" || die "the bundle does not build: nothing deployed"
  check_urls dry
  ASSETS=()
  if [ $STATIC_ASSETS -eq 1 ]; then
    arc=0
    araw=$(node scripts/build-assets.mjs public 2>&1) || arc=$?
    printf '%s\n' "$araw" | scrub > "$LOG_DIR/00-assets.txt"; araw=""
    [ $arc -eq 0 ] || die "scripts/build-assets.mjs failed (see $LOG_DIR/00-assets.txt): nothing deployed"
    ASSETS=(--assets public)
    note "static assets built into ./public (the /times page)"
  fi
  if ! wr deploy deploy --strict ${ASSETS[@]+"${ASSETS[@]}"}; then
    dlog=$WR_OUT
    if grep -q 'Aborting the upload operation because of conflicts' "$dlog"; then
      echo "--- wrangler refused under --strict (scrubbed, from its first [ERROR]):" >&2
      sed -n '/\[ERROR\]/,$p' "$dlog" | sed -n '1,20p' | scrub >&2
      die "wrangler deploy ABORTED under --strict: the live Worker has changes this package does not have (a dashboard edit or another deploy). NOTHING WAS UPLOADED. STOP: report this message; the owner decides; do not remove --strict."
    fi
    if grep -q '\[ERROR\]' "$dlog"; then
      echo "--- wrangler's error (scrubbed, from its first [ERROR]; $dlog):" >&2
      sed -n '/\[ERROR\]/,$p' "$dlog" | sed -n '1,30p' | scrub >&2
    else
      echo "--- last 20 lines of the scrubbed wrangler output ($dlog):" >&2
      tail -n 20 "$dlog" | scrub >&2
    fi
    die "wrangler deploy FAILED, and it may have failed part-way (code, custom domain and secrets are separate steps): the live service can be in a changed state. STOP: report this message and run --verify-only to see what is live. Do not deploy again without the owner."
  fi
  check_urls deploy
  if [ $NEW_TOKEN -eq 1 ]; then
    # (noclobber: never over an existing file; umask 077 from the top of the script: mode 600)
    ( set -C; printf '%s\n' "$TOK" > "$TOKEN_FILE" ) 2>/dev/null || die "cannot create the admin token file (it appeared meanwhile, or its folder is not writable): the secret was NOT set"
    chmod 600 "$TOKEN_FILE"
    note "a new admin token was written to the admin token file (mode 600; keep it in the password manager; it was not printed)"
  fi
  if [ $NEW_TOKEN -eq 1 ] || [ $SET_TOKEN -eq 1 ]; then
    printf '%s' "$TOK" | wr secret-put secret put ADMIN_TOKEN || die "secret put failed"
  fi
  # the relay secrets: after the deploy and after every check that can refuse; the values go to wrangler on stdin (never an argument), nothing is printed
  if [ -n "$TURN_KEY_FILE" ]; then
    printf '%s' "$TK_ID" | wr secret-put-turn-key-id secret put TURN_KEY_ID || die "secret put TURN_KEY_ID failed"
    note "secret set: TURN_KEY_ID (from --turn-key-file; value not printed)"
    printf '%s' "$TK_TOKEN" | wr secret-put-turn-key-token secret put TURN_KEY_API_TOKEN || die "secret put TURN_KEY_API_TOKEN failed"
    note "secret set: TURN_KEY_API_TOKEN (from --turn-key-file; value not printed)"
  fi
  if [ -n "$TURN_USAGE_FILE" ]; then
    printf '%s' "$TU_TOKEN" | wr secret-put-turn-usage secret put TURN_USAGE_TOKEN || die "secret put TURN_USAGE_TOKEN failed"
    note "secret set: TURN_USAGE_TOKEN (from --turn-usage-token-file; value not printed)"
  fi
  if [ $HAS_USAGE -eq 1 ] || [ -n "$TURN_USAGE_FILE" ]; then
    # (idempotent: the same value every run; the Worker reads usage for this account)
    printf '%s' "$ACCOUNT_ID" | wr secret-put-turn-account secret put TURN_ACCOUNT_ID || die "secret put TURN_ACCOUNT_ID failed"
    note "secret set: TURN_ACCOUNT_ID (the account id from --account-id; every run; value not printed)"
    HAS_ACCT=1
  fi
fi

# ---------------------------------------------------------------- plan step 8. checks against the live service
FAIL=0
for BASE in "${BASES[@]}"; do
  tag=$([ "$BASE" = "$CUSTOM_URL" ] && echo custom || echo workersdev)
  req "$tag-info" /v1/info
  if [ "$tag" = custom ] && [ "$CODE" = 000 ]; then
    # first deploy: the custom domain may not resolve or have its certificate yet (a zone registered today: minutes). Retry ONLY while there is no answer at all
    # (DNS / TLS failure); any answer, right or wrong, ends the wait.
    cstart=$(date +%s); ctries=1
    note "$BASE: no answer yet (a freshly attached custom domain needs DNS and a certificate, which can take minutes); asking again every 10 s for up to ${CUSTOM_WAIT_SEC} s"
    while [ "$CODE" = 000 ] && [ $(( $(date +%s) - cstart )) -lt "$CUSTOM_WAIT_SEC" ]; do
      sleep 10; ctries=$((ctries + 1)); req "$tag-info-try$ctries" /v1/info
    done
    note "$BASE: ${ctries} tries for /v1/info, last answer $CODE"
  fi
  # (parsed, not grepped: the answer must say game smg and app min AND latest exactly as the config of this package declares them)
  GOT=$(node -e 'try{const j=JSON.parse(require("fs").readFileSync(process.argv[1],"utf8"));const a=j.app||{};console.log([j.game,a.min,a.latest].map(x=>typeof x==="string"?x:"?").join(" "))}catch{console.log("? ? ?")}' "$LAST.body" 2>/dev/null || echo "? ? ?")
  if [ "$CODE" = 200 ] && [ "$GOT" = "smg $APP_MIN $APP_LATEST" ]; then note "$BASE GET /v1/info: 200, game smg, app min $APP_MIN latest $APP_LATEST (a public check only: it never decides where the admin token goes)"
  elif [ "$CODE" = 200 ]; then note "$BASE GET /v1/info: 200 but the answer says (game min latest) '$GOT', expected 'smg $APP_MIN $APP_LATEST' (the config's MIN_APP_VERSION / LATEST_APP_VERSION; see $LAST.*)"; FAIL=1
  else note "$BASE GET /v1/info: $CODE (expected 200 with game smg and app $APP_MIN / $APP_LATEST; see $LAST.*)"; FAIL=1; fi
  if [ "$tag" = custom ] && [ "$CODE" != 000 ]; then
    # the game's own User-Agent must get through the zone too (Browser Integrity Check applies on a zone, never on workers.dev)
    REQ_UA=GalaxyVita req "$tag-info-gamescan" /v1/info
    if [ "$CODE" = 200 ]; then note "$BASE GET /v1/info with User-Agent GalaxyVita: 200"
    else note "$BASE GET /v1/info with the game's User-Agent GalaxyVita: $CODE (expected 200; a 403 from Cloudflare means a zone rule such as Browser Integrity Check refuses the game; see $LAST.*)"; FAIL=1; fi
  fi
  req "$tag-times-page" /times
  if [ "$CODE" = 200 ]; then note "$BASE GET /times: 200"; else note "$BASE GET /times: $CODE (see $LAST.*)"; FAIL=1; fi
  req "$tag-no-ranking" /v1/rank/get
  # (404 only: a 405 would mean the route EXISTS for another method, which is what the Strikers service would answer)
  if [ "$CODE" = 404 ]; then note "$BASE Strikers-only route absent: 404"; else note "$BASE GET /v1/rank/get: $CODE (expected 404; see $LAST.*)"; FAIL=1; fi
done
# PROOF BEFORE THE TOKEN. GET /v1/prove?n=<nonce> answers {"proof": HMAC-SHA256(key = the admin token, "smg-online prove v1\n" + the host name + "\n" + nonce)} and only the service that
# holds the admin secret can make it. Before EVERY request that carries the admin token, to the address it goes to, a fresh nonce is made, the proof is asked and checked here; the
# expected value is worked out inside one node process that gets the token in its ENVIRONMENT only (never an argument) and prints only ok or bad (so the expected proof is in no variable,
# no file and no log); the comparison is constant time. Sets PROVE_V = ok | wait (404 or no answer: the secret is not visible there yet) | fail, PROVE_CODE and LAST.
prove_base() { # $1 tag, $2 try
  local nonce host res
  nonce=$(openssl rand -hex 16 2>/dev/null || true)
  [[ "$nonce" =~ ^[0-9a-f]{32}$ ]] || die "openssl rand -hex 16 did not give 32 hex digits: the proof before the admin token needs openssl. Refusing: install openssl and run again"
  host=$(printf '%s' "${BASE#https://}" | tr 'A-Z' 'a-z')
  rm -f "$RAWD/prove.body"
  REQ_RAWCOPY=prove.body req "$1-prove-try$2" "/v1/prove?n=$nonce"
  PROVE_CODE=$CODE
  PROVE_V=fail
  if [ "$CODE" = 200 ]; then
    res=$(PROVE_KEY=$TOK node -e 'const c = require("crypto"); let r = "bad"; try {
      const j = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
      const okShape = typeof j.proof === "string" && /^[0-9a-f]{64}$/.test(j.proof);
      const want = c.createHmac("sha256", process.env.PROVE_KEY).update("smg-online prove v1\n" + process.argv[2] + "\n" + process.argv[3]).digest();
      const got = okShape ? Buffer.from(j.proof, "hex") : Buffer.alloc(32);
      if (c.timingSafeEqual(want, got) && okShape) r = "ok"; } catch { r = "bad"; } console.log(r);' "$RAWD/prove.body" "$host" "$nonce" 2>/dev/null || echo bad)
    [ "$res" = ok ] && PROVE_V=ok
  elif [ "$CODE" = 404 ] || [ "$CODE" = 000 ]; then PROVE_V=wait; fi
  rm -f "$RAWD/prove.body"
  if [ "$PROVE_V" = ok ]; then note "$BASE prove: ok (the service holds the admin token; host bound)"; fi
}
prove_failed() { # $1 what: the reason, in words
  note "$BASE prove: FAILED ($1; see $LAST.*): the admin token is not sent there"
  FAIL=1
}
# The admin check, on EVERY address checked above (the custom domain first, then workers.dev while it is on), each only after its own proof: a fresh secret takes a while to reach
# every Cloudflare location. 404 or no answer at all (the proof or the admin route: the Worker has no ADMIN_TOKEN yet) = not yet: ask again every 10 s for up
# to ADMIN_WAIT_SEC. Any other answer ends the wait at once: 200 is success; 401 means the token in the file is not the one the service has;
# 429 means the admin brake (wrong tokens from this address). After any answer other than 200, 404 or none on one address the next one is not tried (it would only add
# a failure to the brake for the same token). A proof that does not match (or any answer to /v1/prove other than 200, 404 or none) ends this address at once with NO admin request.
admin_check() { # $1 tag
  local start tries=0 code last
  start=$(date +%s)
  ADMIN_STOP=0; ADMIN_OK=0
  while :; do
    tries=$((tries + 1))
    prove_base "$1" "$tries"
    if [ "$PROVE_V" = fail ]; then
      if [ "$PROVE_CODE" = 200 ]; then prove_failed "HTTP 200 but the proof does not match: this address is not bound to the service that holds the admin token"
      else prove_failed "HTTP $PROVE_CODE: not the answer of the Galaxy service"; fi
      return 0
    fi
    if [ "$PROVE_V" = ok ]; then
      req "$1-admin-stats-try$tries" /v1/admin/stats admin
      code=$CODE
      last=$LAST
      if [ "$code" = 200 ]; then ADMIN_OK=1; note "$BASE admin check: 200 after $tries tries ($last.body)"; return 0; fi
      if [ "$code" != 404 ] && [ "$code" != 000 ]; then
        note "$BASE admin check: $code on try $tries: stopped (not a propagation delay; see $last.*)"
        FAIL=1; ADMIN_STOP=1
        return 0
      fi
    else
      code=$PROVE_CODE; last=$LAST
    fi
    if [ $(( $(date +%s) - start )) -ge $ADMIN_WAIT_SEC ]; then
      if [ "$PROVE_V" = ok ]; then note "$BASE admin check: still $code after ${ADMIN_WAIT_SEC} s ($tries tries): the secret is not visible to the Worker (see $last.*)"; FAIL=1
      else prove_failed "still HTTP $code after ${ADMIN_WAIT_SEC} s, $tries tries: the service has no admin token yet, or this address is not the service"; fi
      return 0
    fi
    sleep 10
  done
}
CUSTOM_ADMIN_OK=0
for BASE in "${BASES[@]}"; do
  tag=$([ "$BASE" = "$CUSTOM_URL" ] && echo custom || echo workersdev)
  admin_check "$tag"
  [ "$tag" = custom ] && CUSTOM_ADMIN_OK=$ADMIN_OK
  if [ $ADMIN_STOP -eq 1 ]; then note "the admin check on the remaining addresses was not tried (same token)"; break; fi
done
BASE=$CUSTOM_URL

# ---------------------------------------------------------------- the relay self-check (REQUIRED whenever the relay is on)
# POST /v1/admin/turn-selfcheck on the custom domain: the Worker mints a 300 s pass (identifier "selfcheck"), revokes it at once and reads the usage numbers;
# the answer never holds the credential. Pass: HTTP 200, mint ok, revoke ok, and usage ok when TURN_USAGE_TOKEN exists (else ok or absent), not tripped.
# "Not yet" (asked again every 10 s for up to ADMIN_WAIT_SEC): 404 or no answer (the route or the secrets are not visible yet), mint off (the key secrets are not
# visible to the Worker yet), usage absent while the usage token exists. Anything else ends the wait at once, with the reason the service gave.
relay_selfcheck() {
  local start tries=0 code last want_usage=0 st verdict sc_mint sc_revoke sc_usage sc_gm sc_am sc_trip sc_tag sc_detail mint_txt
  if [ $HAS_USAGE -eq 1 ] || [ -n "$TURN_USAGE_FILE" ]; then want_usage=1; fi
  if [ $want_usage -eq 1 ] && [ $HAS_ACCT -eq 0 ]; then
    note "relay self-check: FAIL: TURN_USAGE_TOKEN exists but TURN_ACCOUNT_ID does not (a normal run, without --verify-only, sets it from --account-id)"
    FAIL=1; return 0
  fi
  if [ "$CUSTOM_ADMIN_OK" != 1 ]; then
    note "relay self-check: FAIL: not run, because the admin check on $CUSTOM_URL did not pass (the self-check is an admin call)"
    FAIL=1; return 0
  fi
  start=$(date +%s)
  BASE=$CUSTOM_URL
  while :; do
    tries=$((tries + 1))
    prove_base custom-selfcheck "$tries"
    if [ "$PROVE_V" = fail ]; then prove_failed "HTTP $PROVE_CODE, or a proof that does not match: relay self-check not run"; note "relay: on, hard stop NOT confirmed"; return 0; fi
    if [ "$PROVE_V" = wait ]; then
      if [ $(( $(date +%s) - start )) -ge $ADMIN_WAIT_SEC ]; then prove_failed "still HTTP $PROVE_CODE after ${ADMIN_WAIT_SEC} s, $tries tries"; note "relay: on, hard stop NOT confirmed"; return 0; fi
      sleep 10; continue
    fi
    REQ_METHOD=POST req "custom-turn-selfcheck-try$tries" /v1/admin/turn-selfcheck admin
    code=$CODE; last=$LAST
    sc_mint="?"; sc_revoke="?"; sc_usage="?"; sc_gm="?"; sc_am="?"; sc_trip="?"; sc_tag="tagged"; sc_detail=""
    # (parsed, not grepped; never anything but the fields below is read from the answer)
    { IFS=' ' read -r sc_mint sc_revoke sc_usage sc_gm sc_am sc_trip sc_tag || true; IFS= read -r sc_detail || true; } < <(node -e '
      try { const j = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8")); const f = (x) => (typeof x === "string" && /^[a-z]+$/.test(x) ? x : "?");
        const n = (x) => (typeof x === "number" ? String(Math.round(x)) : "none");
        console.log([f(j.mint), f(j.revoke), f(j.usage), n(j.galaxyMonthMb), n(j.accountMonthMb), j.tripped === true ? "yes" : "no", j.untagged === true ? "untagged" : "tagged"].join(" "));
        console.log([j.mintDetail, j.revokeDetail, j.usageDetail].filter((x) => typeof x === "string").join("; ").replace(/[A-Za-z0-9_.~+\/=-]{32,}/g, "[token]").slice(0, 200)); }
      catch { console.log("? ? ? ? ? ? tagged"); console.log(""); }' "$last.body" 2>/dev/null || echo "? ? ? ? ? ? tagged")
    mint_txt="mint $sc_mint"; [ "$sc_mint" = ok ] && [ "$sc_tag" = untagged ] && mint_txt="mint ok (untagged)"   # (the relay API refused the tag: still a pass)
    verdict=fail
    if [ "$code" = 200 ] && [ "$sc_mint" = ok ] && [ "$sc_revoke" = ok ] && [ "$sc_trip" = no ]; then
      if [ $want_usage -eq 1 ]; then [ "$sc_usage" = ok ] && verdict=pass; else { [ "$sc_usage" = ok ] || [ "$sc_usage" = absent ]; } && verdict=pass; fi
    fi
    if [ "$verdict" = fail ]; then
      if [ "$code" = 404 ] || [ "$code" = 000 ] || { [ "$code" = 200 ] && { [ "$sc_mint" = off ] || { [ $want_usage -eq 1 ] && [ "$sc_usage" = absent ]; }; }; }; then verdict=wait; fi
    fi
    if [ "$verdict" = pass ]; then
      note "$BASE relay self-check: PASS ($mint_txt, revoke ok, usage $sc_usage) after $tries tries ($last.body)"
      if [ "$sc_usage" = ok ]; then note "relay: on, hard stop: on (Galaxy month $sc_gm MB, account month $sc_am MB)"
      else note "relay: on, hard stop: off (no TURN_USAGE_TOKEN: only the estimate caps limit the relay)"; fi
      return 0
    fi
    if [ "$verdict" = fail ]; then
      note "$BASE relay self-check: FAIL (HTTP $code: mint $sc_mint, revoke $sc_revoke, usage $sc_usage$([ "$sc_trip" = yes ] && echo ', hard stop TRIPPED')${sc_detail:+; reason: $sc_detail}; stopped on try $tries; see $last.*)"
      note "relay: on, hard stop NOT confirmed"
      FAIL=1; return 0
    fi
    if [ $(( $(date +%s) - start )) -ge $ADMIN_WAIT_SEC ]; then
      note "$BASE relay self-check: FAIL (still not ready after ${ADMIN_WAIT_SEC} s, $tries tries: HTTP $code, mint $sc_mint, revoke $sc_revoke, usage $sc_usage; the relay secrets are not visible to the Worker; see $last.*)"
      note "relay: on, hard stop NOT confirmed"
      FAIL=1; return 0
    fi
    sleep 10
  done
}
if [ "$RELAY" = on ]; then relay_selfcheck; fi

check_wr_logs
if [ $FAIL -eq 0 ]; then note "ALL CHECKS PASSED"; else note "SOME CHECKS FAILED: read $LOG_DIR/summary.txt and the kept replies"; fi
exit $FAIL
