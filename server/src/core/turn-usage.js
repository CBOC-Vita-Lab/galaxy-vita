// The hard TURN usage stop (svcfix3 part D3). The estimate-based caps in turn.js count what the clients report; this reads what Cloudflare
// itself billed (relay egress, GraphQL Analytics API) and stops minting new relay passes when a limit is reached.
//
//   source:   'cloudflare' when TURN_USAGE_TOKEN (a read-only analytics token) and TURN_ACCOUNT_ID are set, else 'estimate'
//             (the estimate caps stay in force either way; without the token the hard stop is unavailable, logged once).
//   numbers:  egress bytes, month to date and today (UTC), for OUR key and (month) for the whole account: the free 1,000 GB a month is shared.
//   when:     lazily on the mint path and from the Durable Object alarm, at most every usageEverySec (default 600 s).
//   trip:     any of Galaxy month >= TURN_HARD_MONTH_GB, account month >= TURN_HARD_ACCOUNT_MONTH_GB, Galaxy day >= TURN_HARD_DAY_GB.
//             While tripped no new pass is minted (503 turn_budget with retry-after) and every pass still valid is revoked.
//   fail safe: three analytics reads in a row that fail = tripped for an hour (logged); the next success re-evaluates.
//
// API names (Cloudflare's docs): dataset callsTurnUsageAdaptiveGroups, filters keyId and date_geq / date_leq (UTC dates YYYY-MM-DD), metric
// sum { egressBytes }. The numbers are adaptively sampled: estimates, which is fine for a breaker. Not run against the live API from here:
// the deploy script's self-check route reports usage: ok|error (with the reason) so a wrong name shows up at once.

import { ApiError } from './util.js';
import { redactorFor } from './redact.js';

export const GRAPHQL_URL = 'https://api.cloudflare.com/client/v4/graphql';
const GB = 1e9;
const RETRY_AFTER_FAIL_MS = 60_000;
const FAIL_SAFE_FAILS = 3;
const FAIL_SAFE_MS = 3600_000;
const REVOKE_BATCH = 200;
const ID_RE = /^[A-Za-z0-9_-]{1,64}$/;

// Dates are UTC calendar dates (YYYY-MM-DD): the dataset's `date_geq` / `date_leq` filters.
const dateOf = (ms) => new Date(ms).toISOString().slice(0, 10);
const startOfMonth = (ms) => { const d = new Date(ms); return new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), 1)).toISOString().slice(0, 10); };
const startOfDay = (ms) => dateOf(ms);
const nextMonth = (ms) => { const d = new Date(ms); return Date.UTC(d.getUTCFullYear(), d.getUTCMonth() + 1, 1); };
const nextMidnight = (ms) => Date.parse(`${startOfDay(ms)}T00:00:00Z`) + 86400_000;

export function usageQuery(accountId, keyId, now) {
  const m = startOfMonth(now), d = startOfDay(now), e = dateOf(now);
  const grp = (alias, filter) => `${alias}: callsTurnUsageAdaptiveGroups(limit: 1, filter: { ${filter} }) { sum { egressBytes } }`;
  return `query { viewer { accounts(filter: { accountTag: "${accountId}" }) { ` +
    grp('gm', `keyId: "${keyId}", date_geq: "${m}", date_leq: "${e}"`) + ' ' +
    grp('gd', `keyId: "${keyId}", date_geq: "${d}", date_leq: "${d}"`) + ' ' +
    grp('am', `date_geq: "${m}", date_leq: "${e}"`) + ' } } }';
}

/**
 * @param {any} t cfg.turn
 * @param {typeof fetch} fetchImpl
 * @param {(msg: string) => void} log
 */
