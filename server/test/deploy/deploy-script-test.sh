#!/bin/bash
# Offline test of scripts/deploy-smg-online.sh: npx (wrangler, vitest) and curl are stubs on PATH; nothing is contacted.
# Checks: --verify-only calls no deploy and no secret put; the admin check waits through 404 / no answer and succeeds on 200;
# it stops at once on 401; every wrangler call carries --config wrangler.smg.toml (whoami: account env only); the token never appears in any log,
# argument, environment, temp file or output; whoami output (account name, e-mail) is never stored; the account id and subdomain pins;
# --print-plan contacts nothing; no wrangler call exists outside wr().
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
SRV=$(cd "$HERE/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/tmp" "$T/seen"
. "$HERE/portability-check.sh"
# MAC_SHIM=1: run the whole test under a PATH whose stat/readlink/date/grep/... refuse the GNU-only forms (a stand-in for the Mac's userland)
if [ -n "${MAC_SHIM:-}" ]; then make_mac_shim "$T/shim"; export PATH="$T/shim:$PATH" MOCK_NO_PROC=1; fi
# the script refuses a run with any of these set (pin_environment): the test starts from a clean environment and sets them one at a time
for v in CLOUDFLARE_API_BASE_URL WRANGLER_API_ENVIRONMENT WRANGLER_AUTH_DOMAIN WRANGLER_AUTH_URL WRANGLER_TOKEN_URL WRANGLER_REVOKE_URL WRANGLER_CLIENT_ID CLOUDFLARE_API_TOKEN CLOUDFLARE_API_KEY CLOUDFLARE_EMAIL \
         WRANGLER_CI_OVERRIDE_NAME CLOUDFLARE_ENV WRANGLER_OUTPUT_FILE_PATH WRANGLER_OUTPUT_FILE_DIRECTORY WRANGLER_LOG_PATH NODE_OPTIONS MSC_HMAC_SOURCE WRANGLER_LOG_SANITIZE NODE_TLS_REJECT_UNAUTHORIZED \
         HTTPS_PROXY https_proxy HTTP_PROXY http_proxy ALL_PROXY all_proxy NO_PROXY no_proxy NODE_EXTRA_CA_CERTS SSL_CERT_FILE CURL_CA_BUNDLE XDG_CONFIG_HOME CF_API_TOKEN CF_API_KEY CF_EMAIL CF_API_BASE_URL CF_ACCOUNT_ID CLOUDFLARE_AUTH_USE_KEYRING CLOUDFLARE_COMPLIANCE_REGION WRANGLER_CF_AUTHORIZATION_TOKEN CLOUDFLARE_ACCESS_CLIENT_ID CLOUDFLARE_ACCESS_CLIENT_SECRET WRANGLER_BUILD_CONDITIONS WRANGLER_BUILD_PLATFORM WRANGLER_CACHE_DIR WRANGLER_REGISTRY_PATH WRANGLER_TRACE_ID WRANGLER_CI_MATCH_TAG LOG_DIR CLOUDFLARE_CLIENT_ID CLOUDFLARE_BASE_URL CLOUDFLARE_API_USER_SERVICE_KEY CLOUDFLARE_INCLUDE_PROCESS_ENV CLOUDFLARE_LOAD_DEV_VARS_FROM_DOT_ENV CLOUDFLARE_CONTAINER_REGISTRY WRANGLER_CI_GENERATE_PREVIEW_ALIAS WORKERS_CI_BRANCH CF_PAGES_UPLOAD_JWT WRANGLER_BASIN_SQL_AUTH_TOKEN; do unset "$v"; done
ACCT=0123456789abcdef0123456789abcdef
SECRET="tok_a.b+c/d=e~f-$(head -c 24 /dev/urandom | od -An -tx1 | tr -d ' \n')"   # (the characters the script allows, with regex-special ones among them)
printf '%s\n' "$SECRET" > "$T/token"; chmod 600 "$T/token"
cat > "$T/bin/npx" <<'S'
#!/bin/bash
echo "npmcfg=${npm_config_update_notifier:-UNSET}/${npm_config_fund:-UNSET}/${npm_config_audit:-UNSET}/${npm_config_logs_max:-UNSET}" >> "$STUB_DIR/seen/npmcfg"
echo "acct=${CLOUDFLARE_ACCOUNT_ID:-NONE} wlogs=${WRANGLER_WRITE_LOGS:-UNSET} metrics=${WRANGLER_SEND_METRICS:-UNSET} nodeenv=${NODE_ENV:-UNSET} npx $*" >> "$STUB_LOG"
[ -n "${STUB_WRITE_WRANGLER_LOG:-}" ] && case "$*" in *"wrangler whoami"*) mkdir -p "$HOME/${STUB_WL_SUB:-.wrangler/logs}" && echo "owner@example.com Acme Hat Company" > "$HOME/${STUB_WL_SUB:-.wrangler/logs}/wrangler-stub.log" ;; esac
case "$*" in *"secret put"*) [ -n "${STUB_PUT_FAIL:-}" ] && { cat > /dev/null; echo "✘ [ERROR] put failed owner@example.com"; exit 1; }; cat > "$STUB_DIR/stdin.$$"; case "$*" in *"secret put ADMIN_TOKEN"*) cp "$STUB_DIR/stdin.$$" "$STUB_DIR/service-admin-key" ;; esac; [ -n "${STUB_ECHO_SECRET:-}" ] && { echo "secret value: $(cat "$STUB_DIR/stdin.$$")"; rm -f "$STUB_DIR/stdin.$$"; } ;; esac
case "$*" in *"vitest"*) [ -n "${STUB_VITEST_FAIL:-}" ] && { echo " Tests  1 failed (1)"; exit 1; }; echo " Tests  1 passed (1)"; exit 0 ;; esac
case "$*" in *"wrangler whoami"*)
  [ -n "${STUB_WHOAMI_FAIL:-}" ] && { echo "Not logged in. owner@example.com"; exit 1; }
  case "$*" in *"--json"*) echo "{\"loggedIn\":true,\"authType\":\"OAuth Token\",\"email\":\"owner@example.com\",\"accounts\":[{\"name\":\"Acme Hat Company\",\"id\":\"${STUB_WHOAMI_ID:-0123456789abcdef0123456789abcdef}\"}],\"tokenPermissions\":[]}"; exit 0 ;; esac
  echo " ⛅️ wrangler 4.0"; echo "Getting User settings..."; echo "You are logged in with an OAuth Token, associated with the email owner@example.com."
  echo "| Account Name        | Account ID |"; echo "| Acme Hat Company    | ${STUB_WHOAMI_ID:-0123456789abcdef0123456789abcdef} |"; exit 0 ;; esac
case "$*" in *"deployments status"*) case "$*" in *"--config wrangler.smg.toml"*) ;; *) echo "NO CONFIG" >> "$STUB_LOG"; exit 9 ;; esac
  case "${STUB_DEPLOY_VERSIONS:-1}" in
    none) echo "✘ [ERROR] Worker \"smg-online\" not found. [code: 10007]"; exit 1 ;;
    fail) echo "✘ [ERROR] boom owner@example.com"; for i in 1 2 3 4 5 6 7 8; do echo "stderr-reason-$i owner@example.com 203.0.113.7" >&2; done; exit 1 ;;
    junk) echo "not json"; exit 0 ;;
    2) echo '{"id":"d1","author_email":"owner@example.com","source":"wrangler","versions":[{"version_id":"v1","percentage":50},{"version_id":"v2","percentage":50}]}'; exit 0 ;;
    *) [ -n "${STUB_DEPLOY_STDERR:-}" ] && { echo "▲ [WARNING] Proxy environment variables detected. We'll use your proxy for fetch requests." >&2; echo "(node:1) ExperimentalWarning: something owner@example.com" >&2; }
       echo '{"id":"d1","author_email":"owner@example.com","source":"'"${STUB_DEPLOY_SOURCE:-wrangler}"'","versions":[{"version_id":"v1","percentage":100}]}'; exit 0 ;; esac ;; esac
case "$*" in *"secret list"*) case "$*" in *"--config wrangler.smg.toml"*) ;; *) echo "NO CONFIG" >> "$STUB_LOG"; exit 9 ;; esac
  [ -n "${STUB_SECRETS_FAIL:-}" ] && { echo "✘ [ERROR] ${STUB_SECRETS_FAIL} owner@example.com"; exit 1; }
  printf '['; sep=""; for n in ${STUB_SECRETS:-}; do printf '%s{"name":"%s","type":"secret_text"}' "$sep" "$n"; sep=","; done; echo ']'; exit 0 ;; esac
case "$*" in *"wrangler"*) case "$*" in *"--config wrangler.smg.toml"*) ;; *) echo "NO CONFIG" >> "$STUB_LOG"; exit 9 ;; esac
  echo "Deployed by owner@example.com"; echo "| Acme Hat Company    | 0123456789abcdef0123456789abcdef |"
  case "$*" in *"--dry-run"*) [ -n "${STUB_DRY_URL:-}" ] && echo "Would publish $STUB_DRY_URL"; [ -n "${STUB_DRY_HOST:-}" ] && echo "Would attach $STUB_DRY_HOST" ;; *"wrangler deploy"*) [ -n "${STUB_DEPLOY_STRICT:-}" ] && { echo "✘ [ERROR] Aborting the upload operation because of conflicts. To override and upload anyway, remove the \`--strict\` flag"; i=0; while [ $i -lt ${STUB_DEPLOY_MANY:-0} ]; do echo "  extra $i: detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail"; i=$((i+1)); done; exit 1; }; [ -n "${STUB_DEPLOY_APIERR:-}" ] && { echo "✘ [ERROR] A request to the Cloudflare API (/accounts/${CLOUDFLARE_ACCOUNT_ID}/workers/scripts/smg-online) failed. owner@example.com"; echo "  Authentication error [code: 10000] account ${CLOUDFLARE_ACCOUNT_ID}"; i=0; while [ $i -lt ${STUB_DEPLOY_MANY:-0} ]; do echo "  extra $i: detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail detail"; i=$((i+1)); done; exit 1; }; echo "Published ${STUB_DEPLOY_URL-https://smg-online.example.workers.dev}${STUB_DEPLOY_HOST-}" ;; esac
  [ -n "${STUB_SETTINGS_BLOCK:-}" ] && { echo "Getting User settings..."; echo "👋 You are logged in with an OAuth Token, associated with the email owner@example.com."; echo "| Account Name | Account ID |"; echo "| Hat Co | 0123456789abcdef0123456789abcdef |"; echo "afterblock-marker"; } ;; esac
exit 0
S
cat > "$T/bin/curl" <<'S'
#!/bin/bash
echo "curl $*" >> "$STUB_LOG"; echo "curl $*" >> "$STUB_DIR/seen/curl-args"
out=""; hdrs=""; url=""; ua=""; admin=no; method=GET
env >> "$STUB_DIR/seen/curl-env"
while [ $# -gt 0 ]; do case "$1" in -o) out=$2; shift 2 ;; -D) hdrs=$2; shift 2 ;; -X) method=$2; shift 2 ;; -K) admin=yes; cat >> "$STUB_DIR/seen/hdr-stdin"; shift 2 ;; -H) case "$2" in "User-Agent: "*) ua=${2#User-Agent: } ;; esac; shift 2 ;; -w|--max-time) shift 2 ;; -*) shift ;; *) url=$1; shift ;; esac; done
code=200; body='{"ok":true}'
case "$url" in
  */v1/info) body=$(cat "$STUB_DIR/info-body" 2>/dev/null || echo '{"proto":1,"game":"smg","app":{"min":"3.0.0","latest":"3.0.0"}}')
     if case "$url" in https://galaxy.psvita.dev/*) true ;; *) false ;; esac && [ -s "$STUB_DIR/info-seq" ]; then
       code=$(head -1 "$STUB_DIR/info-seq"); sed 1d "$STUB_DIR/info-seq" > "$STUB_DIR/info-seq.n"; mv "$STUB_DIR/info-seq.n" "$STUB_DIR/info-seq"
       [ "$code" = 000 ] && { echo "curl: (6) Could not resolve host" >&2; exit 6; }
     fi ;;
  */v1/prove\?n=*) pn=$(cat "$STUB_DIR/prove-n" 2>/dev/null || echo 0); pn=$((pn+1)); echo $pn > "$STUB_DIR/prove-n"
     pm=$(sed -n "${pn}p" "$STUB_DIR/prove-seq" 2>/dev/null); [ -n "$pm" ] || pm=${STUB_PROVE:-ok}
     case "$url" in https://galaxy.psvita.dev/*) ;; *) [ -z "${STUB_PROVE_WD:-}" ] || pm=$STUB_PROVE_WD ;; esac
     phost=${url#https://}; phost=${phost%%/*}; pnon=${url##*n=}
     pkey=$(head -1 "$STUB_DIR/service-admin-key" 2>/dev/null || true); [ -n "$pkey" ] || pkey=$(head -1 "$STUB_DIR/token")
     good=$(printf 'smg-online prove v1\n%s\n%s' "$phost" "$pnon" | openssl dgst -sha256 -hmac "$pkey" -hex | sed 's/^.* //')
     other=$(printf 'smg-online prove v1\n%s\n%s' "other.example" "$pnon" | openssl dgst -sha256 -hmac "$pkey" -hex | sed 's/^.* //')
     case "$pm" in
       ok) body="{\"proof\":\"$good\"}" ;;
       wrong) body="{\"proof\":\"$(printf '%064d' 0)\"}" ;;
       otherhost) body="{\"proof\":\"$other\"}" ;;
       nokey) body="{\"proof\":\"$(printf 'smg-online prove v1\n%s\n%s' "$phost" "$pnon" | openssl dgst -sha256 -hmac "some-other-token-0123456789" -hex | sed 's/^.* //')\"}" ;;
       404) code=404; body='{"error":{"code":"not_found","message":"no such endpoint"}}' ;;
       429) code=429; body='{"error":{"code":"rate_limited"}}' ;;
       500) code=500; body='{"error":"x"}' ;;
       noproof) body='{"ok":true}' ;;
       badshape) body='{"proof":"zz"}' ;;
       upper) body="{\"proof\":\"$(printf '%s' "$good" | tr a-f A-F)\"}" ;;
       infocopy) body=$(cat "$STUB_DIR/info-body" 2>/dev/null || echo '{"proto":1,"game":"smg","app":{"min":"3.0.0","latest":"3.0.0"}}') ;;
       000) echo "curl: (7) Failed to connect" >&2; exit 7 ;;
     esac ;;
  */v1/rank/get) code=${STUB_RANK_CODE:-404} ;;
  */v1/admin/turn-selfcheck) sn=$(cat "$STUB_DIR/sc-n" 2>/dev/null || echo 0); sn=$((sn+1)); echo $sn > "$STUB_DIR/sc-n"
     line=$(sed -n "${sn}p" "$STUB_DIR/sc-seq" 2>/dev/null); [ -n "$line" ] || line=$(cat "$STUB_DIR/sc-default" 2>/dev/null || echo '200 {"mint":"ok","revoke":"ok","usage":"absent","galaxyMonthMb":null,"accountMonthMb":null,"tripped":false}')
     code=${line%% *}; body=${line#* }; [ "$method" = POST ] || { code=405; body='{}'; }
     [ "$code" = 000 ] && { echo "curl: (7) Failed to connect" >&2; exit 7; } ;;
  */v1/admin/stats) n=$(cat "$STUB_DIR/admin-n" 2>/dev/null || echo 0); n=$((n+1)); echo $n > "$STUB_DIR/admin-n"
     code=$(sed -n "${n}p" "$STUB_DIR/admin-seq"); [ -n "$code" ] || code=$(cat "$STUB_DIR/admin-default" 2>/dev/null || echo 200)
     [ "$code" = 000 ] && { echo "curl: (7) Failed to connect" >&2; exit 7; } ;;
esac
echo "$method|$(printf '%s' "$url" | sed -E 's/\?n=[0-9a-f]{32}$/?n=<random-hex32>/')|$ua|$admin" >> "$STUB_DIR/reqs"
[ -n "$out" ] && echo "$out $(ls -ld "$(dirname "$out")" | cut -c1-10)" >> "$STUB_DIR/seen/rawdir"
[ -n "$hdrs" ] && echo "HTTP/2 $code" > "$hdrs"; [ -n "$out" ] && printf '%s' "$body" > "$out"
if [ -n "${STUB_LEAK:-}" ]; then [ -n "$hdrs" ] && echo "x-leak: rawleak-marker@example.com 9f8e7d6c5b4a39281706f5e4d3c2b1a0" >> "$hdrs"; echo "curl: warning rawleak-marker@example.com 9f8e7d6c5b4a39281706f5e4d3c2b1a0" >&2; fi
printf '%s' "$code"
S
cat > "$T/bin/sleep" <<'S'
#!/bin/bash
exit 0
S
# sed stub: every time the scrubber runs, no kept file under a log folder may hold raw remote text yet (the raw marker below is what STUB_LEAK makes the curl stub emit)
REAL_SED=$(command -v sed)
cat > "$T/bin/sed" <<S
#!/bin/bash
if grep -rqs "rawleak-marker@example.com" "\$STUB_DIR"/log* 2>/dev/null; then echo "raw text in a log folder when the scrubber ran" >> "\$STUB_DIR/leak.log"; fi
exec $REAL_SED "\$@"
S
chmod +x "$T/bin/"*
export PATH="$T/bin:$PATH" STUB_LOG="$T/stub.log" STUB_DIR="$T" TMPDIR="$T/tmp"
fail=0; NOK=0; NFAIL=0
check() { if eval "$2"; then echo "ok   $1"; NOK=$((NOK + 1)); else echo "FAIL $1"; fail=1; NFAIL=$((NFAIL + 1)); [ -f "$T/log$RUNN/summary.txt" ] && sed 's/^/     /' "$T/log$RUNN/summary.txt"; tail -3 "$T/out$RUNN" | sed 's/^/     /'; fi; }
run() { [ -z "${RUN_SRV:-}" ] || [ -e "$RUN_SRV/node_modules" ] || ln -s "$SRV/node_modules" "$RUN_SRV/node_modules"; rm -f "$T/admin-n" "$T/sc-n" "$T/prove-n" "$T/service-admin-key" "$T/stub.log" "$T/admin-default" "$T/reqs"; [ -f "$T/admin-default.next" ] && mv "$T/admin-default.next" "$T/admin-default"; printf '%s\n' $1 > "$T/admin-seq"; shift
  local launch=(bash)
  if [ -n "${RUN_PID_FILE:-}" ]; then
    # Record the shell's PID, then replace it with the deploy script.
    launch=(bash -c 'printf "%s\n" "$$" > "$1" || exit 1; shift; exec bash "$@"' bash "$RUN_PID_FILE")
  fi
  (cd "${RUN_SRV:-$SRV}" && "${launch[@]}" scripts/deploy-smg-online.sh --account-id $ACCT ${RUN_URL:+--url $RUN_URL} $([ -n "${RUN_NOSUB:-}" ] || echo --expect-subdomain example) --admin-token-file "$T/token" --log-dir "$T/log$RUNN" "$@") > "$T/out$RUNN" 2>&1; echo $?; }
