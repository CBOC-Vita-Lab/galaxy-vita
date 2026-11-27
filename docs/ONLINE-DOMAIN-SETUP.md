# The online service's own domain: what the Codex request does, and how to undo it

Decision of 14 Nov 2026: the Galaxy 3.0 online service answers on **https://galaxy.psvita.dev**, a Worker custom domain of the zone `psvita.dev`
on the same Cloudflare account as the Strikers service. There are no manual dashboard steps for the owner: everything Cloudflare-side is ONE
Codex request (`CODEX-galaxy-3.0-service-1114.md`, kept by the coordinator). This page documents what that request changes and how to undo each part.
Details and sources: `server/README.md`, "What the DEPLOYER must do".

## What the request does, in order

1. **Deploy the package** with `server/scripts/deploy-smg-online.sh` (wrangler, with the custom domain declared in `server/wrangler.smg.toml`:
   `routes = [{ pattern = "galaxy.psvita.dev", custom_domain = true }]`). Cloudflare creates the DNS record for `galaxy` and its certificate.
   `workers_dev = true` stays on until release; then it is set to `false` and the service is deployed again.
2. **Zone rules** with `server/scripts/setup-psvita-zone.sh` (Cloudflare Rulesets API; the zone is looked up by name `psvita.dev` and must belong to the account):
   * WAF custom rule `galaxy-psvita-block-unserved` (phase `http_request_firewall_custom`), action Block: every path on `galaxy.psvita.dev` that the
     Worker does not serve. The Worker serves exactly `/v1/*` and `/times`. Expression:
     `(http.host eq "galaxy.psvita.dev" and not starts_with(http.request.uri.path, "/v1/") and http.request.uri.path ne "/times" and not starts_with(http.request.uri.path, "/.well-known/acme-challenge/") and not starts_with(http.request.uri.path, "/.well-known/pki-validation/"))`
     (the last two are certificate-validation paths, left alone so renewals work).
   * Rate-limiting rule `psvita-shared-ratelimit` (phase `http_ratelimit`), action Block: the paths `/v1/*` and `/times` of the whole zone, per IP, 100 requests
     per 10 s, block for 10 s. Expression: `(starts_with(http.request.uri.path, "/v1/") or http.request.uri.path eq "/times")`. **The zone is shared by the games**
     (Strikers is on its own host and Worker), and the Free plan has ONE rate-limiting rule per zone, so this is the one rule for all of them, path form only
     (no host field). Strikers must not add a second rate-limiting rule. `--ratelimit-extra-paths "<list>"` (default none, only when the owner asks) adds another
     game's paths to this same rule. `--ratelimit-path-only` is a no-op kept for one release.
   * If a rate-limiting rule named `galaxy-psvita-ratelimit` exists from an earlier run of this script (the Galaxy-only host form; it was never deployed by the 14 Nov request, but check), the owner deletes it once in the dashboard
     (Security > Security rules): the Free plan has ONE rate-limiting rule and the script stops while a rule that is not its own is there.
   * The WAF rule above is Galaxy's alone: it always carries `http.host eq "galaxy.psvita.dev"`, so it never touches the Strikers host.
   * Only these two rules are written, found by their `ref`; rules of anyone else in the same phases are read and kept. A second run changes nothing.