export function createUsageGuard(t, fetchImpl, log = (m) => console.warn(m)) {
  const configured = !!(t.usageToken && t.accountId);
  const redact = redactorFor(t); // every upstream text that can reach a log line or the stats goes through it
  const st = {
    at: 0, nextAt: 0, galaxyMonth: null, galaxyDay: null, accountMonth: null,
    fails: 0, trip: null, lastError: '', revoked: 0, revokeNotFound: 0, revokeGone: 0, warned: false, inflight: null, lastOk: 0,
  };

  function warnOnce() {
    if (configured || st.warned || !(t.keyId || t.provider === 'cloudflare')) return;
    st.warned = true;
    log('turn: hard usage stop unavailable (no TURN_USAGE_TOKEN / TURN_ACCOUNT_ID): the estimate-based caps are the only limit');
  }

  function evaluate(now) {
    const over = (v, gb) => gb > 0 && v !== null && v >= gb * GB;
    if (over(st.galaxyMonth, t.hardMonthGb)) return { reason: 'galaxy_month', until: nextMonth(now) };
    if (over(st.accountMonth, t.hardAccountMonthGb)) return { reason: 'account_month', until: nextMonth(now) };
    if (over(st.galaxyDay, t.hardDayGb)) return { reason: 'galaxy_day', until: nextMidnight(now) };
    return null;
  }

  async function read(now) {
    if (!ID_RE.test(t.accountId) || !ID_RE.test(t.keyId || '')) throw new Error('bad account or key id');
    const resp = await fetchImpl(GRAPHQL_URL, {
      method: 'POST',
      headers: { authorization: `Bearer ${t.usageToken}`, 'content-type': 'application/json' },
      body: JSON.stringify({ query: usageQuery(t.accountId, t.keyId, now) }),
    });
    if (!resp.ok) throw new Error(`analytics HTTP ${resp.status}`);
    const j = await resp.json();
    if (Array.isArray(j.errors) && j.errors.length) {
      // the first 120 characters of the first message, so a wrong field or date range can be read from one run; no e-mail address, no run of 32+ hex digits
      const m = j.errors[0] && typeof j.errors[0].message === 'string' ? j.errors[0].message : '';
      const clean = redact(m);
      throw new Error(`analytics answered with errors${clean ? `: ${clean}` : ''}`);
    }
    const acct = j && j.data && j.data.viewer && Array.isArray(j.data.viewer.accounts) ? j.data.viewer.accounts[0] : null;
    if (!acct) throw new Error('analytics answer has no account');
    const sum = (g) => {
      if (!Array.isArray(g)) throw new Error('analytics answer is missing a group');
      return g.length && g[0].sum && Number.isFinite(Number(g[0].sum.egressBytes)) ? Number(g[0].sum.egressBytes) : 0;
    };
    return { galaxyMonth: sum(acct.gm), galaxyDay: sum(acct.gd), accountMonth: sum(acct.am) };
  }

  const api = {
    configured,
    state: st,
    /** Is a read due? (never when the usage token is absent) */
    due: (now) => configured && (now >= st.nextAt || now < st.at),
    /** Read the analytics if due (or `force`). Concurrent callers share one read. Never throws. Returns true when a NEW trip began. */
    async refresh(now, force = false) {
      warnOnce();
      if (!configured) return false;
      if (st.inflight) { await st.inflight; return false; }
      if (!force && !api.due(now)) return false;
      const before = st.trip && st.trip.until > now ? st.trip.reason : null;
      st.inflight = (async () => {
        try {
          const r = await read(now);
          st.galaxyMonth = r.galaxyMonth; st.galaxyDay = r.galaxyDay; st.accountMonth = r.accountMonth;
          st.at = now; st.lastOk = now; st.nextAt = now + t.usageEverySec * 1000; st.fails = 0; st.lastError = '';
          const tr = evaluate(now);
          if (tr && !(st.trip && st.trip.reason === tr.reason && st.trip.until > now)) log(`turn: hard stop reached (${tr.reason}): new relay passes are refused; passes still valid are revoked`);
          if (!tr && st.trip) log('turn: usage back under the hard limits: relay passes are issued again');
          st.trip = tr;
        } catch (e) {
          st.fails++; st.at = now; st.nextAt = now + RETRY_AFTER_FAIL_MS; st.lastError = redact(`${e && e.message ? e.message : e}`, 170);
          log(`turn: usage read failed (${st.fails} in a row): ${st.lastError}`);
          if (st.fails >= FAIL_SAFE_FAILS) {
            if (!(st.trip && st.trip.reason === 'analytics_unavailable' && st.trip.until > now)) log('turn: usage unreadable 3 times in a row: relay passes stopped for an hour (fail safe)');
            st.trip = { reason: 'analytics_unavailable', until: now + FAIL_SAFE_MS };
          }
        }
      })();
      try { await st.inflight; } finally { st.inflight = null; }
      return !!(st.trip && st.trip.until > now && st.trip.reason !== before);
    },
    tripped: (now) => (st.trip && st.trip.until > now ? st.trip : null),
    /** Throws the mint-path refusal while tripped. */
    gate(now) {
      const tr = api.tripped(now);
      if (!tr) return;
      const wait = Math.min(3600, Math.max(60, Math.ceil((tr.until - now) / 1000)));
      throw new ApiError(503, 'turn_budget', 'the relay is stopped for now (usage limit)', { retryAfterSec: wait }, { 'retry-after': String(wait) });
    },
    status(now) {
      const tr = api.tripped(now);
      const mb = (v) => (v === null ? null : Math.round(v / 1e6));
      return {
        source: configured ? 'cloudflare' : 'estimate',
        hardStop: configured ? 'on' : 'unavailable',
        galaxyMonthBytes: st.galaxyMonth, galaxyDayBytes: st.galaxyDay, accountMonthBytes: st.accountMonth,
        galaxyMonthMb: mb(st.galaxyMonth), galaxyDayMb: mb(st.galaxyDay), accountMonthMb: mb(st.accountMonth),
        limits: { galaxyMonthGb: t.hardMonthGb, accountMonthGb: t.hardAccountMonthGb, galaxyDayGb: t.hardDayGb },
        tripped: !!tr, trippedReason: tr ? tr.reason : null, trippedUntilMs: tr ? tr.until : null,
        readAtMs: st.lastOk || null, readFailsInARow: st.fails, lastError: st.lastError || null, revokedPasses: st.revoked, revokeNotFound: st.revokeNotFound, revokeGone: st.revokeGone,
      };
    },
    /** Revoke the passes still valid (named, not yet revoked). Never throws; returns how many were revoked. One try per pass: a 404 on a pass past its TTL counts as gone, a 404 on a valid one is counted (revokeNotFound) and retried by the next sweep. */
    async revokeValid(c) {
      if (!c.turn || typeof c.turn.revoke !== 'function') return 0;
      const rows = c.db.all(
        `SELECT id, username, minted_ms FROM turn_mints WHERE username != '' AND revoked = 0 AND minted_ms >= ? ORDER BY id LIMIT ${REVOKE_BATCH}`,
        c.now - t.ttlSec * 1000,
      );
      let n = 0;
      for (const r of rows) {
        let ok = false;
        try { ok = await c.turn.revoke(r.username); } catch (e) {
          if (e && e.notFound) {
            // 404 "cannot find": a pass whose TTL has run out is gone already (done); a pass still valid may not be known to the revoke endpoint yet: keep it for the next sweep
            if (r.minted_ms + t.ttlSec * 1000 <= c.now) { c.db.run(`UPDATE turn_mints SET revoked = 1 WHERE id = ?`, r.id); st.revokeGone++; continue; }
            st.revokeNotFound++;
          }
          ok = false;
        }
        if (ok) { c.db.run(`UPDATE turn_mints SET revoked = 1 WHERE id = ?`, r.id); n++; st.revoked++; }
      }
      return n;
    },
    /** From the alarm: read when due; while tripped, sweep the revoke. */
    async tick(c) {
      await api.refresh(c.now);
      if (api.tripped(c.now)) await api.revokeValid(c);
    },
  };
  return api;
}