srun() { rm -f "$T/stub.log" "$T/reqs"; (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --log-dir "$T/log$RUNN" "$@") > "$T/out$RUNN" 2>&1; echo $?; }
export DEPLOY_OFFLINE_TEST=1 DEPLOY_ADMIN_WAIT_SEC=${DEPLOY_ADMIN_WAIT_SEC:-120}
RUNN=1; rc=$(run "404 000 404 200" --verify-only)
check "verify-only: passes after 404 / no answer / 404 / 200" '[ "$rc" = 0 ] && grep -q "200 after 4 tries" "$T/log1/summary.txt"'
check "verify-only: no deploy, no secret put" '! grep -E -q "wrangler deploy |secret put" "$T/stub.log"'
RUNN=2; rc=$(run "404 401" --set-admin-token --skip-tests)
check "a 401 stops the admin check at once (try 2)" '[ "$rc" = 1 ] && grep -q "401 on try 2: stopped" "$T/log2/summary.txt" && [ "$(cat "$T/admin-n")" = 2 ]'
check "deploy path: whoami, dry-run, deploy, secret put: EVERY wrangler call carries --config wrangler.smg.toml (whoami included)" 'grep -q "deploy --dry-run" "$T/stub.log" && grep -q "secret put ADMIN_TOKEN --config wrangler.smg.toml" "$T/stub.log" && grep -q "wrangler whoami --json --config wrangler.smg.toml" "$T/stub.log" && ! grep -q "NO CONFIG" "$T/stub.log" && [ $(grep -c "npx --no wrangler" "$T/stub.log") = $(grep "npx --no wrangler" "$T/stub.log" | grep -c "wrangler.*--config wrangler.smg.toml") ]'
check "the secret went to wrangler on stdin" 'cat "$T"/stdin.* 2>/dev/null | grep -q "$SECRET"'
check "the admin header reached curl on stdin (-K -) as a config line" 'grep -q "^header = \"Authorization: Bearer $SECRET\"$" "$T/seen/hdr-stdin"'
RUNN=3; rc=$(echo 404 > "$T/admin-default.next"; DEPLOY_ADMIN_WAIT_SEC=2 run "404" --verify-only)
check "404 until the time limit (2 s here, 120 s for real): gives up" '[ "$rc" = 1 ] && grep -q "after 2 s" "$T/log3/summary.txt"'
check "the token appears in no log, argument or output" '! grep -rq "$SECRET" "$T/log1" "$T/log2" "$T/log3" "$T/stub.log" "$T/out1" "$T/out2" "$T/out3" "$T/tmp" "$T/seen/curl-env"'
check "every reply kept with its request" '[ -f "$T/log1/$(ls "$T/log1" | grep -m1 admin-stats-try1.status)" ] && ls "$T/log1" | grep -q "admin-stats-try4.body" && ls "$T/log1" | grep -q "admin-stats-try4.request"'
RUNN=4; rc=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --url https://msc-online.example.workers.dev --admin-token-file "$T/token" --verify-only) >/dev/null 2>&1; echo $?)
check "refuses the Strikers address" '[ "$rc" = 1 ]'

# ---- item 1: whoami output and e-mail / account name never stored
check "whoami-not-stored: whoami log is the single word OK" '[ "$(cat "$T"/log2/*-wrangler-whoami.txt)" = OK ]'
check "scrub: no e-mail address or account name in any log, output or temp file" '! grep -rqE "owner@example.com|Acme Hat" "$T/log1" "$T/log2" "$T/log3" "$T/out1" "$T/out2" "$T/out3" "$T/tmp"'
check "scrub: other wrangler logs keep the text but with [EMAIL] and the table row removed" 'grep -q "\[EMAIL\]" "$T"/log2/*-wrangler-deploy.txt && grep -q "account table row removed" "$T"/log2/*-wrangler-deploy.txt'
RUNN=5; rc=$(STUB_WHOAMI_FAIL=1 run "200" --verify-only)
check "whoami failure: refused, reason kept without the e-mail" '[ "$rc" = 1 ] && grep -q "^FAILED" "$T"/log5/*-wrangler-whoami.txt && ! grep -rq "owner@example.com" "$T/log5" "$T/out5"'

# ---- item 2: no copy of the token on disk
check "token-never-on-disk: no hit in the log folders or the temp dir; temp dir empty" '! grep -rq "$SECRET" "$T"/log* "$T/tmp" && [ -z "$(find "$T/tmp" -type f)" ]'
check "token-never-on-disk: kept request shows Authorization: Bearer [REDACTED]" 'grep -q "Authorization: Bearer \[REDACTED\]" "$T/log1/$(ls "$T/log1" | grep -m1 admin-stats-try1.request)"'
check "token-never-on-disk: curl got no -H @file and no token in its arguments" '! grep -q "curl.*-H @" "$T/seen/curl-args" && ! grep -q "$SECRET" "$T/seen/curl-args" && grep -q "curl.* -K - " "$T/seen/curl-args"'