3. **Bot Fight Mode stays OFF** (it cannot be skipped by WAF rules and may challenge the game's API calls; a Vita cannot answer a challenge). Nothing changes it;
   `setup-psvita-zone.sh --verify-only` reports it if it is on. It is a zone-wide setting, so it matters for every game on the zone. **Browser Integrity Check**
   is zone-wide too: `--verify-only` reports whether it is on, as text only; the script never changes it.
4. **Notifications: none.** The Free plan has no Workers / Durable Objects usage notification (Usage Based Billing notifications and budget alerts are for
   Professional and pay-as-you-go accounts). Use Workers & Pages > smg-online > Metrics and `GET /v1/admin/stats` instead.

## Checks after it ran (read only)

    cd server
    scripts/deploy-smg-online.sh --account-id <32 hex> --admin-token-file <file> --expect-subdomain <workers.dev subdomain> --verify-only
    scripts/setup-psvita-zone.sh --account-id <32 hex> --token-file <file> --verify-only

A junk path must be refused by Cloudflare, not by the Worker: `curl -i https://galaxy.psvita.dev/robots.txt` answers 403 from Cloudflare (no `x-msc-*` headers).

## Rollback

* **Zone rules**: `scripts/setup-psvita-zone.sh --account-id <32 hex> --token-file <file> --remove` deletes only our WAF rule. The shared rate-limiting rule serves the
  other game too, so it is deleted only with `--remove-shared-ratelimit` added. A normal run that finds the shared rule holding anything the plan lacks (another game's
  paths or any hand-written term) stops and prints both expressions: `--ratelimit-drop-paths` drops it, `--ratelimit-extra-paths` keeps paths. A rule whose threshold,
  period, mitigation, characteristics or description differ stops the same way; `--ratelimit-reset` resets them. By hand: Security > Security rules, delete the WAF rule named
  "smg-online (galaxy.psvita.dev) ..." and the rate-limiting rule named "shared by the games on psvita.dev ...".
* **Custom domain**: remove the `routes = [...]` line from `server/wrangler.smg.toml` and deploy again (the deploy script then refuses: it insists on the
  line; use `npx wrangler deploy --config wrangler.smg.toml` by hand for a rollback), or dashboard: Workers & Pages > smg-online > Settings > Domains & Routes,
  remove `galaxy.psvita.dev` (this deletes its DNS record). Clients built with the default address then cannot reach the service until a build or
  `mp.cfg` `server = ` names another one; there is no fallback, by design.
* **A bad Worker version**: `npx wrangler rollback --config wrangler.smg.toml`.
* **At release**: set `workers_dev = false` in `wrangler.smg.toml` and deploy (the workers.dev address stops answering; nothing else changes).

## What the token needs

* Deploy: no API token. wrangler runs on the existing `wrangler login`, as on 13 Nov. If the login cannot create the custom domain, wrangler says so and the request stops.
* Zone rules (`setup-psvita-zone.sh --token-file`): created once by the owner (My Profile > API Tokens > Create Token > Create Custom Token): Zone > Zone > Read and
  Zone > Zone WAF > Edit, Zone Resources "Include > Specific zone > psvita.dev" ONLY (nothing account-wide: it would reach the other game's service too).
  Optional: Zone > Bot Management > Read (only so `--verify-only` can read Bot Fight Mode) and Zone > Zone Settings > Read (only so it can report Browser Integrity Check). Without the file the request still deploys and verifies; only the zone part waits
  ("ZONE RULES PENDING", then "rerun part C"). A missing permission stops the script with its name; a refusal by the plan is reported as such, not as a permission.
  `--bot-fight-off` (opt-in, never used by the request) is the only way the script writes Bot Fight Mode, and needs a permission to edit it.

## Notes

* The rate-limiting rule counts by `cf.colo.id` + `ip.src` (Cloudflare lists `cf.colo.id` as mandatory: developers.cloudflare.com/waf/rate-limiting-rules/parameters/).
  The rule is in the path form only (the Free plan documents only Path as a field), so it covers every host of the zone with ONE shared counter.
* Test builds carry the production address, so the test harness sets `SMG_ONLINE_URL=none` for every process unless a scenario names a service (`tools/mp/harness/common.sh`;
  `HARNESS_LIVE_SERVICE=1` opts out; `tests/mp/harness_test.py` checks it). Existing cmake build folders keep a cached empty or old `NP_ONLINE_URL`: configure again or pass `-DNP_ONLINE_URL=...`.
* A test console that reached the service through `SMG_ONLINE_URL` / `mp.cfg` with the workers.dev address gets a second account once that line is removed (accounts are per address).