# ---- item 3: account and subdomain pins
bad() { (cd "$SRV" && bash scripts/deploy-smg-online.sh --url https://smg-online.example.workers.dev --admin-token-file "$T/token" --log-dir "$T/logb" --verify-only "$@") >/dev/null 2>&1; echo $?; }
rm -f "$T/stub.log"
check "account-pin: --account-id is required" '[ "$(bad)" = 1 ]'
check "account-pin: a wrong format is refused (short, non-hex, 33 chars)" '[ "$(bad --account-id abc123)" = 1 ] && [ "$(bad --account-id zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz)" = 1 ] && [ "$(bad --account-id ${ACCT}0)" = 1 ] && [ ! -s "$T/stub.log" ]'
RUNN=6; rc=$(STUB_WHOAMI_ID=ffffffffffffffffffffffffffffffff run "200" --verify-only)
check "account-pin: refused when whoami does not list the id (no deploy, no live request)" '[ "$rc" = 1 ] && grep -q "does not list account" "$T/out6" && ! grep -qs "^curl" "$T/stub.log"'
check "account-pin: CLOUDFLARE_ACCOUNT_ID was set for every wrangler call" '[ -s "$T/stub.log" ] && ! grep "npx --no wrangler" "$T/stub.log" | grep -vq "^acct=$ACCT "'
check "wrangler-no-disk-log: WRANGLER_WRITE_LOGS=false and WRANGLER_SEND_METRICS=false on every wrangler call (whoami included)" '[ -s "$T/stub.log" ] && grep -q "npx --no wrangler whoami" "$T/stub.log" && ! grep "npx --no wrangler" "$T/stub.log" | grep -vq " wlogs=false metrics=false "'
RUNN=7; rc=$(STUB_DRY_URL=https://smg-online.other.workers.dev run "200" --skip-tests)
check "subdomain-pin: a wrong URL in the dry run stops BEFORE the deploy" '[ "$rc" = 1 ] && grep -q "deploy --dry-run" "$T/stub.log" && ! grep "wrangler deploy " "$T/stub.log" | grep -qv dry-run'
RUNN=8; rc=$(STUB_DEPLOY_URL=https://smg-online.other.workers.dev run "200" --skip-tests --set-admin-token)
check "subdomain-pin: a wrong URL after the deploy stops at once, loudly: no secret, no live check" '[ "$rc" = 1 ] && grep -q "WRONG ADDRESS" "$T/out8" && ! grep -q "secret put" "$T/stub.log" && ! grep "^curl" "$T/stub.log" | grep -q .'
RUNN=9; rc=$(STUB_DEPLOY_URL=https://msc-online.example.workers.dev run "200" --skip-tests)
check "subdomain-pin: the Strikers worker name in the reported URL is a mismatch" '[ "$rc" = 1 ] && grep -q "WRONG ADDRESS" "$T/out9"'
RUNN=10; rc=$(STUB_DEPLOY_URL= STUB_DEPLOY_HOST= run "200" --skip-tests)
check "subdomain-pin: no URL reported by the deploy: noted, the live checks ask each address" '[ "$rc" = 0 ] && grep -q "the deploy reported no address" "$T/out10"'
RUNN=11; rc=$(STUB_DEPLOY_URL=https://smg-online.example.workers.dev run "200" --skip-tests --expect-subdomain example)
check "subdomain-pin: the expected subdomain (also the default from --url) passes" '[ "$rc" = 0 ]'
rc=$(RUN_URL=https://smg-online.example.workers.dev run "200" --verify-only --expect-subdomain other)
check "subdomain-pin: --expect-subdomain that differs from --url is refused" '[ "$rc" = 1 ]'

# ---- 14 Nov: the custom domain https://galaxy.psvita.dev
RUNN=40; rc=$(run "200" --verify-only)
check "custom-domain: --url defaults to https://galaxy.psvita.dev; verify-only checks it AND workers.dev (workers_dev is on)" '[ "$rc" = 0 ] && for p in /v1/info /times /v1/rank/get /v1/admin/stats; do grep -q "curl.* https://galaxy.psvita.dev$p" "$T/stub.log" || exit 1; done; for p in /v1/info /times /v1/rank/get; do grep -q "curl.* https://smg-online.example.workers.dev$p" "$T/stub.log" || exit 1; done'
check "admin check on BOTH addresses: custom domain and workers.dev (verify-only)" 'grep -q "curl.* https://galaxy.psvita.dev/v1/admin/stats" "$T/stub.log" && grep -q "curl.* https://smg-online.example.workers.dev/v1/admin/stats" "$T/stub.log" && grep -q "smg-online.example.workers.dev admin check: 200" "$T/log40/summary.txt"'
check "custom-domain: nothing else is contacted (only the two pinned addresses)" '! grep "^curl" "$T/stub.log" | grep -E -v "https://galaxy.psvita.dev/|https://smg-online.example.workers.dev/"'
RUNN=41; rm -f "$T/stub.log"; rc=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --admin-token-file "$T/token" --log-dir "$T/log41" --verify-only) > "$T/out41" 2>&1; echo $?)
check "workers-dev: workers_dev = true and NO --expect-subdomain is refused before anything is contacted, with the instruction" '[ "$rc" = 1 ] && grep -q "workers.dev is on" "$T/out41" && grep -q "pass --expect-subdomain <name> so that address is checked too" "$T/out41" && [ ! -s "$T/stub.log" ]'
mkdir -p "$T/srv-off"; cp -r "$SRV/scripts" "$T/srv-off/scripts"; ln -s "$SRV/node_modules" "$T/srv-off/node_modules"; sed 's/^workers_dev = true/workers_dev = false/' "$SRV/wrangler.smg.toml" > "$T/srv-off/wrangler.smg.toml"
RUNN=42; rc=$(RUN_SRV="$T/srv-off" run "200" --verify-only)
check "workers-dev: workers_dev = false WITH --expect-subdomain is a contradiction: refused before anything is contacted" '[ "$rc" = 1 ] && grep -q "workers_dev = false but a workers.dev address was given" "$T/out42" && [ ! -s "$T/stub.log" ]'
RUNN=43; rc=$(RUN_NOSUB=1 RUN_SRV="$T/srv-off" run "200" --verify-only)
check "workers-dev: workers_dev = false without the flag is allowed (the release step): checks run on the custom domain ONLY, admin included" '[ "$rc" = 0 ] && ! grep "^curl" "$T/stub.log" | grep -q workers.dev && grep -q "galaxy.psvita.dev/v1/admin/stats" "$T/stub.log" && ! grep -q "workers.dev admin check" "$T/log43/summary.txt"'
pl_on=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --print-plan) 2>&1 | grep "workers.dev address")
pl_off=$( (cd "$T/srv-off" && bash scripts/deploy-smg-online.sh --account-id $ACCT --admin-token-file "$T/token" --print-plan) 2>&1 | grep "workers.dev address")
check "workers-dev: the plan prints on or off from the config" 'echo "$pl_on" | grep -q "https://smg-online.example.workers.dev (on)" && echo "$pl_off" | grep -q "(off)" && ! echo "$pl_off" | grep -q "(on)"'
RUNN=143; rc=$(RUN_URL=https://smg-online.example.workers.dev run "200" --verify-only)
check "custom-domain: the legacy workers.dev --url is still accepted" '[ "$rc" = 0 ]'
check "custom-domain: other addresses are refused (lookalike, another psvita.dev name, http, Strikers)" 'for u in https://galaxy.psvita.dev.evil.example https://strikers.psvita.dev http://galaxy.psvita.dev https://galaxy.psvita.dev/v1 https://msc-online.example.workers.dev; do [ "$(RUN_URL=$u run 200 --verify-only)" = 1 ] || exit 1; done'
# (cfgvariant is defined again further down, the same)
cfgvariant() { # name, sed expression: a copy of the service dir whose wrangler.smg.toml is edited by the expression
  mkdir -p "$T/srv-$1"; cp -r "$SRV/scripts" "$T/srv-$1/scripts"; ln -sfn "$SRV/node_modules" "$T/srv-$1/node_modules"
  sed "$2" "$SRV/wrangler.smg.toml" > "$T/srv-$1/wrangler.smg.toml"
}
# round g: wrangler never publishes routes or domains: the config has none, and a config with any is refused
check "no-routes: the shipped wrangler.smg.toml has no route, routes or custom_domain (outside comments)" '! grep -vE "^[[:space:]]*#" "$SRV/wrangler.smg.toml" | grep -qE "^[[:space:]]*\[{0,2}[[:space:]]*[\"\x27]?routes?[\"\x27]?([][:space:].=]|$)|custom_domain"'
routebad=""; n=44
for res in 'routes = [{ pattern = "galaxy.psvita.dev", custom_domain = true }]' 'route = "galaxy.psvita.dev/*"' '[[routes]]\npattern = "galaxy.psvita.dev"\ncustom_domain = true' '[[routes]]\npattern = "x.example/*"' '"routes" = []' "'route' = \"x\"" 'x = { custom_domain = true }' '[routes]'; do
  RUNN=$n; cfgvariant routes 's/^KILL_SWITCH = .*/&/'; printf '\n%b\n' "$res" >> "$T/srv-routes/wrangler.smg.toml"
  if [ "$n" = 44 ]; then sed -i.bak 's/^\[vars\]/routes = [{ pattern = "galaxy.psvita.dev", custom_domain = true }]\n[vars]/' "$T/srv-routes/wrangler.smg.toml"; fi
  rc=$(RUN_SRV="$T/srv-routes" run "200" --verify-only)
  [ "$rc" = 1 ] && grep -q "declares a route or custom domain" "$T/out$n" && grep -q "attached ONCE, by the owner, in the Cloudflare dashboard" "$T/out$n" && grep -q "wrangler must never publish routes or domains" "$T/out$n" && [ ! -s "$T/stub.log" ] || routebad="$routebad $n"; n=$((n+1)); done
check "no-routes: routes, route, [[routes]], custom_domain (also quoted keys and a top-level routes before [vars]) anywhere in the config: refused before anything runs, with the dashboard instruction${routebad:+ (failed run numbers:$routebad)}" '[ -z "$routebad" ]'
RUNN=45; rc=$(STUB_DEPLOY_HOST= run "200" --skip-tests --set-admin-token)
check "custom-domain: a deploy that reports no psvita.dev name is the normal case now (wrangler does not touch the domain): passes" '[ "$rc" = 0 ] && grep -q "wrangler reported only the expected address (workers.dev) and no psvita.dev name" "$T/log45/summary.txt"'
RUNN=46; rc=$(STUB_DEPLOY_HOST="  other.psvita.dev (custom domain)" run "200" --skip-tests --set-admin-token)
check "custom-domain: another psvita.dev name in the deploy output is a WRONG ADDRESS (no secret, no live check)" '[ "$rc" = 1 ] && grep -q "WRONG ADDRESS" "$T/out46" && ! grep -q "secret put" "$T/stub.log" && ! grep "^curl" "$T/stub.log" | grep -q .'
RUNN=47; rc=$(STUB_DRY_HOST="strikers.psvita.dev" run "200" --skip-tests)
check "custom-domain: a psvita.dev name in the dry run stops BEFORE the deploy" '[ "$rc" = 1 ] && grep -q "and no psvita.dev name" "$T/out47" && ! grep "wrangler deploy " "$T/stub.log" | grep -qv dry-run'
RUNN=52; rc=$(STUB_DEPLOY_HOST="  galaxy.psvita.dev (custom domain)" run "200" --skip-tests --set-admin-token)
check "custom-domain: even galaxy.psvita.dev in the deploy output is a stop now (wrangler must report no psvita.dev name at all): no secret, no live check" '[ "$rc" = 1 ] && grep -q "WRONG ADDRESS" "$T/out52" && ! grep -q "secret put" "$T/stub.log" && ! grep "^curl" "$T/stub.log" | grep -q .'
RUNN=48; rc=$(run "200" --skip-tests --set-admin-token)
check "custom-domain: full deploy path passes (the custom domain is checked by the live checks only)" '[ "$rc" = 0 ] && grep -q "https://galaxy.psvita.dev prove: ok (the service holds the admin token; host bound)" "$T/log48/summary.txt" && ! grep -qE "dig |nslookup |^host " "$T/stub.log"'

# ---- first deploy: the new custom domain may not answer for a while
RUNN=49; printf '000\n000\n000\n200\n' > "$T/info-seq"; rc=$(DEPLOY_CUSTOM_WAIT_SEC=60 run "200" --verify-only); rm -f "$T/info-seq"
check "first deploy: /v1/info on the custom domain retried through 'no answer' (4 tries), then passes" '[ "$rc" = 0 ] && grep -q "4 tries for /v1/info, last answer 200" "$T/log49/summary.txt" && grep -q "a freshly attached custom domain needs DNS and a certificate, which can take minutes" "$T/log49/summary.txt"'
RUNN=50; printf '000\n000\n000\n000\n000\n000\n000\n000\n' > "$T/info-seq"; rc=$(DEPLOY_CUSTOM_WAIT_SEC=0 run "200" --verify-only); rm -f "$T/info-seq"
check "no answer for the whole wait: gives up and FAILS (the wait is bounded)" '[ "$rc" = 1 ] && grep -q "GET /v1/info: 000" "$T/log50/summary.txt"'
RUNN=51; printf '500\n' > "$T/info-seq"; rc=$(run "200" --verify-only); rm -f "$T/info-seq"
check "a wrong ANSWER (not 'no answer') is not retried: fails at once" '[ "$rc" = 1 ] && ! grep -q "tries for /v1/info" "$T/log51/summary.txt"'
check "--help works without GNU sed" 'bash "$SRV/scripts/deploy-smg-online.sh" --help | grep -q "^#   --verify-only"'

# ---- item 4: no wrangler call outside wr()
check "wr-only: the script passes its own self-check" 'bash "$SRV/scripts/deploy-smg-online.sh" --self-check >/dev/null'
cp "$SRV/scripts/deploy-smg-online.sh" "$T/doctored.sh"; printf '\nnpx --no wrangler deploy\n' >> "$T/doctored.sh"
check "wr-only: a direct call is caught by the self-check" '! bash "$T/doctored.sh" --self-check >/dev/null 2>&1'
cp "$SRV/scripts/deploy-smg-online.sh" "$T/doctored2.sh"; printf '\nif true; then wrangler secret put X; fi\n' >> "$T/doctored2.sh"
check "wr-only: a bare wrangler command is caught too" '! bash "$T/doctored2.sh" --self-check >/dev/null 2>&1'

# ---- item 5: --print-plan
rm -f "$T/stub.log"
plan=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --url https://smg-online.example.workers.dev --admin-token-file "$T/token" --log-dir "$T/logplan" --set-admin-token --print-plan) 2>&1; echo "rc=$?")
check "print-plan: exit 0, nothing run, nothing written" 'echo "$plan" | grep -q "^rc=0" && [ ! -e "$T/stub.log" ] && [ ! -e "$T/logplan" ]'
check "print-plan: shows the commands, the account id as last 4 only, and NO part of the token" 'echo "$plan" | grep -q "npx --no wrangler deploy " && echo "$plan" | grep -q "\*\*\*\*cdef" && ! echo "$plan" | grep -E -q "$ACCT|$SECRET|${SECRET: -4}"'
# the plan must not open the token file at all: a token path that does not exist still gives a plan
plan2=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --url https://smg-online.example.workers.dev --admin-token-file "$T/no-such-token-file" --print-plan) 2>&1; echo "rc=$?")
check "print-plan: never opens the token file (a missing token file still prints the plan)" 'echo "$plan2" | grep -q "^rc=0" && echo "$plan2" | grep -q "npx --no wrangler deploy "'
# ---- a failing local test suite must stop the script BEFORE any deploy (the exit code must not be lost in a pipe)
RUNN=30; rc=$(STUB_VITEST_FAIL=1 run "200")
check "tests-fail-stops: failing local suites give a non-zero exit" '[ "$rc" != 0 ]'
check "tests-fail-stops: nothing was deployed, no secret set" '! grep -E -q "wrangler deploy |secret put" "$T/stub.log"'
check "tests-fail-stops: the summary says the tests failed" 'grep -q "tests: FAILED" "$T/log30/summary.txt"'
# ---- a wrangler that writes its debug log anyway must make the run FAIL (the log would hold account details)
RUNN=31; rc=$(HOME="$T/home31" STUB_WRITE_WRANGLER_LOG=1 run "200" --verify-only)
check "wrangler-no-disk-log: a debug log file appearing during the run fails it" '[ "$rc" != 0 ] && grep -q "debug log file" "$T/log31/summary.txt"'
RUNN=35; rc=$(HOME="$T/home35" STUB_WL_SUB="Library/Preferences/.wrangler/logs" STUB_WRITE_WRANGLER_LOG=1 run "200" --verify-only)
check "wrangler-no-disk-log: a debug log under ~/Library/Preferences/.wrangler/logs (where wrangler writes on macOS) fails the run too" '[ "$rc" != 0 ] && grep -q "debug log file" "$T/log35/summary.txt"'
# ---- a wrangler API error prints the full account id in its resource path: no full id may reach any kept file or the console
RUNN=32; rc=$(STUB_DEPLOY_APIERR=1 run "200" --skip-tests)
check "api-error-id: a failed deploy gives a non-zero exit" '[ "$rc" != 0 ]'
check "deploy-failed: the console shows the last lines of the SCRUBBED wrangler output, names the kept file, says STOP, and no longer advises --verify-only" 'grep -q "wrangler.s error (scrubbed, from its first \[ERROR\]" "$T/out32" && grep -q "Authentication error" "$T/out32" && grep -q "\[EMAIL\]" "$T/out32" && ! grep -q "Deployed by" "$T/out32" && grep -q "wrangler deploy FAILED, and it may have failed part-way" "$T/out32" && grep -q "STOP: report this message and run --verify-only to see what is live. Do not deploy again without the owner." "$T/out32" && grep -q "wrangler-deploy.txt" "$T/out32"'
check "api-error-id: the full account id is in no kept file and not in the console output" '! grep -rq "$ACCT" "$T/log32" "$T/out32"'
check "api-error-id: the error text itself is kept, with the id as its last 4 and the e-mail removed" 'grep -rq "accounts/\[id \*\*\*\*${ACCT: -4}\]/workers/scripts/smg-online" "$T/log32" && ! grep -rq "owner@example.com" "$T/log32" "$T/out32"'
# ---- no full account id in ANY kept file of ANY run of this test
check "no-full-id-anywhere: no run of this test left the full account id in a log folder" '! grep -rq "$ACCT" "$T"/log* "$T"/out* 2>/dev/null'
# ---- the Strikers-only route must answer 404 exactly (405 would mean the route exists for another method)
RUNN=33; rc=$(STUB_RANK_CODE=405 run "200" --verify-only)
check "rank-404-only: a 405 on the Strikers-only route fails the checks" '[ "$rc" != 0 ] && grep -q "expected 404" "$T/log33/summary.txt"'
# ---- a wrangler that echoes the secret it was given must not get it into any kept file
RUNN=34; rc=$(STUB_ECHO_SECRET=1 run "200" --skip-tests --set-admin-token)
check "secret-echo: a wrangler that echoes the secret leaves no copy in the logs or the console" '! grep -rq "$SECRET" "$T/log34" "$T/out34" && grep -rq "\[TOKEN\]" "$T/log34"'
# ---- 15 Nov (Codex review): app min AND latest, admin check on both addresses, the game's User-Agent, order of writes
RUNN=60; rc=$(run "200" --verify-only)
check "app-versions: /v1/info app min and latest equal the config's values (derived, 3.0.0 now)" '[ "$rc" = 0 ] && grep -q "app min 3.0.0 latest 3.0.0" "$T/log60/summary.txt" && grep -q "^MIN_APP_VERSION = \"3.0.0\"" "$SRV/wrangler.smg.toml"'
RUNN=61; echo '{"proto":1,"game":"smg","app":{"min":"2.9.0","latest":"3.0.0"}}' > "$T/info-body"; rc=$(run "200" --verify-only)
check "app-versions: a stale app.min fails the run and says which values were seen" '[ "$rc" = 1 ] && grep -q "expected .smg 3.0.0 3.0.0." "$T/log61/summary.txt"'
RUNN=62; echo '{"proto":1,"game":"smg","app":{"min":"3.0.0","latest":"2.9.0"}}' > "$T/info-body"; rc=$(run "200" --verify-only)
check "app-versions: a stale app.latest fails the run" '[ "$rc" = 1 ] && grep -q "expected .smg 3.0.0 3.0.0." "$T/log62/summary.txt"'
RUNN=63; echo '{"proto":1,"game":"smg"}' > "$T/info-body"; rc=$(run "200" --verify-only)
check "app-versions: no app object at all fails the run" '[ "$rc" = 1 ]'
rm -f "$T/info-body"
# the expected values follow the config of the package (no second copy in the script)
mkdir -p "$T/srv-ver/scripts"; cp "$SRV/scripts/deploy-smg-online.sh" "$T/srv-ver/scripts/"; sed 's/^MIN_APP_VERSION = "3.0.0"/MIN_APP_VERSION = "3.1.0"/; s/^LATEST_APP_VERSION = "3.0.0"/LATEST_APP_VERSION = "3.2.0"/' "$SRV/wrangler.smg.toml" > "$T/srv-ver/wrangler.smg.toml"
RUNN=64; rc=$(RUN_SRV="$T/srv-ver" run "200" --verify-only)
check "app-versions: the expectation comes from the config (3.1.0 / 3.2.0 makes the default answer fail)" '[ "$rc" = 1 ] && grep -q "expected .smg 3.1.0 3.2.0." "$T/log64/summary.txt"'
RUNN=65; echo '{"proto":1,"game":"smg","app":{"min":"3.1.0","latest":"3.2.0"}}' > "$T/info-body"; rc=$(RUN_SRV="$T/srv-ver" run "200" --verify-only); rm -f "$T/info-body"
check "app-versions: and passes when the service announces the config's values" '[ "$rc" = 0 ]'
grep -v '^MIN_APP_VERSION' "$SRV/wrangler.smg.toml" > "$T/srv-ver/wrangler.smg.toml"
RUNN=66; rc=$(RUN_SRV="$T/srv-ver" run "200" --verify-only)
check "app-versions: a config without MIN_APP_VERSION is refused before any request" '[ "$rc" = 1 ] && ! grep -qs "^curl" "$T/stub.log"'
# admin check on workers.dev as well, in the normal run too
RUNN=67; rc=$(run "200" --skip-tests --set-admin-token)
check "admin-both: the normal run checks admin stats on the custom domain AND on workers.dev" '[ "$rc" = 0 ] && grep -q "curl.* https://galaxy.psvita.dev/v1/admin/stats" "$T/stub.log" && grep -q "curl.* https://smg-online.example.workers.dev/v1/admin/stats" "$T/stub.log"'
RUNN=68; rc=$(echo 404 > "$T/admin-default.next"; DEPLOY_ADMIN_WAIT_SEC=0 run "200" --verify-only)
check "admin-both: custom domain 200 but workers.dev never sees the secret: the run FAILS" '[ "$rc" = 1 ] && grep -q "smg-online.example.workers.dev admin check: still 404" "$T/log68/summary.txt"'
RUNN=69; rc=$(run "200 401" --verify-only)
check "admin-both: custom 200, workers.dev 401: fails (a different token on the two addresses)" '[ "$rc" = 1 ] && grep -q "workers.dev admin check: 401" "$T/log69/summary.txt"'
RUNN=70; rc=$(run "401" --verify-only)
check "admin-both: a 401 on the custom domain does not go on to workers.dev (one failure for the brake, not two)" '[ "$rc" = 1 ] && [ "$(cat "$T/admin-n")" = 1 ]'
# the game's own User-Agent
RUNN=71; rc=$(run "200" --verify-only)
check "user-agent: GET /v1/info with User-Agent GalaxyVita is checked on the custom domain" '[ "$rc" = 0 ] && grep -q "with User-Agent GalaxyVita: 200" "$T/log71/summary.txt"'
# kept reply bodies and the console never hold the token (a server that echoes it)
RUNN=72; echo "{\"proto\":1,\"game\":\"smg\",\"echo\":\"$SECRET\"}" > "$T/info-body"; rc=$(run "200" --verify-only); rm -f "$T/info-body"
check "req-scrub: a reply that echoes the admin token leaves no copy in the kept files or the console" '! grep -rq "$SECRET" "$T/log72" "$T/out72"'
# nothing is written (no token file) before the account pin held
RUNN=73; rm -f "$T/newtok73"; rc=$(STUB_WHOAMI_ID=ffffffffffffffffffffffffffffffff; export STUB_WHOAMI_ID; (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/newtok73" --log-dir "$T/log73" --new-admin-token --skip-tests) > "$T/out73" 2>&1; echo $?)
check "write-order: --new-admin-token with a wrong account creates NO token file" '[ "$rc" = 1 ] && [ ! -e "$T/newtok73" ] && ! grep -E -qs "secret put|wrangler deploy " "$T/stub.log"'
# ---- 15 Nov (second review, svcfix): items 1, 2, 6, 7, 8, 9, 10
mres=""; mok=""; mdet=""
# item 10: the token substitution is a LITERAL one (awk index/substr, token via the environment): glob characters, backslash, & in a token are not special
eval "$(sed -n '/^scrub() {/,/^}/p' "$SRV/scripts/deploy-smg-online.sh")"
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
globfile() { printf '%s\n' "$1" > "$T/globtok"; chmod 600 "$T/globtok"; (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/globtok" --log-dir "$T/logglob" --verify-only) > "$T/outglob" 2>&1; echo $?; }
check "token file: a token holding glob characters (* ? [ &) is refused by the script's own charset check" '[ "$(globfile "tok*aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && grep -q "characters other than" "$T/outglob" && [ "$(globfile "tok?aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && [ "$(globfile "tok[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ] && [ "$(globfile "tok&aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")" = 1 ]'

# item 8: a token file that group or others can read or write is refused; created files are 600 and folders 700
for m in 640 604 660 666 620 602 644 777; do cp "$T/token" "$T/tokm"; chmod $m "$T/tokm"; rm -f "$T/stub.log"
  rcm=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/tokm" --log-dir "$T/logm" --verify-only) > "$T/outm" 2>&1; echo $?)
  mres="$mres$rcm"; [ -z "$(grep -s "^curl" "$T/stub.log")" ] || mres="${mres}C"; grep -q "only its owner can read and write" "$T/outm" || mres="${mres}M"; done
check "token-mode: a token file readable or writable by group or others (640 604 660 666 620 602 644 777) is refused, and no request is made" '[ "$mres" = 11111111 ]'
for m in 600 400 700; do cp "$T/token" "$T/tokm"; chmod $m "$T/tokm"; rcm=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/tokm" --log-dir "$T/logm$m" --verify-only) > "$T/outm" 2>&1; echo $?); mok="$mok$rcm"; done
check "token-mode: owner-only modes (600, 400, 700) are accepted" '[ "$mok" = 000 ]'
ln -sf "$T/token" "$T/toklink"; chmod 644 "$T/token"; rcm=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/toklink" --log-dir "$T/logm2" --verify-only) > "$T/outm" 2>&1; echo $?); chmod 600 "$T/token"
check "token-mode: a symlink to a group-readable file is refused too (the target's mode counts)" '[ "$rcm" = 1 ] && grep -q "only its owner" "$T/outm"'
check "file-modes: every file the script wrote is mode 600 and every folder mode 700 (umask 077)" '[ -z "$(find "$T/log40" -type f ! -perm 600)" ] && [ -z "$(find "$T/log40" -type d ! -perm 700)" ] && [ -s "$T/log40/summary.txt" ]'

# item 9: the config is refused unless its top-level name is smg-online and workers_dev is true (the other game's config is wrangler.toml)
cfgcase() { # $1 dir name, $2 sed program, $3 text appended (may be empty)
  mkdir -p "$T/srv-$1/scripts"; cp "$SRV/scripts/deploy-smg-online.sh" "$T/srv-$1/scripts/"
  sed "$2" "$SRV/wrangler.smg.toml" > "$T/srv-$1/wrangler.smg.toml"; printf '%s' "$3" >> "$T/srv-$1/wrangler.smg.toml"; }
cfgcase msc 's/^name = "smg-online"/name = "msc-online"/' ""
RUNN=80; rc=$(RUN_SRV="$T/srv-msc" run "200" --verify-only)
check "config: a top-level name other than smg-online (the Strikers Worker msc-online) is refused before anything is contacted" '[ "$rc" = 1 ] && grep -q "does not name the worker smg-online" "$T/out80" && [ ! -s "$T/stub.log" ]'
cfgcase pre 's/^name = "smg-online"/name = "smg-online-2"/' ""
RUNN=81; rc=$(RUN_SRV="$T/srv-pre" run "200" --verify-only)
check "config: a name that only starts with smg-online is refused" '[ "$rc" = 1 ] && grep -q "does not name the worker smg-online" "$T/out81"'
cfgcase tbl '/^name = "smg-online"/d' '
[extra]
name = "smg-online"
'
RUNN=82; rc=$(RUN_SRV="$T/srv-tbl" run "200" --verify-only)
check "config: a name = \"smg-online\" inside a [table], with no top-level name, is refused" '[ "$rc" = 1 ] && grep -q "does not name the worker smg-online" "$T/out82"'
cfgcase wd '/^workers_dev/d' ""
RUNN=83; rc=$(RUN_SRV="$T/srv-wd" run "200" --verify-only)
check "config: a config without workers_dev is refused" '[ "$rc" = 1 ] && grep -q "does not set workers_dev to true or false" "$T/out83"'
cfgcase wdt '/^workers_dev/d' '
[extra]
workers_dev = true
'
RUNN=84; rc=$(RUN_SRV="$T/srv-wdt" run "200" --verify-only)
check "config: workers_dev = true only inside a [table] is refused" '[ "$rc" = 1 ] && grep -q "does not set workers_dev to true or false" "$T/out84"'
cfgcase wdn 's/^workers_dev = true.*/workers_dev = maybe/' ""
RUNN=97; rc=$(RUN_SRV="$T/srv-wdn" run "200" --verify-only)
check "config: workers_dev that is not a boolean is refused" '[ "$rc" = 1 ] && grep -q "does not set workers_dev to true or false" "$T/out97"'
cfgcase wdc 's/^workers_dev = true.*/workers_dev = true   # on/' ""
RUNN=85; rc=$(RUN_SRV="$T/srv-wdc" run "200" --verify-only)
check "config: workers_dev = true with a trailing comment is accepted" '[ "$rc" = 0 ]'

# item 7: --new-admin-token writes its file only after every check that can refuse
RUNN=86; rc=$(STUB_VITEST_FAIL=1 run "200" --new-admin-token --admin-token-file "$T/newtok86")
check "new-token-order: failing local tests: refused, NO token file" '[ "$rc" = 1 ] && [ ! -e "$T/newtok86" ] && grep -q "tests: FAILED" "$T/log86/summary.txt"'
RUNN=87; rc=$(STUB_DRY_URL=https://smg-online.other.workers.dev run "200" --skip-tests --new-admin-token --admin-token-file "$T/newtok87")
check "new-token-order: a wrong address in the dry run: refused, NO token file, no secret" '[ "$rc" = 1 ] && [ ! -e "$T/newtok87" ] && ! grep -q "secret put" "$T/stub.log"'
RUNN=88; rc=$(STUB_DEPLOY_HOST="  galaxy.psvita.dev" run "200" --skip-tests --new-admin-token --admin-token-file "$T/newtok88")
check "new-token-order: a deploy that reports a psvita.dev name: refused, NO token file, no secret" '[ "$rc" = 1 ] && [ ! -e "$T/newtok88" ] && ! grep -q "secret put" "$T/stub.log"'
RUNN=89; printf 'x' > "$T/newtok89"; rc=$(run "200" --skip-tests --new-admin-token --admin-token-file "$T/newtok89")
check "new-token-order: an existing token file is refused up front (nothing deployed) and left untouched" '[ "$rc" = 1 ] && [ "$(cat "$T/newtok89")" = x ] && ! grep -q "wrangler deploy " "$T/stub.log"'
rm -f "$T/stdin."* "$T/newtok89"
RUNN=90; rc=$(run "200" --skip-tests --new-admin-token --admin-token-file "$T/newtok90"); NEWTOK=$(head -1 "$T/newtok90" 2>/dev/null)
check "new-token-order: a passing run writes the file (mode 600, a valid token), sets the same value as the secret, and prints it nowhere" '[ "$rc" = 0 ] && [ "$(ls -l "$T/newtok90" | cut -c1-10)" = "-rw-------" ] && [ ${#NEWTOK} -ge 32 ] && printf "%s" "$NEWTOK" | grep -qE "^[A-Za-z0-9._~+/=-]+$" && [ "$(cat "$T"/stdin.* | tr -d "\n")" = "$NEWTOK" ] && ! grep -rqF "$NEWTOK" "$T/log90" "$T/out90" "$T/stub.log" "$T/tmp"'
check "new-token-order: the file is written after the deploy and right before the secret is put (the summary order shows it)" '[ "$(grep -n "wrangler deploy: ok" "$T/log90/summary.txt" | head -1 | cut -d: -f1)" -lt "$(grep -n "new admin token was written" "$T/log90/summary.txt" | head -1 | cut -d: -f1)" ] && [ "$(grep -n "new admin token was written" "$T/log90/summary.txt" | head -1 | cut -d: -f1)" -lt "$(grep -n "wrangler secret-put: ok" "$T/log90/summary.txt" | head -1 | cut -d: -f1)" ]'

# item 2: the plan lists exactly the requests a run makes (the tie: plan lines == requests seen by the curl stub)
planlines() { (cd "${PLAN_SRV:-$SRV}" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" "$@" --print-plan) 2>&1 | sed -n -E 's/^   (check|prove): (GET|POST) (https:[^ ]*) User-Agent: ([^ ]*) admin-token: (yes|no)$/\2|\3|\4|\5/p' | sort -u; }
seenlines() { sort -u "$T/reqs"; }
RUNN=91; rc=$(run "200" --verify-only); planv=$(planlines --verify-only); seen=$(seenlines)
check "plan-tie: --verify-only: the plan's check lines equal the requests the run made (URL, User-Agent, admin token), and include the GalaxyVita and the admin checks on EVERY address" '[ "$rc" = 0 ] && [ -n "$planv" ] && [ "$planv" = "$seen" ] && echo "$planv" | grep -q "^GET|https://galaxy.psvita.dev/v1/info|GalaxyVita|no$" && echo "$planv" | grep -q "^GET|https://galaxy.psvita.dev/v1/admin/stats|smg-deploy-check|yes$" && echo "$planv" | grep -q "^GET|https://smg-online.example.workers.dev/v1/admin/stats|smg-deploy-check|yes$" && [ "$(echo "$planv" | wc -l | tr -d " ")" = 11 ]'
RUNN=92; rc=$(run "200" --skip-tests --set-admin-token); planf=$(planlines --skip-tests --set-admin-token); seen=$(seenlines)
check "plan-tie: the full deploy run: the same equality" '[ "$rc" = 0 ] && [ -n "$planf" ] && [ "$planf" = "$seen" ]'
check "wrangler.toml: the package keeps it (the core free-tier test reads it), and every wrangler call of a full deploy run carries --config wrangler.smg.toml" '[ -f "$SRV/wrangler.toml" ] && ! grep -q "NO CONFIG" "$T/stub.log" && [ "$(grep -c "npx --no wrangler" "$T/stub.log")" = "$(grep "npx --no wrangler" "$T/stub.log" | grep -c "wrangler.*--config wrangler.smg.toml")" ] && [ "$(grep -c "npx --no wrangler" "$T/stub.log")" -ge 4 ]'
RUNN=93; rm -f "$T/stub.log" "$T/reqs"; (cd "$T/srv-off" && bash scripts/deploy-smg-online.sh --account-id $ACCT --admin-token-file "$T/token" --log-dir "$T/log93" --verify-only) > "$T/out93" 2>&1; rc=$?
plann=$( (cd "$T/srv-off" && bash scripts/deploy-smg-online.sh --account-id $ACCT --admin-token-file "$T/token" --verify-only --print-plan) 2>&1 | sed -n -E 's/^   (check|prove): (GET|POST) (https:[^ ]*) User-Agent: ([^ ]*) admin-token: (yes|no)$/\2|\3|\4|\5/p' | sort -u)
check "plan-tie: workers_dev = false, no flag (custom domain only): the same equality" '[ "$rc" = 0 ] && [ -n "$plann" ] && [ "$plann" = "$(seenlines)" ] && ! echo "$plann" | grep -q workers.dev'
# the tie really detects drift: a script whose run no longer makes the GalaxyVita request, or no longer checks admin, differs from its own plan
for mut in 'REQ_UA=GalaxyVita req' 'admin_check "$tag"'; do
  mkdir -p "$T/srv-mut/scripts"; cp "$SRV/wrangler.smg.toml" "$T/srv-mut/"; grep -vF "$mut" "$SRV/scripts/deploy-smg-online.sh" > "$T/srv-mut/scripts/deploy-smg-online.sh"
  RUNN=94; rc=$(RUN_SRV="$T/srv-mut" run "200" --verify-only); pm=$(PLAN_SRV="$T/srv-mut" planlines --verify-only); sm=$(seenlines)
  if [ "$pm" != "$sm" ]; then mdet="$mdet+"; else mdet="$mdet-"; fi
done
check "plan-tie: the tie detects drift (a run without the GalaxyVita request, or without the admin checks, differs from its plan)" '[ "$mdet" = "++" ]'

# items 1 and 6: raw remote text reaches neither the console nor a kept file, and never sits under the log folder
rawdir_ok() { awk -v t="$T/tmp/" 'index($1, t) != 1 || $2 != "drwx------" { bad = 1 } END { exit bad }' "$T/seen/rawdir"; }
RUNN=95; printf '%s' '{"proto":1,"game":"smg","note":"rawleak-marker@example.com 9f8e7d6c5b4a39281706f5e4d3c2b1a0 truncated' > "$T/info-body"; rm -f "$T/seen/rawdir"; rc=$(STUB_LEAK=1 run "200" --verify-only); rm -f "$T/info-body"
check "leak: a 200 reply with a truncated body, plus headers and a curl diagnostic holding an e-mail and a 32-hex run: none of it in the console or any kept file" '[ "$rc" = 1 ] && ! grep -rqE "rawleak-marker|9f8e7d6c5b4a39281706f5e4d3c2b1a0" "$T/out95" "$T/log95"'
check "leak: the scrubbed copies ARE kept (body, headers and curl diagnostic hold [EMAIL]; the id shows as its last 4)" 'grep -q "\[EMAIL\]" "$T/log95"/*custom-info.body && grep -q "\[EMAIL\]" "$T/log95"/*custom-info.headers && grep -q "\[EMAIL\]" "$T/log95"/*custom-info.curl-error && grep -q "\*\*\*\*b1a0" "$T/log95"/*custom-info.body'
check "raw-temp: curl wrote raw replies only into a private (mode 700) temp dir under TMPDIR, never under a log folder; the dir is gone after the run" '[ -s "$T/seen/rawdir" ] && rawdir_ok && ! grep -q "/log" "$T/seen/rawdir" && [ -z "$(ls -A "$T/tmp")" ]'
check "leak-window: at no moment did a kept file hold raw remote text while the scrubber ran (the old raw-then-scrub-in-place order would)" '[ ! -e "$T/leak.log" ]'
# a SIGTERM in the middle of a request: the trap still removes the raw temp dir, and nothing raw is in the logs
mkdir -p "$T/bin-kill"
cat > "$T/bin-kill/curl" <<S
#!/bin/bash
"$T/bin/curl" "\$@"; rc=\$?
IFS= read -r deploy_pid < "$T/deploy-pid" || exit 1
[[ "\$deploy_pid" =~ ^[1-9][0-9]*\$ ]] || exit 1
kill -TERM "\$deploy_pid" || exit 1
/bin/sleep 2
exit \$rc
S
chmod +x "$T/bin-kill/curl"
RUNN=96; rc=$(PATH="$T/bin-kill:$PATH" STUB_LEAK=1 RUN_PID_FILE="$T/deploy-pid" run "200" --verify-only)
check "kill: SIGTERM during a request ends the run (exit 143), removes the raw temp dir, leaves no raw text under the log folder" '[ "$rc" = 143 ] && [ -z "$(ls -A "$T/tmp")" ] && ! grep -rq "rawleak-marker" "$T/log96"'
check "no leftovers: the temp area is empty after every run of this test, and the scrubber never saw raw text in a log folder" '[ -z "$(ls -A "$T/tmp")" ] && [ ! -e "$T/leak.log" ]'
# node is a required tool (the checks parse JSON with it)
if ! PATH="$T/bin:/usr/bin:/bin" command -v node >/dev/null 2>&1; then
  RUNN=98; rc=$( (cd "$SRV" && PATH="$T/bin:/usr/bin:/bin" "$(command -v bash)" scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/log98" --verify-only) > "$T/out98" 2>&1; echo $?)
  check "tools: a missing node is refused with a clear message before anything is contacted" '[ "$rc" = 1 ] && grep -q "node is needed" "$T/out98"'
else echo "ok   tools: (skipped: node is also in /usr/bin here) a missing node is refused with a clear message"; NOK=$((NOK + 1)); fi
check "portability: no GNU- or Linux-only form (the forms listed in portability-check.sh) in scripts/*.sh or test/deploy/*" 'portability_scan "$SRV" > "$T/port.out" 2>&1 || { cat "$T/port.out"; false; }'
check "deploy plan wording: the next admin check is skipped after 'any answer other than 200, 404 or none'; the header no longer claims notifications; the comment about the token does not claim 'no environment'" 'echo "$planv" >/dev/null; (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --print-plan) | grep -q "any answer other than 200, 404 or none" && ! grep -q "401, 403 or 429" "$SRV/scripts/deploy-smg-online.sh" && ! grep -q "rate-limiting rule, notifications" "$SRV/scripts/deploy-smg-online.sh" && ! grep -q "another process.s environment" "$SRV/scripts/deploy-smg-online.sh"'
# ---- 15 Nov (round f, amendment to part D): the relay. Secret NAMES from `wrangler secret list`, the half-set-up refusal, --require-relay,
# --turn-key-file / --turn-usage-token-file, TURN_ACCOUNT_ID, and the required relay self-check (POST /v1/admin/turn-selfcheck)
KID=abcdef0123456789abcdef0123456789
KTOK="keytok_$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')"
UTOK="usetok_$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')"
printf '%s\n%s\n' "$KID" "$KTOK" > "$T/tkey"; chmod 600 "$T/tkey"
printf '%s\n' "$UTOK" > "$T/tuse"; chmod 600 "$T/tuse"
SC_OK_ABSENT='200 {"mint":"ok","revoke":"ok","usage":"absent","galaxyMonthMb":null,"accountMonthMb":null,"tripped":false}'
SC_OK_USAGE='200 {"mint":"ok","revoke":"ok","usage":"ok","galaxyMonthMb":1234,"accountMonthMb":5678,"tripped":false}'
sc() { printf '%s\n' "$1" > "$T/sc-default"; rm -f "$T/sc-seq"; }
sc_clear() { rm -f "$T/sc-default" "$T/sc-seq"; }
NOLEAK='! grep -rqF -e "$KTOK" -e "$UTOK" -e "$KID" "$T"/log2* "$T"/out2* "$T/stub.log" "$T/tmp" "$T/seen/curl-args" "$T/seen/curl-env" 2>/dev/null'
SEC2="TURN_KEY_ID TURN_KEY_API_TOKEN"
SEC3="TURN_KEY_ID TURN_KEY_API_TOKEN TURN_USAGE_TOKEN"
SEC4="TURN_KEY_ID TURN_KEY_API_TOKEN TURN_USAGE_TOKEN TURN_ACCOUNT_ID"
# the secret list is read with --config, before the tests, the dry run and the deploy
RUNN=200; rc=$(run "200" --verify-only)
check "relay-names: wrangler secret list --config wrangler.smg.toml is called (names only), also in --verify-only" '[ "$rc" = 0 ] && grep -q "secret list --config wrangler.smg.toml" "$T/stub.log" && grep -q "secrets by name: TURN_KEY_ID absent, TURN_KEY_API_TOKEN absent, TURN_USAGE_TOKEN absent, TURN_ACCOUNT_ID absent" "$T/log200/summary.txt"'
check "relay-off: neither key secret: 'relay: off (no TURN key secrets)' and the run does NOT fail; no self-check request" 'grep -q "relay: off (no TURN key secrets)" "$T/log200/summary.txt" && ! grep -q "turn-selfcheck" "$T/stub.log"'
RUNN=201; rc=$(STUB_SECRETS="TURN_KEY_ID" run "200" --skip-tests)
check "relay-half: TURN_KEY_ID only: refused before the deploy, with the exact wording" '[ "$rc" = 1 ] && grep -q "relay half set up: TURN_KEY_ID present, TURN_KEY_API_TOKEN missing" "$T/out201" && ! grep -E -q "deploy --dry-run|wrangler deploy |secret put|^curl" "$T/stub.log"'
RUNN=202; rc=$(STUB_SECRETS="TURN_KEY_API_TOKEN" run "200" --skip-tests)
check "relay-half: TURN_KEY_API_TOKEN only: refused before the deploy, with the exact wording" '[ "$rc" = 1 ] && grep -q "relay half set up: TURN_KEY_API_TOKEN present, TURN_KEY_ID missing" "$T/out202" && ! grep -E -q "deploy --dry-run|wrangler deploy |secret put|^curl" "$T/stub.log"'
RUNN=203; rc=$(STUB_SECRETS="ADMIN_TOKEN TURN_USAGE_TOKEN" run "200" --verify-only --require-relay)
check "require-relay: no key secrets: refused (also in --verify-only), before any live request" '[ "$rc" = 1 ] && grep -q "relay: off (no TURN key secrets) and --require-relay is given: refusing before the deploy" "$T/out203" && ! grep -q "^curl" "$T/stub.log"'
RUNN=204; rc=$(run "200" --skip-tests --require-relay --set-admin-token)
check "require-relay: refused before the dry run, the deploy and the admin secret" '[ "$rc" = 1 ] && ! grep -E -q "deploy --dry-run|wrangler deploy |secret put" "$T/stub.log"'
RUNN=205; sc "$SC_OK_ABSENT"; rc=$(STUB_SECRETS="$SEC2" run "200" --verify-only --require-relay); sc_clear
check "relay-on (by name, --require-relay satisfied): verify-only runs the self-check as ONE POST, passes with usage absent, and says hard stop: off" '[ "$rc" = 0 ] && [ "$(grep -c "^curl.*turn-selfcheck" "$T/stub.log")" = 1 ] && grep -q "relay: on (TURN_KEY_ID and TURN_KEY_API_TOKEN present as secrets): the relay self-check is required" "$T/log205/summary.txt" && grep -q "relay self-check: PASS (mint ok, revoke ok, usage absent) after 1 tries" "$T/log205/summary.txt" && grep -q "relay: on, hard stop: off (no TURN_USAGE_TOKEN: only the estimate caps limit the relay)" "$T/log205/summary.txt" && ! grep -E -q "secret put|wrangler deploy " "$T/stub.log"'
check "relay self-check: a POST (curl -X POST), admin token on stdin, on the custom domain only, kept request shows [REDACTED]" 'grep -q "curl.* -X POST .*https://galaxy.psvita.dev/v1/admin/turn-selfcheck" "$T/stub.log" && ! grep -q "workers.dev/v1/admin/turn-selfcheck" "$T/stub.log" && grep -q "^POST /v1/admin/turn-selfcheck   Authorization: Bearer \[REDACTED\]$" "$T"/log205/*custom-turn-selfcheck-try1.request && grep -q "^POST|https://galaxy.psvita.dev/v1/admin/turn-selfcheck|smg-deploy-check|yes$" "$T/reqs"'
RUNN=206; sc "$SC_OK_USAGE"; rc=$(STUB_SECRETS="$SEC4" run "200" --verify-only); sc_clear
check "relay-on with the usage token: usage ok is required and the summary line is 'relay: on, hard stop: on (Galaxy month X MB, account month Y MB)'" '[ "$rc" = 0 ] && grep -q "relay self-check: PASS (mint ok, revoke ok, usage ok)" "$T/log206/summary.txt" && grep -q "^\[[0-9:]*\] relay: on, hard stop: on (Galaxy month 1234 MB, account month 5678 MB)$" "$T/log206/summary.txt"'
RUNN=207; sc "$SC_OK_ABSENT"; rc=$(STUB_SECRETS="$SEC4" DEPLOY_ADMIN_WAIT_SEC=0 run "200" --verify-only); sc_clear
check "relay-on with the usage token: usage absent is NOT a pass (the Worker does not see the usage token/account yet): FAIL" '[ "$rc" = 1 ] && grep -q "relay self-check: FAIL (still not ready after 0 s" "$T/log207/summary.txt" && grep -q "relay: on, hard stop NOT confirmed" "$T/log207/summary.txt"'
RUNN=208; rc=$(STUB_SECRETS="$SEC3" run "200" --verify-only)
check "verify-only: TURN_USAGE_TOKEN without TURN_ACCOUNT_ID fails the self-check with that reason (a normal run sets the account id)" '[ "$rc" = 1 ] && grep -q "relay self-check: FAIL: TURN_USAGE_TOKEN exists but TURN_ACCOUNT_ID does not" "$T/log208/summary.txt" && ! grep -q "turn-selfcheck" "$T/stub.log"'
RUNN=209; sc '200 {"mint":"error","revoke":"skipped","usage":"absent","mintDetail":"HTTP 400: unknown field customIdentifier","tripped":false}'; rc=$(STUB_SECRETS="$SEC2" run "200" --verify-only); sc_clear
check "relay self-check fail: mint error is a FAIL at once (not retried), and the service's own reason is shown" '[ "$rc" = 1 ] && [ "$(cat "$T/sc-n")" = 1 ] && grep -q "relay self-check: FAIL (HTTP 200: mint error, revoke skipped, usage absent; reason: HTTP 400: unknown field customIdentifier; stopped on try 1" "$T/log209/summary.txt"'
RUNN=232; sc '200 {"mint":"ok","revoke":"ok","usage":"absent","galaxyMonthMb":null,"accountMonthMb":null,"tripped":false,"untagged":true}'; rc=$(STUB_SECRETS="$SEC2" run "200" --verify-only); sc_clear
check "relay self-check: 'mint ok (untagged)' (the relay API refused the tag) is a PASS, with that wording" '[ "$rc" = 0 ] && grep -q "relay self-check: PASS (mint ok (untagged), revoke ok, usage absent) after 1 tries" "$T/log232/summary.txt"'
RUNN=210; sc '200 {"mint":"ok","revoke":"error","usage":"absent","revokeDetail":"HTTP 400: bad revoke","tripped":false}'; rc=$(STUB_SECRETS="$SEC2" run "200" --verify-only); sc_clear
check "relay self-check fail: revoke error is a FAIL" '[ "$rc" = 1 ] && grep -q "mint ok, revoke error, usage absent; reason: HTTP 400: bad revoke" "$T/log210/summary.txt"'
RUNN=211; sc '200 {"mint":"ok","revoke":"ok","usage":"error","usageDetail":"analytics answered with errors","tripped":false}'; rc=$(STUB_SECRETS="$SEC4" run "200" --verify-only); sc_clear
check "relay self-check fail: usage error (a wrong analytics field) is a FAIL with its reason, not retried" '[ "$rc" = 1 ] && [ "$(cat "$T/sc-n")" = 1 ] && grep -q "usage error; reason: analytics answered with errors" "$T/log211/summary.txt"'
RUNN=212; sc '200 {"mint":"ok","revoke":"ok","usage":"ok","galaxyMonthMb":1,"accountMonthMb":2,"tripped":true}'; rc=$(STUB_SECRETS="$SEC4" run "200" --verify-only); sc_clear
check "relay self-check fail: a tripped hard stop is a FAIL (no new passes are being issued)" '[ "$rc" = 1 ] && grep -q "hard stop TRIPPED" "$T/log212/summary.txt"'
RUNN=213; printf '%s\n' '404 {}' '200 {"mint":"off","revoke":"skipped","usage":"absent","tripped":false}' "$SC_OK_ABSENT" > "$T/sc-seq"; rc=$(STUB_SECRETS="$SEC2" run "200" --verify-only); sc_clear
check "relay self-check: 404 (route/secrets not visible yet) and mint off are retried; the third try passes" '[ "$rc" = 0 ] && grep -q "relay self-check: PASS (mint ok, revoke ok, usage absent) after 3 tries" "$T/log213/summary.txt"'
RUNN=214; printf '%s\n' '404 {}' > "$T/sc-default"; rc=$(STUB_SECRETS="$SEC2" DEPLOY_ADMIN_WAIT_SEC=0 run "200" --verify-only); sc_clear
check "relay self-check: still 404 after the wait: FAIL" '[ "$rc" = 1 ] && grep -q "relay self-check: FAIL (still not ready after 0 s" "$T/log214/summary.txt"'
RUNN=215; rc=$(STUB_SECRETS="$SEC2" run "401" --verify-only)
check "relay self-check: not run (and a FAIL) when the admin check on the custom domain did not pass" '[ "$rc" = 1 ] && grep -q "relay self-check: FAIL: not run, because the admin check" "$T/log215/summary.txt" && ! grep -q "turn-selfcheck" "$T/stub.log"'
RUNN=216; rc=$(STUB_SECRETS_FAIL="Could not do the thing" run "200" --verify-only)
check "secret-list failure: verify-only refuses ('cannot read the secret names'); the e-mail in the error is scrubbed" '[ "$rc" = 1 ] && grep -q "cannot read the secret names" "$T/out216" && ! grep -rq "owner@example.com" "$T/log216" "$T/out216"'
RUNN=217; rc=$(STUB_SECRETS_FAIL="Worker not found. [code: 10007]" run "200" --skip-tests)
check "secret-list: a worker that does not exist yet (10007) is treated as no secrets in a deploying run: relay off, the run goes on" '[ "$rc" = 0 ] && grep -q "the worker has no secrets yet" "$T/log217/summary.txt" && grep -q "relay: off (no TURN key secrets)" "$T/log217/summary.txt"'
# --- the files
RUNN=220; sc "$SC_OK_ABSENT"; rc=$(run "200" --skip-tests --turn-key-file "$T/tkey" --set-admin-token); sc_clear
check "turn-key-file: relay on from the file; TURN_KEY_ID and TURN_KEY_API_TOKEN are put with --config, AFTER the deploy and the admin secret, and the self-check passes" '[ "$rc" = 0 ] && grep -q "secret put TURN_KEY_ID --config wrangler.smg.toml" "$T/stub.log" && grep -q "secret put TURN_KEY_API_TOKEN --config wrangler.smg.toml" "$T/stub.log" && grep -q "relay: on (TURN_KEY_ID and TURN_KEY_API_TOKEN present from --turn-key-file)" "$T/log220/summary.txt" && grep -q "relay self-check: PASS" "$T/log220/summary.txt"'
check "turn-key-file: the values went to wrangler on STDIN only (the id and the token, each alone), never in an argument, a log, the console or a temp file" 'grep -lxF "$KID" "$T"/stdin.* >/dev/null && grep -lxF "$KTOK" "$T"/stdin.* >/dev/null && ! grep -q -e "$KID" -e "$KTOK" "$T/stub.log" && '"$NOLEAK"
check "turn-key-file: console wording of the secret steps" 'grep -q "secret set: TURN_KEY_ID (from --turn-key-file; value not printed)" "$T/log220/summary.txt" && grep -q "secret set: TURN_KEY_API_TOKEN (from --turn-key-file; value not printed)" "$T/log220/summary.txt"'
check "turn-key-file: order in the summary: deploy, admin secret, TURN_KEY_ID, TURN_KEY_API_TOKEN, then the live checks and the self-check" 'lnum() { grep -n "$1" "$T/log220/summary.txt" | head -1 | cut -d: -f1; }; [ "$(lnum "wrangler deploy: ok")" -lt "$(lnum "wrangler secret-put: ok")" ] && [ "$(lnum "wrangler secret-put: ok")" -lt "$(lnum "secret set: TURN_KEY_ID")" ] && [ "$(lnum "secret set: TURN_KEY_ID")" -lt "$(lnum "secret set: TURN_KEY_API_TOKEN")" ] && [ "$(lnum "secret set: TURN_KEY_API_TOKEN")" -lt "$(lnum "GET /v1/info: 200")" ] && [ "$(lnum "GET /v1/info: 200")" -lt "$(lnum "relay self-check: PASS")" ]'
check "turn-key-file: no TURN_ACCOUNT_ID without a usage token" '! grep -q "secret put TURN_ACCOUNT_ID" "$T/stub.log" && ! grep -q "secret put TURN_USAGE_TOKEN" "$T/stub.log"'
RUNN=221; sc "$SC_OK_USAGE"; rc=$(run "200" --skip-tests --turn-key-file "$T/tkey" --turn-usage-token-file "$T/tuse"); sc_clear
check "turn-usage-token-file: TURN_USAGE_TOKEN from the file on stdin, and TURN_ACCOUNT_ID = the account id on stdin; usage must be ok; summary line with the numbers" '[ "$rc" = 0 ] && grep -q "secret put TURN_USAGE_TOKEN --config wrangler.smg.toml" "$T/stub.log" && grep -q "secret put TURN_ACCOUNT_ID --config wrangler.smg.toml" "$T/stub.log" && grep -lxF "$UTOK" "$T"/stdin.* >/dev/null && grep -lxF "$ACCT" "$T"/stdin.* >/dev/null && grep -q "secret set: TURN_USAGE_TOKEN (from --turn-usage-token-file; value not printed)" "$T/log221/summary.txt" && grep -q "secret set: TURN_ACCOUNT_ID (the account id from --account-id; every run; value not printed)" "$T/log221/summary.txt" && grep -q "relay: on, hard stop: on (Galaxy month 1234 MB, account month 5678 MB)" "$T/log221/summary.txt"'
check "turn-usage-token-file: the usage token is in no log, console, argument or temp file" '! grep -rqF -e "$UTOK" -e "$KTOK" "$T/log221" "$T/out221" "$T/stub.log" "$T/tmp" "$T/seen/curl-args" "$T/seen/curl-env"'
rm -f "$T"/stdin.*
RUNN=231; sc "$SC_OK_USAGE"; rc=$(STUB_ECHO_SECRET=1 run "200" --skip-tests --turn-key-file "$T/tkey" --turn-usage-token-file "$T/tuse"); sc_clear
check "a wrangler that echoes every secret it is given (key id, key token, usage token) leaves no value in any log or the console: [TOKEN] shows instead" '[ "$rc" = 0 ] && ! grep -rqF -e "$UTOK" -e "$KTOK" -e "$KID" "$T/log231" "$T/out231" && grep -rq "\[TOKEN\]" "$T/log231"'
RUNN=222; sc "$SC_OK_USAGE"; rc=$(STUB_SECRETS="$SEC3" run "200" --skip-tests); sc_clear
check "TURN_USAGE_TOKEN present by name: TURN_ACCOUNT_ID is set from --account-id on stdin on this run (and no other TURN secret is touched)" '[ "$rc" = 0 ] && grep -q "secret put TURN_ACCOUNT_ID --config wrangler.smg.toml" "$T/stub.log" && [ "$(grep -c "secret put" "$T/stub.log")" = 1 ] && grep -lxF "$ACCT" "$T"/stdin.* >/dev/null'
RUNN=223; rc=$(STUB_SECRETS="$SEC3" STUB_DEPLOY_HOST="  galaxy.psvita.dev" run "200" --skip-tests)
check "TURN_ACCOUNT_ID is put only after every check that can refuse: a deploy that reports a psvita.dev name puts nothing" '[ "$rc" = 1 ] && ! grep -q "secret put" "$T/stub.log"'
RUNN=224; rc=$(STUB_SECRETS="$SEC3" STUB_VITEST_FAIL=1 run "200")
check "TURN_ACCOUNT_ID: failing local tests: nothing deployed, nothing put" '[ "$rc" = 1 ] && ! grep -E -q "wrangler deploy |secret put" "$T/stub.log"'
RUNN=225; sc "$SC_OK_ABSENT"; rc=$(STUB_SECRETS="$SEC2" run "200" --skip-tests); sc_clear
check "relay on without a usage token: no TURN_ACCOUNT_ID put" '[ "$rc" = 0 ] && ! grep -q "secret put" "$T/stub.log"'
# --- refusals of the files
for m in 644 640 660 604; do cp "$T/tkey" "$T/tkm"; chmod $m "$T/tkm"; rm -f "$T/stub.log"; rcm=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logtk$m" --skip-tests --turn-key-file "$T/tkm") > "$T/outtk" 2>&1; echo $?); mres2="${mres2:-}$rcm"; grep -q "the TURN key file must be a regular file that only its owner can read and write (mode 600)" "$T/outtk" || mres2="${mres2}M"; grep -E -q "wrangler deploy |secret put|^curl" "$T/stub.log" && mres2="${mres2}W"; done
check "turn-key-file mode: a file group/others can read or write (644 640 660 604) is refused before any deploy, secret or request" '[ "$mres2" = 1111 ]'
cp "$T/tuse" "$T/tum"; chmod 640 "$T/tum"; rm -f "$T/stub.log"; rcm=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logtu" --skip-tests --turn-key-file "$T/tkey" --turn-usage-token-file "$T/tum") > "$T/outtu" 2>&1; echo $?)
check "turn-usage-token-file mode: 640 is refused before any deploy or secret" '[ "$rcm" = 1 ] && grep -q "the TURN usage token file must be a regular file that only its owner" "$T/outtu" && ! grep -E -q "wrangler deploy |secret put" "$T/stub.log"'
badkey() { printf "$1" > "$T/tbad"; chmod 600 "$T/tbad"; rm -f "$T/stub.log"; (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logbad" --skip-tests --turn-key-file "$T/tbad") > "$T/outbad" 2>&1; echo $?; }
check "turn-key-file format: a key id that is not 32 hex, a missing second line, a short token and a third line are each refused, without echoing the file's text, and nothing is deployed" '[ "$(badkey "notahexid-SECRETLINE\n$KTOK\n")" = 1 ] && grep -q "line 1 of the TURN key file must be the TURN key id (32 hex characters)" "$T/outbad" && ! grep -q "SECRETLINE" "$T/outbad" && [ "$(badkey "$KID\n")" = 1 ] && grep -q "line 2 of the TURN key file" "$T/outbad" && [ "$(badkey "$KID\nshort\n")" = 1 ] && [ "$(badkey "$KID\n$KTOK\nextra\n")" = 1 ] && grep -q "exactly two lines" "$T/outbad" && ! grep -E -q "wrangler deploy |secret put" "$T/stub.log"'
printf 'abc\n' > "$T/tshort"; chmod 600 "$T/tshort"; rcm=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logsh" --skip-tests --turn-key-file "$T/tkey" --turn-usage-token-file "$T/tshort") > "$T/outsh" 2>&1; echo $?)
check "turn-usage-token-file format: a short or malformed token is refused" '[ "$rcm" = 1 ] && grep -q "the TURN usage token file must hold one line" "$T/outsh"'
RUNN=226; rc=$(run "200" --verify-only --turn-key-file "$T/tkey")
check "verify-only: --turn-key-file / --turn-usage-token-file are refused (it sets no secret)" '[ "$rc" = 1 ] && grep -q "verify-only sets no secret" "$T/out226" && [ "$(run "200" --verify-only --turn-usage-token-file "$T/tuse")" = 1 ]'
RUNN=227; rc=$(run "200" --skip-tests --turn-usage-token-file "$T/tuse")
check "turn-usage-token-file without any TURN key secrets is refused before the deploy" '[ "$rc" = 1 ] && grep -q -- "--turn-usage-token-file without any TURN key secrets" "$T/out227" && ! grep -E -q "deploy --dry-run|wrangler deploy |secret put" "$T/stub.log"'
RUNN=228; rc=$(STUB_SECRETS="TURN_KEY_ID" run "200" --skip-tests --turn-key-file "$T/tkey")
check "turn-key-file completes a half-set relay by name (both come from the file): not refused" '[ "$rc" = 0 ] && grep -q "secret put TURN_KEY_API_TOKEN" "$T/stub.log"'
# --- scrub of several values
multi() { local got; got=$( TOK='a*b'; TK_TOKEN='k.t+k'; TU_TOKEN='u/t=u'; TK_ID=$KID; printf '%s\n' "x a*b y k.t+k z u/t=u w $KID v ${KID}ff" | scrub ); [ "$got" = "x [TOKEN] y [TOKEN] z [TOKEN] w [TOKEN] v [TOKEN]ff" ]; }
check "scrub-multi: the admin token, the key id, the key's API token and the usage token are each replaced literally everywhere" 'multi'
# --- the plan
planr=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --print-plan) 2>&1)
planrk=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --turn-key-file "$T/tkey-none" --print-plan) 2>&1)
check "plan: lists secret list, the relay rules, and the self-check as 'check-if-relay-on:' (relay unknown) or 'check:' (with --turn-key-file); never any file content" 'echo "$planr" | grep -q "npx --no wrangler secret list --config wrangler.smg.toml" && echo "$planr" | grep -q "^   check-if-relay-on: POST https://galaxy.psvita.dev/v1/admin/turn-selfcheck User-Agent: smg-deploy-check admin-token: yes$" && ! echo "$planr" | grep -q "^   check: POST" && echo "$planrk" | grep -q "^   check: POST https://galaxy.psvita.dev/v1/admin/turn-selfcheck User-Agent: smg-deploy-check admin-token: yes$" && echo "$planrk" | grep -q "secret put TURN_KEY_ID --config" && echo "$planrk" | grep -q "secret put TURN_ACCOUNT_ID --config" && [ ! -e "$T/tkey-none" ]'
RUNN=229; rm -f "$T/reqs"; sc "$SC_OK_ABSENT"; rc=$(run "200" --skip-tests --turn-key-file "$T/tkey"); sc_clear; planrt=$(planlines --skip-tests --turn-key-file "$T/tkey"); seen=$(seenlines)
check "plan-tie with the relay on: the plan's check lines (the POST self-check included) equal the requests the run made" '[ "$rc" = 0 ] && [ -n "$planrt" ] && [ "$planrt" = "$seen" ] && echo "$planrt" | grep -q "^POST|https://galaxy.psvita.dev/v1/admin/turn-selfcheck|smg-deploy-check|yes$" && [ "$(echo "$planrt" | wc -l | tr -d " ")" = 12 ]'
RUNN=230; rm -f "$T/reqs"; sc "$SC_OK_ABSENT"; rc=$(STUB_SECRETS="$SEC2" run "200" --verify-only); sc_clear; planrn=$(planlines --verify-only; echo "POST|https://galaxy.psvita.dev/v1/admin/turn-selfcheck|smg-deploy-check|yes"); seen=$(seenlines)
check "plan-tie with the relay on by name: the plan's lines plus the conditional self-check equal the requests the run made" '[ "$rc" = 0 ] && [ "$(echo "$planrn" | sort -u)" = "$seen" ]'
check "relay: the relay lines never hold a secret value" '! grep -rqF -e "$KTOK" -e "$UTOK" "$T"/log2* "$T"/out2* "$T/stub.log"'
rm -f "$T"/stdin.*
planp=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --print-plan) 2>&1)
planpv=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --verify-only --print-plan) 2>&1)
check "whoami: carries --config wrangler.smg.toml in the plan too" 'echo "$planp" | grep -q "npx --no wrangler whoami --json --config wrangler.smg.toml"'

# ---- --strict, the split-deployment refusal, the failed-deploy output
RUNN=270; rc=$(run "200" --skip-tests --set-admin-token)
check "strict: the real deploy carries --strict, the dry run does not; every wrangler call is 'npx --no'; vitest runs with NODE_ENV=test" '[ "$rc" = 0 ] && grep -q "npx --no wrangler deploy --strict --config wrangler.smg.toml" "$T/stub.log" && grep "deploy --dry-run" "$T/stub.log" | grep -qv -- "--strict" && ! grep -E "npx (wrangler|vitest)" "$T/stub.log"'
RUNN=271; rc=$(run "200" --set-admin-token); 
check "vitest: NODE_ENV=test is set for the local suites" '[ "$rc" = 0 ] && grep -q "nodeenv=test npx --no vitest run --project core --project worker-smg" "$T/stub.log"'
RUNN=272; rc=$(STUB_DEPLOY_STRICT=1 run "200" --skip-tests --set-admin-token)
check "strict abort: wrangler's refusal is shown from its first [ERROR]; the line says NOTHING WAS UPLOADED and not to remove --strict; no secret, no live check" '[ "$rc" = 1 ] && grep -q "wrangler refused under --strict (scrubbed, from its first \[ERROR\])" "$T/out272" && grep -q "Aborting the upload operation because of conflicts" "$T/out272" && grep -q "wrangler deploy ABORTED under --strict: the live Worker has changes this package does not have (a dashboard edit or another deploy). NOTHING WAS UPLOADED. STOP: report this message; the owner decides; do not remove --strict." "$T/out272" && ! grep -q "secret put" "$T/stub.log" && ! grep "^curl" "$T/stub.log" | grep -q .'
RUNN=273; rc=$(STUB_DEPLOY_VERSIONS=2 run "200" --skip-tests --set-admin-token)
check "split deployment (2 versions): refused before the dry run and the deploy" '[ "$rc" = 1 ] && grep -q "the current deployment of smg-online is split or cannot be read (versions: 2)" "$T/out273" && ! grep -E -q "deploy --dry-run|wrangler deploy |secret put" "$T/stub.log"'
RUNN=274; rc=$(STUB_DEPLOY_VERSIONS=none run "200" --skip-tests --set-admin-token)
check "first deploy (no deployment yet, code 10007): the split check passes" '[ "$rc" = 0 ] && grep -q "wrangler deployments-status: no deployment yet" "$T/log274/summary.txt"'
RUNN=275; rc=$(STUB_DEPLOY_VERSIONS=junk run "200" --skip-tests)
check "split check: output that is not the expected JSON is 'cannot be read': refused" '[ "$rc" = 1 ] && grep -q "split or cannot be read (versions: unknown)" "$T/out275"'
RUNN=276; rc=$(STUB_DEPLOY_VERSIONS=fail run "200" --skip-tests)
check "split check: a failed 'deployments status' (other than not found): refused, and its text (an e-mail) is kept nowhere" '[ "$rc" = 1 ] && grep -q "cannot read the current deployment" "$T/out276" && ! grep -rq "owner@example.com" "$T/log276" "$T/out276"'
check "deployments-status failure: the message names the status file (not another step's), which keeps the scrubbed reason, 5 lines at most; nothing raw anywhere" '[ "$rc" = 1 ] && grep -q "(see .*-wrangler-deployments-status.txt)" "$T/out276" && f=$(ls "$T"/log276/*-wrangler-deployments-status.txt) && grep -q "^FAILED: wrangler deployments status exit code 1" "$f" && grep -q "stderr-reason-1 \[EMAIL\] \[ip\]" "$f" && grep -q "stderr-reason-5" "$f" && ! grep -q "stderr-reason-6" "$f" && ! grep -rqE "owner@example.com|203.0.113.7" "$T/log276" "$T/out276"'
check "split check: the deployments status output is not stored (only a count line)" 'grep -q "^OK: the current deployment has 1 version" "$T"/log270/*-wrangler-deployments-status.txt && ! grep -rq "author_email\|version_id" "$T/log270"'
RUNN=277; rc=$(STUB_SETTINGS_BLOCK=1 run "200" --skip-tests --set-admin-token)
check "account block: wrangler's 'Getting User settings...' block is dropped from its first line on (no e-mail, account name or id; the marker after it too)" '[ "$rc" = 0 ] && grep -q "^\[wrangler account block removed\]$" "$T"/log277/*-wrangler-deploy.txt && ! grep -rq "Hat Co\|afterblock-marker\|Getting User settings" "$T/log277" "$T/out277"'
# ---- round g: PROOF BEFORE THE TOKEN. The admin token goes only to an address whose /v1/prove answer is the HMAC of the token for that host name.
noadmin() { ! grep -q "|yes$" "$T/reqs" && [ ! -s "$T/seen/hdr-stdin" ]; }
RUNN=278; echo '{"proto":1,"game":"msc","app":{"min":"3.0.0","latest":"3.0.0"}}' > "$T/info-body"; rm -f "$T/seen/hdr-stdin"; rc=$(run "200" --verify-only); rm -f "$T/info-body"
check "prove: /v1/info is only a public check (a wrong game fails the run) and a good proof still gets the token only where the proof matched: ok lines for both addresses" '[ "$rc" = 1 ] && grep -q "GET /v1/info: 200 but the answer says" "$T/log278/summary.txt" && grep -q "https://galaxy.psvita.dev prove: ok (the service holds the admin token; host bound)" "$T/log278/summary.txt" && grep -q "https://smg-online.example.workers.dev prove: ok (the service holds the admin token; host bound)" "$T/log278/summary.txt"'
RUNN=500; rm -f "$T/seen/hdr-stdin"; rc=$(run "200" --verify-only)
check "prove: a service that proves correctly gets the admin requests on BOTH addresses, each preceded by its own prove request with a fresh 32-hex nonce (the token only in the stdin config line)" '[ "$rc" = 0 ] && grep -q "^GET|https://galaxy.psvita.dev/v1/prove?n=<random-hex32>|smg-deploy-check|no$" "$T/reqs" && grep -q "^GET|https://galaxy.psvita.dev/v1/admin/stats|smg-deploy-check|yes$" "$T/reqs" && grep -q "^GET|https://smg-online.example.workers.dev/v1/prove?n=<random-hex32>|smg-deploy-check|no$" "$T/reqs" && grep -q "^GET|https://smg-online.example.workers.dev/v1/admin/stats|smg-deploy-check|yes$" "$T/reqs" && [ "$(grep -c "curl .*v1/prove?n=" "$T/stub.log")" = 2 ] && [ "$(grep "curl.*v1/prove?n=" "$T/stub.log" | sed -E "s/.*n=([0-9a-f]{32}).*/\1/" | sort -u | wc -l)" -eq 2 ] && grep -q "^header = \"Authorization: Bearer $SECRET\"$" "$T/seen/hdr-stdin"'
proveorder() { local h p a; for h in galaxy.psvita.dev smg-online.example.workers.dev; do p=$(grep -n "$h/v1/prove" "$T/reqs" | head -1 | cut -d: -f1); a=$(grep -n "$h/v1/admin/stats" "$T/reqs" | head -1 | cut -d: -f1); { [ -n "$p" ] && [ -n "$a" ] && [ "$p" -lt "$a" ]; } || return 1; done; }
check "prove: the order is prove, then admin, per address (the first admin request of each address comes after that address's prove request)" 'proveorder'
check "prove: neither the token nor any expected or returned proof is in a log, the console or an argument; the kept prove replies hold the proof masked" '! grep -rqE "$SECRET" "$T/log500" "$T/out500" "$T/seen/curl-args" && ! grep -rqE "\"proof\":\"[0-9a-f]{64}\"" "$T/log500" "$T/out500" && ls "$T/log500" | grep -q "custom-prove-try1.body"'
provefail=""
for mode in wrong otherhost nokey 404 noproof badshape upper infocopy 429 500; do
  RUNN=501; rm -f "$T/seen/hdr-stdin"; rc=$(STUB_PROVE=$mode DEPLOY_ADMIN_WAIT_SEC=2 run "200" --verify-only)
  case $mode in 404) want="still HTTP 404 after 2 s" ;; 429) want="HTTP 429" ;; 500) want="HTTP 500" ;; *) want="" ;; esac
  if [ "$rc" = 1 ] && grep -q "prove: FAILED (" "$T/log501/summary.txt" && grep -q "): the admin token is not sent there" "$T/log501/summary.txt" && [ -z "$(grep "|yes$" "$T/reqs")" ] && [ ! -s "$T/seen/hdr-stdin" ] && ! grep -rq "$SECRET" "$T/stub.log" "$T/log501" "$T/out501" && { [ -z "$want" ] || grep -q "$want" "$T/log501/summary.txt"; }; then :; else provefail="$provefail $mode"; fi
done
check "prove: a service that answers a wrong proof, a proof for another host, a proof made with another key, 404, no proof, a malformed or upper-case proof, a copy of /v1/info, 429 or 500 gets NO admin request (no admin line in the requests the stub saw, nothing on stdin, the token nowhere)${provefail:+ (failed:$provefail)}" '[ -z "$provefail" ]'
RUNN=502; rm -f "$T/seen/hdr-stdin"; rc=$(STUB_PROVE_WD=wrong run "200" --verify-only)
check "prove: one address that cannot prove (workers.dev) gets NO admin request while the other (the custom domain) that proves does; the run fails" '[ "$rc" = 1 ] && grep -q "^GET|https://galaxy.psvita.dev/v1/admin/stats|smg-deploy-check|yes$" "$T/reqs" && ! grep -q "workers.dev/v1/admin/stats" "$T/reqs" && grep -q "https://smg-online.example.workers.dev prove: FAILED (HTTP 200 but the proof does not match: this address is not bound to the service that holds the admin token; see " "$T/log502/summary.txt" && grep -q "): the admin token is not sent there" "$T/log502/summary.txt"'
RUNN=503; printf '404\n000\n404\nok\n' > "$T/prove-seq"; rc=$(run "200" --verify-only); rm -f "$T/prove-seq"
check "prove: 404 or no answer (the secret is not there yet) is retried within the admin wait, then the proof passes and the admin check goes on (no admin request before the proof)" '[ "$rc" = 0 ] && grep -q "https://galaxy.psvita.dev prove: ok" "$T/log503/summary.txt" && [ "$(grep -c "galaxy.psvita.dev/v1/prove" "$T/reqs")" -ge 1 ] && [ "$(grep -c "curl .*galaxy.psvita.dev/v1/prove" "$T/stub.log")" = 4 ]'
RUNN=504; rm -f "$T/seen/hdr-stdin"; rc=$(STUB_PROVE=wrong run "200" --skip-tests --set-admin-token)
check "prove: after a deploy and the secret put, a service that cannot prove still gets no admin request, and the run fails" '[ "$rc" = 1 ] && [ -z "$(grep "|yes$" "$T/reqs")" ] && [ ! -s "$T/seen/hdr-stdin" ]'
RUNN=505; SEC5="TURN_KEY_ID TURN_KEY_API_TOKEN"; rm -f "$T/seen/hdr-stdin"; rc=$(STUB_SECRETS="$SEC5" STUB_PROVE=ok run "200" --verify-only)
check "prove: the relay self-check is gated the same way: its own prove request (fresh nonce) comes before the POST, on the custom domain" '[ "$rc" = 0 ] && [ "$(grep -c "curl .*galaxy.psvita.dev/v1/prove?n=" "$T/stub.log")" = 2 ] && p=$(grep -n "galaxy.psvita.dev/v1/prove" "$T/reqs" | tail -1 | cut -d: -f1) && a=$(grep -n "POST|https://galaxy.psvita.dev/v1/admin/turn-selfcheck" "$T/reqs" | head -1 | cut -d: -f1) && [ "$p" -lt "$a" ]'
RUNN=506; rm -f "$T/seen/hdr-stdin"; printf 'ok\nok\nwrong\n' > "$T/prove-seq"; rc=$(STUB_SECRETS="$SEC5" run "200" --verify-only); rm -f "$T/prove-seq"
check "prove: a failed proof before the relay self-check (third prove request) means the self-check POST is never sent" '[ "$rc" = 1 ] && ! grep -q "turn-selfcheck" "$T/reqs" && grep -q "prove: FAILED" "$T/log506/summary.txt"'
check "prove: no DNS tool is used or needed (no dig, host or nslookup anywhere in the script), and openssl rand -hex 16 makes the nonce" '! grep -qE "dig \+|nslookup|host -W|dns_tool|DEPLOY_DNS_TOOLS|domain_precheck" "$SRV/scripts/deploy-smg-online.sh" && grep -q "openssl rand -hex 16" "$SRV/scripts/deploy-smg-online.sh"'
check "prove: the token reaches the proof helper through the environment only (PROVE_KEY=...), never an argument" 'grep -q "PROVE_KEY=\$TOK node -e" "$SRV/scripts/deploy-smg-online.sh"'
# ---- environment pinning
envrefuse() { # $1 = VAR=value
  local rcx; rcx=$( (export "$1"; cd "$SRV" && rm -f "$T/stub.log" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logenv" --verify-only) > "$T/outenv" 2>&1; echo $?); echo "$rcx"; }
envbad=""; for kv in NODE_OPTIONS=--require=x CLOUDFLARE_API_BASE_URL=https://example.invalid WRANGLER_API_ENVIRONMENT=staging CLOUDFLARE_API_TOKEN=secretvalue1234 CLOUDFLARE_API_KEY=x CLOUDFLARE_EMAIL=a@example.com WRANGLER_AUTH_DOMAIN=x WRANGLER_CLIENT_ID=x WRANGLER_OUTPUT_FILE_PATH=/tmp/x WRANGLER_LOG_PATH=/tmp/x CLOUDFLARE_ENV=x MSC_HMAC_SOURCE=x WRANGLER_LOG_SANITIZE=false NODE_TLS_REJECT_UNAUTHORIZED=0; do
  [ "$(envrefuse "$kv")" = 1 ] && [ ! -e "$T/stub.log" ] && grep -q "${kv%%=*}" "$T/outenv" && { case "$kv" in WRANGLER_LOG_SANITIZE=*|NODE_TLS_*) true ;; *) ! grep -q "${kv#*=}" "$T/outenv" ;; esac; } || envbad="$envbad ${kv%%=*}"; done
check "env: each of these set refuses the run before ANY wrangler call or request; the message names the variable and never its value${envbad:+ (failed:$envbad)}" '[ -z "$envbad" ]'
rc=$(envrefuse CLOUDFLARE_API_TOKEN=secretvalue1234)
check "env refusal wording" 'grep -q "deploy-smg-online: the environment variable CLOUDFLARE_API_TOKEN is set (its value is not shown): it changes what wrangler talks to, which credentials it uses, where it writes or what the local tests read. Refusing: unset CLOUDFLARE_API_TOKEN and run again" "$T/outenv"'
rc=$(envrefuse HTTPS_PROXY=http://proxy.invalid:3128)
check "env: a proxy is NOT refused; only its NAME is noted" '[ "$rc" = 0 ] && grep -q "proxy / CA / config-home environment is set (names only, not refused; XDG_CONFIG_HOME decides where wrangler finds its login, the account pin below checks the login): HTTPS_PROXY" "$T/outenv" && ! grep -q "proxy.invalid" "$T/outenv" "$T/logenv"/*'
rc=$(envrefuse NODE_EXTRA_CA_CERTS=/tmp/ca.pem)
check "env: a CA bundle is NOT refused; only its NAME is noted" '[ "$rc" = 0 ] && grep -q "names only, not refused; XDG_CONFIG_HOME decides where wrangler finds its login, the account pin below checks the login): NODE_EXTRA_CA_CERTS" "$T/outenv"'
rc=$( (cd "$SRV" && DEPLOY_OFFLINE_TEST= DEPLOY_CUSTOM_WAIT_SEC=5 bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logenv" --verify-only) > "$T/outenv" 2>&1; echo $?)
check "env: a shortened DEPLOY_CUSTOM_WAIT_SEC is refused outside the offline test suite" '[ "$rc" = 1 ] && grep -q "DEPLOY_CUSTOM_WAIT_SEC is shorter than the default 600 s" "$T/outenv"'
rc=$( (cd "$SRV" && DEPLOY_OFFLINE_TEST= DEPLOY_ADMIN_WAIT_SEC=5 bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --log-dir "$T/logenv" --verify-only) > "$T/outenv" 2>&1; echo $?)
check "env: a shortened DEPLOY_ADMIN_WAIT_SEC is refused outside the offline test suite" '[ "$rc" = 1 ] && grep -q "DEPLOY_ADMIN_WAIT_SEC is shorter than the default 120 s" "$T/outenv"'
mkdir -p "$T/srv-env/scripts"; cp "$SRV/wrangler.smg.toml" "$T/srv-env/"; cp "$SRV/scripts/deploy-smg-online.sh" "$T/srv-env/scripts/"; echo "CLOUDFLARE_API_TOKEN=x" > "$T/srv-env/.env"
RUNN=279; rc=$(RUN_SRV="$T/srv-env" run "200" --verify-only)
check "env: a .env file in server/ is refused (wrangler loads it), before any wrangler call" '[ "$rc" = 1 ] && grep -q "server/.env or server/.env.local exists: wrangler loads those files, and they can set any of the variables refused above. Refusing: move it away and run again" "$T/out279" && [ ! -s "$T/stub.log" ]'
rm -f "$T/srv-env/.env"; echo "X=1" > "$T/srv-env/.env.local"
RUNN=280; rc=$(RUN_SRV="$T/srv-env" run "200" --verify-only)
check "env: a .env.local file is refused too" '[ "$rc" = 1 ] && grep -q "server/.env or server/.env.local exists" "$T/out280"'
rm -f "$T/srv-env/.env.local"
mkdir -p "$T/srv-wv/scripts" "$T/srv-wv/node_modules/wrangler"; cp "$SRV/scripts/deploy-smg-online.sh" "$T/srv-wv/scripts/"; cp "$SRV/wrangler.smg.toml" "$T/srv-wv/"; printf '{\n  "name": "wrangler",\n  "version": "4.0.1",\n}\n' > "$T/srv-wv/node_modules/wrangler/package.json"
RUNN=281; rc=$(RUN_SRV="$T/srv-wv" run "200" --verify-only)
check "wrangler version: anything but the pinned 4.156.0 is refused before wrangler runs" '[ "$rc" = 1 ] && grep -q "node_modules/wrangler is version .4.0.1., not the pinned 4.156.0" "$T/out281" && [ ! -s "$T/stub.log" ]'
# ---- --secret-list and --put-secret
RUNN=282; rc=$(STUB_SECRETS="ADMIN_TOKEN TURN_KEY_ID" srun --secret-list)
check "secret-list: prints the names only and exits 0; nothing deployed, no live request" '[ "$rc" = 0 ] && grep -q "secret list (names only): ADMIN_TOKEN TURN_KEY_ID" "$T/out282" && ! grep -E -q "deploy |secret put|^curl" "$T/stub.log"'
RUNN=283; rc=$(STUB_SECRETS_FAIL='Worker "smg-online" not found. [code: 10007]' srun --secret-list)
check "secret-list: a Worker that does not exist is refused (never treated as 'no secrets')" '[ "$rc" = 1 ] && grep -q "the Worker smg-online does not exist or its secrets cannot be read" "$T/out283"'
PUTV="putval_$(head -c 18 /dev/urandom | od -An -tx1 | tr -d ' \n')"
rm -f "$T/stdin."*; RUNN=284; rc=$(printf '%s\n' "$PUTV" | STUB_SECRETS="ADMIN_TOKEN" srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: the value goes to wrangler on STDIN; the console says 'secret put: NAME ok (value not printed)'; the value is in no log, output, argument or temp file; nothing deployed" '[ "$rc" = 0 ] && grep -q "^\[[0-9:]*\] secret put: TURN_USAGE_TOKEN ok (value not printed)$" "$T/log284/summary.txt" && grep -q "secret put: TURN_USAGE_TOKEN ok (value not printed)" "$T/out284" && [ "$(cat "$T"/stdin.* | tr -d "\n")" = "$PUTV" ] && ! grep -rqF "$PUTV" "$T/log284" "$T/out284" "$T/stub.log" "$T/tmp" && grep -q "secret put TURN_USAGE_TOKEN --config wrangler.smg.toml" "$T/stub.log" && ! grep -E -q "wrangler deploy |^curl" "$T/stub.log"'
rm -f "$T/stdin."*; RUNN=285; rc=$(printf '%s\n' "$PUTV" | STUB_PUT_FAIL=1 STUB_SECRETS="ADMIN_TOKEN" srun --put-secret TURN_KEY_API_TOKEN)
check "put-secret failure: 'secret put: NAME FAILED (value not printed)', exit 1, value and e-mail nowhere" '[ "$rc" = 1 ] && grep -q "secret put: TURN_KEY_API_TOKEN FAILED (value not printed)" "$T/out285" && ! grep -rqF "$PUTV" "$T/log285" "$T/out285" && ! grep -rq "owner@example.com" "$T/log285" "$T/out285"'
RUNN=286; rc=$(printf '%s\n' "$PUTV" | srun --put-secret ADMIN_TOKEN)
check "put-secret: a name outside the allowlist (ADMIN_TOKEN) is refused, before any wrangler call" '[ "$rc" = 1 ] && grep -q "allows only TURN_USAGE_TOKEN, TURN_ACCOUNT_ID, TURN_KEY_ID and TURN_KEY_API_TOKEN" "$T/out286" && [ ! -s "$T/stub.log" ]'
RUNN=287; rc=$(printf '%s\n' "ffffffffffffffffffffffffffffffff" | srun --put-secret TURN_ACCOUNT_ID)
check "put-secret: TURN_ACCOUNT_ID must equal --account-id exactly" '[ "$rc" = 1 ] && grep -q "TURN_ACCOUNT_ID: the value must equal --account-id exactly" "$T/out287" && ! grep -q "secret put" "$T/stub.log"'
rm -f "$T/stdin."*; RUNN=288; rc=$(printf '%s\n' "$ACCT" | srun --put-secret TURN_ACCOUNT_ID)
check "put-secret: TURN_ACCOUNT_ID equal to --account-id is accepted; the id is shown nowhere in full" '[ "$rc" = 0 ] && grep -q "secret put: TURN_ACCOUNT_ID ok (value not printed)" "$T/out288" && ! grep -rq "$ACCT" "$T/log288" "$T/out288"'
RUNN=289; rc=$(printf '%s\n%s\n' "$PUTV" "second" | srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: two lines on stdin are refused" '[ "$rc" = 1 ] && grep -q "holds more than one line" "$T/out289"'
RUNN=290; rc=$(printf '' | srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: an empty stdin is refused" '[ "$rc" = 1 ] && grep -q "the value on stdin is empty" "$T/out290"'
RUNN=291; rc=$(printf '%s\n' "short" | srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: a value that is not token-shaped is refused, without echoing it" '[ "$rc" = 1 ] && grep -q "must be one token of at least 20" "$T/out291" && ! grep -q "short$" "$T/out291"'
RUNN=292; rc=$(printf '%s\n' "$PUTV" | STUB_SECRETS_FAIL='Worker "smg-online" not found. [code: 10007]' srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: refused when secret list does not prove the Worker exists (secret put would create a stub Worker); nothing is put" '[ "$rc" = 1 ] && grep -q "refusing; --put-secret would CREATE a stub Worker" "$T/out292" && ! grep -q "secret put" "$T/stub.log"'
RUNN=293; rc=$(srun --put-secret TURN_USAGE_TOKEN --secret-list)
check "put-secret and --secret-list exclude each other" '[ "$rc" = 1 ] && grep -q "exclude each other" "$T/out293"'
rm -f "$T/stdin."*
# ---- M20: only wrangler's own wording for a missing Worker is 'no secrets yet'
RUNN=294; rc=$(STUB_SECRETS_FAIL='no such page, not found in the cache' run "200" --skip-tests)
check "secret names: an unrelated 'not found' is NOT 'the Worker has no secrets': refused" '[ "$rc" = 1 ] && grep -q "cannot read the secret names" "$T/out294"'
RUNN=295; rc=$(STUB_SECRETS_FAIL='Worker "smg-online" not found. [code: 10007]' run "200" --skip-tests --set-admin-token)
check "secret names: wrangler's own 'Worker \"smg-online\" not found [code: 10007]' is a first deploy: treated as none" '[ "$rc" = 0 ] && grep -q "the worker has no secrets yet" "$T/log295/summary.txt"'
# ---- M13 self-check ignores quoted strings; M17 scrub rules
cp "$SRV/scripts/deploy-smg-online.sh" "$T/quoted.sh"; printf '\necho "npx wrangler deploy is only text here"\n' >> "$T/quoted.sh"
check "self-check: wrangler inside a quoted string is not a call" 'bash "$T/quoted.sh" --self-check >/dev/null 2>&1'
scrubcases() {
  local got
  got=$( TOK=; printf 'a %s b\n' "$(printf 'g%.0s' $(seq 1 40))" | scrub ); [ "$got" = "a [token] b" ] || return 1
  got=$( TOK=; printf 'v 1a2b3c4d-1234-5678-9abc-def012345678 w\n' | scrub ); [ "$got" = "v 1a2b3c4d-1234-5678-9abc-def012345678 w" ] || return 1
  got=$( TOK=; printf 'p /Users/alice/x /home/bob/y\n' | scrub ); [ "$got" = "p ~/x ~/y" ] || return 1
  got=$( TOK=; printf 'ctl\001\002x\033[0m end\n' | scrub ); [ "$got" = "ctlx[0m end" ] || return 1
  got=$( TOK=; printf 'id 0123456789abcdef0123456789abcdef12 z\n' | scrub ); [ "$got" = "id [id ****ef12] z" ] || return 1
  got=$( TOK=; PUT_VALUE=putsecretvalue123456789; printf 'x putsecretvalue123456789 y\n' | scrub ); [ "$got" = "x [TOKEN] y" ] || return 1
}
check "scrub: a 40+ character token-shaped run, home paths, control characters, long hex and the --put-secret value (the 36-character dashed version id is kept)" 'scrubcases'
cfgvariant() { # name, sed expression: a copy of the service dir whose wrangler.smg.toml is edited by the expression
  mkdir -p "$T/srv-$1"; cp -r "$SRV/scripts" "$T/srv-$1/scripts"; ln -sfn "$SRV/node_modules" "$T/srv-$1/node_modules"
  sed "$2" "$SRV/wrangler.smg.toml" > "$T/srv-$1/wrangler.smg.toml"
}
RUNN=401; cfgvariant obsoff 's/^enabled = true/enabled = false/'; rc=$(RUN_SRV="$T/srv-obsoff" run "200" --verify-only)
check "config: Workers Logs off ([observability] enabled = false) is refused before anything runs" '[ "$rc" = 1 ] && grep -q "\[observability\] must set enabled = true" "$T/out401" && ! grep -q "wrangler" "$T/stub.log"'
RUNN=402; cfgvariant obssample 's/^head_sampling_rate = 1/head_sampling_rate = 0.5/'; rc=$(RUN_SRV="$T/srv-obssample" run "200" --verify-only)
check "config: a head sampling rate under 1 is refused" '[ "$rc" = 1 ] && grep -q "must set head_sampling_rate = 1" "$T/out402"'
RUNN=403; cfgvariant obsinv 's/^invocation_logs = false/invocation_logs = true/'; rc=$(RUN_SRV="$T/srv-obsinv" run "200" --verify-only)
check "config: invocation logs on (they carry request URLs) are refused" '[ "$rc" = 1 ] && grep -q "\[observability.logs\] must set invocation_logs = false" "$T/out403"'
RUNN=404; cfgvariant d1bind 's/^binding = "REPORTS_DB"/binding = "DB"/'; rc=$(RUN_SRV="$T/srv-d1bind" run "200" --verify-only)
check "config: a D1 binding not named REPORTS_DB is refused" '[ "$rc" = 1 ] && grep -q "must set binding = \"REPORTS_DB\"" "$T/out404"'
RUNN=405; cfgvariant d1name 's/^database_name = "galaxy-beta-reports"/database_name = "other"/'; rc=$(RUN_SRV="$T/srv-d1name" run "200" --verify-only)
check "config: another D1 database name is refused" '[ "$rc" = 1 ] && grep -q "must set database_name = \"galaxy-beta-reports\"" "$T/out405"'
RUNN=406; cfgvariant d1id 's/^database_id = "272224ff-51e9-4ad0-8575-a5ab999e8358"/database_id = "00000000-0000-0000-0000-000000000000"/'; rc=$(RUN_SRV="$T/srv-d1id" run "200" --verify-only)
check "config: another D1 database id is refused" '[ "$rc" = 1 ] && grep -q "must set database_id" "$T/out406"'
RUNN=407; cfgvariant nod1 '/^\[\[d1_databases\]\]/,/^database_id/d'; rc=$(RUN_SRV="$T/srv-nod1" run "200" --verify-only)
check "config: no D1 binding at all is refused" '[ "$rc" = 1 ] && grep -q "exactly ONE \[\[d1_databases\]\]" "$T/out407"'
RUNN=408; cfgvariant noobs '/^\[observability/,/^invocation_logs/d'; rc=$(RUN_SRV="$T/srv-noobs" run "200" --verify-only)
check "config: no [observability] block is refused" '[ "$rc" = 1 ] && grep -q "must set enabled = true" "$T/out408"'
pl_cfg=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --print-plan) 2>&1 | grep "^config (checked before anything runs)")
check "config: the plan names the observability keys and the D1 binding" 'echo "$pl_cfg" | grep -q "head_sampling_rate = 1" && echo "$pl_cfg" | grep -q "invocation_logs = false" && echo "$pl_cfg" | grep -q "REPORTS_DB = galaxy-beta-reports"'
# ---- round e (package k): stderr noise, secret-mode headers and plans, more environment variables, deployment source, DNS and wording, provisioning, traces, scrub, npm
RUNN=420; rc=$(HTTPS_PROXY=http://proxy.invalid:3128 STUB_DEPLOY_STDERR=1 run "200" --skip-tests --set-admin-token)
check "deployments status: a proxy variable and stderr warnings (a WARNING line, a Node warning) do not break the JSON read: the deploy goes on, stderr is not stored" '[ "$rc" = 0 ] && grep -q "wrangler deployments-status: ok, current deployment has 1 version(s), source: wrangler (output not stored)" "$T/log420/summary.txt" && ! grep -rqE "owner@example.com|Proxy environment" "$T/log420"'
RUNN=421; rc=$(STUB_DEPLOY_STDERR=1 run "200" --verify-only)
check "deployments status: --verify-only reads it too (stderr noise ignored) and prints the source" '[ "$rc" = 0 ] && grep -q "current deployment has 1 version(s), source: wrangler" "$T/log421/summary.txt"'
RUNN=422; rc=$(STUB_DEPLOY_SOURCE=api run "200" --set-admin-token)
check "source api: a WARNING note only (the deploy's own --strict protects), the run goes on to the tests, dry run and deploy" '[ "$rc" = 0 ] && grep -q "WARNING: the current deployment of smg-online was made through the script API (source: api; only a hint, wrangler.s own record is last_deployed_from): wrangler asks before overwriting an API-edited Worker and --strict aborts instead of asking, so the deploy may abort (then NOTHING WAS UPLOADED); the script goes on" "$T/log422/summary.txt" && grep -q "vitest" "$T/stub.log" && grep -q "wrangler deploy --strict" "$T/stub.log" && ! grep -rq "NOTHING WAS RUN" "$T/out422" "$T/log422"'
RUNN=423; rc=$(STUB_DEPLOY_SOURCE=dash run "200" --skip-tests --set-admin-token)
check "source dash: a warning note, the deploy still goes on (with --strict)" '[ "$rc" = 0 ] && grep -q "WARNING: the current deployment of smg-online was made from the Cloudflare dashboard (source: dash)" "$T/log423/summary.txt" && grep -q "wrangler deploy --strict" "$T/stub.log"'
RUNN=424; rc=$(STUB_DEPLOY_SOURCE=api run "200" --verify-only)
check "source api with --verify-only: the same warning, no refusal (nothing is deployed)" '[ "$rc" = 0 ] && grep -q "WARNING: the current deployment of smg-online was made through the script API (source: api; only a hint, wrangler.s own record is last_deployed_from)" "$T/log424/summary.txt"'
RUNN=425; rc=$(STUB_DEPLOY_SOURCE=weird run "200" --skip-tests --set-admin-token)
check "source: an unknown source value is shown as is, not refused (api and dash only warn)" '[ "$rc" = 0 ] && grep -q "source: weird" "$T/log425/summary.txt"'
# secret modes
RUNN=426; rc=$(STUB_SECRETS="ADMIN_TOKEN" srun --secret-list)
check "secret-list: the header says 'mode: secret-list' and names only the custom domain (no deploy mode, no empty second address)" '[ "$rc" = 0 ] && grep -q "smg-online (custom domain https://galaxy.psvita.dev); account \*\*\*\*cdef; mode: secret-list; replies kept in" "$T/out426" && ! grep -q "mode: deploy" "$T/out426" && ! grep -q "galaxy.psvita.dev  " "$T/out426"'
rm -f "$T/stdin."*; RUNN=427; rc=$(printf '%s\n' "$PUTV" | STUB_SECRETS="ADMIN_TOKEN" srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: the header says 'mode: put-secret TURN_USAGE_TOKEN'" '[ "$rc" = 0 ] && grep -q "mode: put-secret TURN_USAGE_TOKEN; replies kept in" "$T/out427" && ! grep -q "mode: deploy" "$T/out427"'
RUNN=428; rc=$(PUTV2=x srun --secret-list --print-plan); cp "$T/out428" "$T/plan-sl"
check "plan --secret-list: its own short plan (whoami, secret list), no deploy plan, nothing contacted" '[ "$rc" = 0 ] && grep -q "Mode: secret-list, stand-alone" "$T/plan-sl" && grep -q "npx --no wrangler whoami --json" "$T/plan-sl" && grep -q "npx --no wrangler secret list" "$T/plan-sl" && ! grep -qE "dry run|--strict|live checks|wrangler deploy" "$T/plan-sl" && [ ! -s "$T/stub.log" ]'
RUNN=429; rc=$(srun --put-secret TURN_KEY_ID --print-plan); cp "$T/out429" "$T/plan-ps"
check "plan --put-secret: whoami, secret list, then secret put NAME with the value on stdin (never an argument), no deploy plan, nothing contacted, no stdin read" '[ "$rc" = 0 ] && grep -q "Mode: put-secret TURN_KEY_ID, stand-alone" "$T/plan-ps" && grep -q "npx --no wrangler secret put TURN_KEY_ID --config wrangler.smg.toml" "$T/plan-ps" && grep -q "the value is read from stdin only" "$T/plan-ps" && ! grep -qE "dry run|--strict|live checks|wrangler deploy" "$T/plan-ps" && [ ! -s "$T/stub.log" ]'
RUNN=430; rc=$(HOME="$T/home430" STUB_WRITE_WRANGLER_LOG=1 STUB_SECRETS="ADMIN_TOKEN" srun --secret-list)
check "secret-list: a wrangler debug log written during the run fails it (the check also runs before the secret modes exit)" '[ "$rc" != 0 ] && grep -q "debug log file" "$T/log430/summary.txt"'
rm -f "$T/stdin."*; RUNN=431; rc=$(printf '%s\n' "$PUTV" | HOME="$T/home431" STUB_WRITE_WRANGLER_LOG=1 STUB_SECRETS="ADMIN_TOKEN" srun --put-secret TURN_USAGE_TOKEN)
check "put-secret: a wrangler debug log written during the run fails it" '[ "$rc" != 0 ] && grep -q "debug log file" "$T/log431/summary.txt"'
# more environment variables
envbad2=""
for v in CF_API_TOKEN CF_API_KEY CF_EMAIL CF_API_BASE_URL CF_ACCOUNT_ID CLOUDFLARE_AUTH_USE_KEYRING CLOUDFLARE_COMPLIANCE_REGION WRANGLER_CF_AUTHORIZATION_TOKEN CLOUDFLARE_ACCESS_CLIENT_ID CLOUDFLARE_ACCESS_CLIENT_SECRET WRANGLER_BUILD_CONDITIONS WRANGLER_BUILD_PLATFORM WRANGLER_CACHE_DIR WRANGLER_REGISTRY_PATH WRANGLER_TRACE_ID WRANGLER_CI_MATCH_TAG LOG_DIR CLOUDFLARE_CLIENT_ID CLOUDFLARE_BASE_URL CLOUDFLARE_API_USER_SERVICE_KEY CLOUDFLARE_INCLUDE_PROCESS_ENV CLOUDFLARE_LOAD_DEV_VARS_FROM_DOT_ENV CLOUDFLARE_CONTAINER_REGISTRY WRANGLER_CI_GENERATE_PREVIEW_ALIAS WORKERS_CI_BRANCH CF_PAGES_UPLOAD_JWT WRANGLER_BASIN_SQL_AUTH_TOKEN; do
  [ "$(envrefuse "$v=zzvalue-$v")" = 1 ] && [ ! -e "$T/stub.log" ] && grep -q "the environment variable $v is set (its value is not shown)" "$T/outenv" && ! grep -q "zzvalue" "$T/outenv" || envbad2="$envbad2 $v"; done
check "env: each of the further variables wrangler 4.156.0 reads (CF_* names, keyring, compliance region, Access client id and secret, build conditions and platform, cache dir, registry path, trace id, CI tag, LOG_DIR, ...) refuses the run: name shown, value never${envbad2:+ (failed:$envbad2)}" '[ -z "$envbad2" ]'
rc=$(envrefuse XDG_CONFIG_HOME=/tmp/xdgconf)
check "env: XDG_CONFIG_HOME is NOT refused (it decides where wrangler finds its login); its NAME is noted with the proxy and CA names, its value is not" '[ "$rc" = 0 ] && grep -q "config-home environment is set (names only, not refused.*): XDG_CONFIG_HOME" "$T/outenv" && ! grep -q "xdgconf" "$T/outenv" "$T/logenv"/*'
# provisioning and traces
RUNN=434; cfgvariant d1two 's/^database_id = .*/&\n[[d1_databases]]\nbinding = "X"\ndatabase_name = "x"\ndatabase_id = "00000000-0000-0000-0000-000000000000"/'; rc=$(RUN_SRV="$T/srv-d1two" run "200" --verify-only)
check "provisioning: two [[d1_databases]] are refused (exactly ONE, the existing database with its id)" '[ "$rc" = 1 ] && grep -q "must declare exactly ONE \[\[d1_databases\]\]" "$T/out434" && [ ! -s "$T/stub.log" ]'
RUNN=435; cfgvariant d1none '/^\[\[d1_databases\]\]/,/^database_id/d'; rc=$(RUN_SRV="$T/srv-d1none" run "200" --verify-only)
check "provisioning: no D1 binding at all: refused (found 0)" '[ "$rc" = 1 ] && grep -q "found 0" "$T/out435"'
provbad=""; n=436
for res in '[[kv_namespaces]]\nbinding = "K"' '[[r2_buckets]]\nbinding = "R"' '[[queues.producers]]\nbinding = "Q"' '[[vectorize]]\nbinding = "V"' '[[hyperdrive]]\nbinding = "H"' '[[workflows]]\nbinding = "W"' '[[services]]\nbinding = "S"' '[ai]\nbinding = "AI"' 'kv_namespaces = []'; do
  RUNN=$n; cfgvariant prov 's/^KILL_SWITCH = .*/&/'; printf '\n%b\n' "$res" >> "$T/srv-prov/wrangler.smg.toml"
  rc=$(RUN_SRV="$T/srv-prov" run "200" --verify-only); resn=$(printf '%b' "$res" | head -1 | tr -d '[]=' | sed 's/ .*//; s/\..*//')
  [ "$rc" = 1 ] && grep -q "declares '$resn'" "$T/out$n" && [ ! -s "$T/stub.log" ] || provbad="$provbad $resn"; n=$((n+1)); done
check "provisioning: kv_namespaces, r2_buckets, queues, vectorize, hyperdrive, workflows, services, ai and an inline kv_namespaces are each refused before anything runs${provbad:+ (failed:$provbad)}" '[ -z "$provbad" ]'
RUNN=450; cfgvariant trcon '/^\[observability.traces\]/,/^enabled/s/false/true/'; rc=$(RUN_SRV="$T/srv-trcon" run "200" --verify-only)
check "config: traces on is refused" '[ "$rc" = 1 ] && grep -q "\[observability.traces\] must set enabled = false" "$T/out450"'
RUNN=451; cfgvariant trcno '/^\[observability.traces\]/,/^enabled/d'; rc=$(RUN_SRV="$T/srv-trcno" run "200" --verify-only)
check "config: no [observability.traces] block is refused (explicit off)" '[ "$rc" = 1 ] && grep -q "\[observability.traces\] must set enabled = false" "$T/out451"'
# npm notices and scrub IPs
check "npm: update notifier, fund and audit messages are switched off and npm_config_logs_max=0 (no ~/.npm/_logs debug file) for every npx call" '[ -s "$T/seen/npmcfg" ] && ! grep -qv "npmcfg=false/false/false/0" "$T/seen/npmcfg"'
scrubip() {
  local got
  got=$( TOK=; printf 'peer 203.0.113.7:41100 ip_1.2.3.4 x192.168.1.1 1.2.3.4b\n' | scrub ); [ "$got" = "peer [ip]:41100 ip_[ip] x[ip] [ip]b" ] || { echo "v4: $got"; return 1; }
  got=$( TOK=; printf 'a 2001:db8::1 b ::1 c ::ffff:192.0.2.128 d 1:2:3:4:5:6:7:8 e [fe80::1]:80\n' | scrub ); case "$got" in *2001*|*fe80*|*"1:2:3"*|*192.0.2*) echo "v6: $got"; return 1 ;; esac
  got=$( TOK=; printf 'at 12:34:56 on 2026-11-15T10:11:12.123Z version 1a2b3c4d-1234-5678-9abc-def012345678 db 272224ff-51e9-4ad0-8575-a5ab999e8358 wrangler 4.156.0\n' | scrub ); [ "$got" = "at 12:34:56 on 2026-11-15T10:11:12.123Z version 1a2b3c4d-1234-5678-9abc-def012345678 db 272224ff-51e9-4ad0-8575-a5ab999e8358 wrangler 4.156.0" ] || { echo "keep: $got"; return 1; }
}
check "scrub: IPv4 (also glued to letters or underscores) and IPv6 are masked; times, the 36-character version id, the dashed D1 id and version numbers stay" 'scrubip'
# round f: more provisioning / config guards, plan numbering and config line
provbad=""; n=460
for res in '[site]\nbucket = "./x"' '[[tail_consumers]]\nservice = "other"' '[env.staging]\nname = "x"' '[[env.staging.kv_namespaces]]\nbinding = "K"' '[triggers]\ncrons = ["0 * * * *"]' '[["kv_namespaces"]]\nbinding = "K"' "[['r2_buckets']]\nbinding = \"R\"" '"queues" = []' '[[ "d1_databases" ]]\nbinding = "X"'; do
  RUNN=$n; cfgvariant prov2 's/^KILL_SWITCH = .*/&/'; printf '\n%b\n' "$res" >> "$T/srv-prov2/wrangler.smg.toml"
  rc=$(RUN_SRV="$T/srv-prov2" run "200" --verify-only)
  [ "$rc" = 1 ] && grep -qE "declares '|exactly ONE" "$T/out$n" && [ ! -s "$T/stub.log" ] || provbad="$provbad $n"; n=$((n+1)); done
check "provisioning: [site], [[tail_consumers]], [env.*], [triggers], quoted resource keys and a quoted second D1 are each refused before anything runs${provbad:+ (failed run numbers:$provbad)}" '[ -z "$provbad" ]'
RUNN=470; rc=$(run "200" --verify-only)
check "config: the shipped wrangler.smg.toml passes the new guards (no [triggers], [site], [env.*], tail consumers) and sets TURN_NEW_ACCOUNT_DAILY = 200" '[ "$rc" = 0 ] && grep -q "^TURN_NEW_ACCOUNT_DAILY = \"200\"" "$SRV/wrangler.smg.toml"'
planfull=$( (cd "$SRV" && bash scripts/deploy-smg-online.sh --account-id $ACCT --expect-subdomain example --admin-token-file "$T/token" --print-plan --set-admin-token) 2>&1)
check "plan numbering follows the run order: 1 login, 2 secret names, 3 current deployment, 4 tests, 5 dry run, 6 deploy, 7 secrets, 8 live checks; no domain pre-check step" 'o=$(echo "$planfull" | grep -oE "^[0-9]+[a-z]?\. [A-Za-z]+( [a-z]+)?" | tr "\n" "|"); echo "$o" | grep -q "^1. login check|2. read the|3. current deployment|4. local suites|5. dry run|6. the deploy|7. set the|7d. if|8. live checks|$" && ! echo "$planfull" | grep -qE "^(1a|1b|1c)\.|PRE-DEPLOY|pre-check"'
check "plan: a prove line before the admin line of each address; the PROOF BEFORE THE TOKEN explanation; no DNS lookup" 'echo "$planfull" | grep -q "^   prove: GET https://galaxy.psvita.dev/v1/prove?n=<random-hex32> User-Agent: smg-deploy-check admin-token: no$" && echo "$planfull" | grep -q "^   prove: GET https://smg-online.example.workers.dev/v1/prove?n=<random-hex32> User-Agent: smg-deploy-check admin-token: no$" && echo "$planfull" | grep -q "^   PROOF BEFORE THE TOKEN: before EVERY request that carries the admin token" && echo "$planfull" | grep -q "^   prove-if-relay-on: GET https://galaxy.psvita.dev/v1/prove" && ! echo "$planfull" | grep -qE "dig|nslookup|DNS"'
check "plan: step 5 and 6 say wrangler reports no psvita.dev name; the retries line says every 10 s" 'echo "$planfull" | grep -q "^5. dry run .*it must report no psvita.dev name at all (the config has no route and no custom domain: wrangler never publishes them)" && echo "$planfull" | grep -q "^6. the deploy, with --strict .*no psvita.dev name (the custom domain galaxy.psvita.dev was attached once in the Cloudflare dashboard; this deploy does not touch it)" && echo "$planfull" | grep -q "asked again every 10 s while the proof is 404 or no answer"'
check "plan config line names traces off and the one-D1 / no-other-resource guard" 'echo "$planp" | grep -q "\[observability.traces\] enabled = false" && echo "$planp" | grep -q "exactly ONE \[\[d1_databases\]\] and no other resource" && echo "$planp" | grep -q "\[triggers\]"'
check "plan step 4 says api and dash are only a WARNING and that --strict aborts by itself" 'echo "$planp" | grep -q "^3. current deployment .*api and dash: a WARNING only" && echo "$planp" | grep -q "NOTHING WAS UPLOADED"'
# round g2: the flake. Under load the secret-name check "TURN_KEY_ID absent" came from `printf names | grep -qx NAME` under pipefail: grep -q exits at the first line, the writer gets SIGPIPE, the
# pipeline reports failure. A long name list (more than a pipe buffer) makes that deterministic: both key secrets must still be seen, however many names follow them.
BIGSEC="TURN_KEY_ID TURN_KEY_API_TOKEN $(seq -f 'F_%05g' 1 9000 | tr '\n' ' ')"
RUNN=510; sc "$SC_OK_ABSENT"; rc=$(STUB_SECRETS="$BIGSEC" run "200" --verify-only); sc_clear
check "secret names: TURN_KEY_ID (the FIRST name) and TURN_KEY_API_TOKEN are seen even when 9,000 more names follow (no SIGPIPE under pipefail): the relay is on" '[ "$rc" = 0 ] && grep -q "secrets by name: TURN_KEY_ID present, TURN_KEY_API_TOKEN present" "$T/log510/summary.txt" && grep -q "relay self-check: PASS" "$T/log510/summary.txt"'
# round m3: the same class everywhere. A pipeline must never END in (or run through) a command that can stop reading early: head, grep -q / -m, sed q, awk exit, read. The writer gets SIGPIPE, pipefail
# turns that into a failure, and under set -e the script exits BEFORE the die text. Use sed -n '1,20p' (reads everything) or grep ... >/dev/null. A justified exception carries "# pipefail-ok: <why>" on the same line.
early_pipes() { grep -nE '\|[[:space:]]*(head([[:space:]]|$)|read[[:space:]]|tail[[:space:]]+-n[[:space:]]*\+|grep[^|]*[[:space:]](-[A-Za-z]*[qm][A-Za-z0-9]*|--quiet|--max-count)[[:space:]=]|sed[[:space:]][^|]*[;{[:space:][:digit:]]q([;}'"'"'"[:space:]]|$)|awk[[:space:]][^|]*exit)' "$1" | grep -vE '^[0-9]+:[[:space:]]*#' | grep -v 'pipefail-ok:'; }
check "no pipeline in the deploy script runs through head, grep -q/-m, sed q, awk exit or read (pipefail + SIGPIPE hides the die text); allow-list: a '# pipefail-ok: why' comment" '[ -z "$(early_pipes "$SRV/scripts/deploy-smg-online.sh")" ]'
check "the same in the zone script" '[ -z "$(early_pipes "$SRV/scripts/setup-psvita-zone.sh")" ]'
check "the static check is not blind: it finds head, grep -q, grep -m, sed q and awk exit pipelines, and honours the allow-list comment" 'printf "%s\n" "x | head -1" "x | grep -q y" "x | grep -E -m1 y" "x | sed -n 1q" "x | awk NR==1{exit}" "x | head -1 # pipefail-ok: fine" > "$T/ep.sh"; [ "$(early_pipes "$T/ep.sh" | wc -l | tr -d " ")" = 5 ]'
# 500 wrangler error lines of about 200 bytes (100 KB: more than a pipe holds, so the old `sed | head -n 20` got SIGPIPE every time): the whole die text must still be printed
RUNN=511; rc=$(STUB_DEPLOY_STRICT=1 STUB_DEPLOY_MANY=500 run "200" --skip-tests --set-admin-token)
check "500 error lines on the --strict abort: exactly the first 20 lines from the first [ERROR] are shown and the WHOLE die text follows (ABORTED, NOTHING WAS UPLOADED, do not remove --strict); no secret, no live check" '[ "$rc" = 1 ] && [ "$(grep -c "^  extra " "$T/out511")" = 19 ] && grep -q "wrangler deploy ABORTED under --strict" "$T/out511" && grep -q "NOTHING WAS UPLOADED. STOP: report this message" "$T/out511" && grep -q "do not remove --strict" "$T/out511" && ! grep -q "secret put" "$T/stub.log" && ! grep -q "^curl" "$T/stub.log"'
RUNN=512; rc=$(STUB_DEPLOY_APIERR=1 STUB_DEPLOY_MANY=500 run "200" --skip-tests --set-admin-token)
check "500 error lines on a failed deploy: exactly the first 30 lines from the first [ERROR] are shown and the WHOLE die text follows (FAILED, may have failed part-way, STOP, run --verify-only); no secret, no live check" '[ "$rc" = 1 ] && [ "$(grep -c "^  extra " "$T/out512")" = 28 ] && grep -q "wrangler deploy FAILED, and it may have failed part-way" "$T/out512" && grep -q "STOP: report this message and run --verify-only" "$T/out512" && ! grep -q "secret put" "$T/stub.log" && ! grep -q "^curl" "$T/stub.log"'
echo "[dns/strict/env/secret tests done]" >/dev/null
echo "total: $NOK ok, $NFAIL FAIL"
exit $fail
