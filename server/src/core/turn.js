// TURN credential minting. The provider call sits behind a tiny interface so tests and local runs use a stub and
// production uses Cloudflare Realtime TURN. The long-lived TURN key API token never leaves the Worker; clients only
// get short-lived username/credential pairs, and only while they are in a live room.

import { ApiError, b64url, randomBytes, isInt, strict } from './util.js';
import { loadRoom, slotOf } from './rooms.js';
import { turnAllowed, turnNeeded, memberAddr } from './rooms4.js';
import { bumpDaily, dailyCapError, getDaily } from './limits.js';
import { createUsageGuard } from './turn-usage.js';
import { redactorFor } from './redact.js';

/**
 * @typedef {{urls: string[], username: string, credential: string}} IceServer
 * @typedef {{mint(ttlSec: number, opts?: {customIdentifier?: string}): Promise<{servers: IceServer[], username: string}>, revoke(username: string, opts?: {retries?: number, firstDelayMs?: number, sleep?: (ms: number) => Promise<void>, report?: object}): Promise<boolean>}} TurnProvider
 */

/**
 * `HTTP 400: <redacted text, at most 120 characters>`: what the self-check shows when the relay API refuses (a wrong field name must not fail silently).
 * The text goes through redact.js (the configured secrets, e-mail, hex and token-shaped runs masked) BEFORE it is cut; the raw body is never kept.
 */
async function upstreamReason(resp, redact) {
  let text = '';
  try { text = redact(await resp.text()); } catch { /* none */ }
  return `HTTP ${resp.status}${text ? `: ${text}` : ''}`;
}

/** Cloudflare Realtime TURN: POST .../turn/keys/{id}/credentials/generate-ice-servers. */
export function cloudflareProvider(cfg, fetchImpl = fetch, sleepImpl = null) {
  // The tag (customIdentifier) is documented on .../credentials/generate, not on generate-ice-servers. It is only a label for analytics (the hard
  // stop filters by key id), so it must NEVER make a mint fail: any 4xx except 401, 403 and 429 (those are about the key or the rate, not the field) when the
  // field was sent is retried ONCE without it, and from then on, for the life of this isolate, mints are sent untagged.
  let tagUnsupported = false;
  const redact = redactorFor(cfg);
  return {
    get tagUnsupported() { return tagUnsupported; },
    async mint(ttlSec, opts = {}) {
      const url = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(cfg.keyId)}/credentials/generate-ice-servers`;
      const send = (tag) => fetchImpl(url, {
        method: 'POST',
        headers: { authorization: `Bearer ${cfg.apiToken}`, 'content-type': 'application/json' },
        body: JSON.stringify(tag ? { ttl: ttlSec, customIdentifier: tag } : { ttl: ttlSec }),
      });
      const tag = opts.customIdentifier && !tagUnsupported ? opts.customIdentifier : '';
      let resp = await send(tag);
      if (!resp.ok && tag && resp.status >= 400 && resp.status < 500) {
        if (resp.status !== 401 && resp.status !== 403 && resp.status !== 429) {
          tagUnsupported = true;
          console.warn('turn: the relay API refused the customIdentifier tag: passes are minted untagged from now on');
          resp = await send('');
        }
      }
      if (!resp.ok) {
        const e = new ApiError(502, 'turn_upstream', 'the relay service refused the request');
        e.upstream = await upstreamReason(resp, redact); // for the admin self-check only: never put in an answer to a player
        throw e;
      }
      const data = await resp.json();
      const list = Array.isArray(data.iceServers) ? data.iceServers : data.iceServers ? [data.iceServers] : [];
      const named = list.find((x) => x && typeof x.username === 'string' && x.username);
      return { servers: list, username: named ? named.username : '', untagged: tagUnsupported };
    },
    /**
     * POST .../credentials/<username>/revoke: Cloudflare answers 204 on success. Anything else throws (with `upstream` for the self-check).
     * A 404 ("cannot find specified username") right after a mint is most likely eventual consistency, so with `opts.retries` > 0 it is retried with a
     * doubling wait (`opts.firstDelayMs`, default 1000; 1, 2, 4, 8 s for 4 retries = 15 s in all). Without `opts` there is exactly one try (the hard-stop sweep: the next sweep is the retry).
     * `opts.report` (an object) is filled with {tries, waitedMs}; a thrown error carries `status`, `notFound` (the last answer was a 404), `tries` and `waitedMs` too.
     */
    async revoke(username, opts = {}) {
      if (!username) return false;
      const retries = Math.max(0, Math.min(6, opts.retries | 0));
      const sleep = opts.sleep || sleepImpl || ((ms) => new Promise((r) => setTimeout(r, ms)));
      let wait = opts.firstDelayMs > 0 ? opts.firstDelayMs : 1000;
      const url = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(cfg.keyId)}/credentials/${encodeURIComponent(username)}/revoke`;
      let tries = 0, waitedMs = 0;
      for (;;) {
        tries++;
        const resp = await fetchImpl(url, { method: 'POST', headers: { authorization: `Bearer ${cfg.apiToken}`, 'content-type': 'application/json' }, body: '{}' });
        if (opts.report) { opts.report.tries = tries; opts.report.waitedMs = waitedMs; }
        if (resp.status === 204) return true;
        if (resp.status === 404 && tries <= retries) {
          try { await resp.text(); } catch { /* drain */ }
          await sleep(wait);
          waitedMs += wait; wait *= 2;
          continue;
        }
        const e = new Error('revoke refused');
        e.status = resp.status; e.notFound = resp.status === 404; e.tries = tries; e.waitedMs = waitedMs;
        e.upstream = await upstreamReason(resp, redact);
        throw e;
      }
    },
  };
}

/**
 * Fake credentials for local development and tests. By default random and meaningless (turn.example.invalid). With
 * `TURN_STUB_URLS` (comma separated turn: URLs) and `TURN_STUB_SECRET` they follow the "TURN REST" scheme that coturn's
 * `use-auth-secret` and the client's test TURN server (native/tools/net/turnsim.cpp) check: username `<expiry unix s>:<id>`,
 * credential = base64(HMAC-SHA1(secret, username)). Never used with the real provider.
 */
export function stubProvider(cfg = {}) {
  const urlList = String(cfg.stubUrls || '').split(',').map((u) => u.trim()).filter(Boolean);
  const secret = String(cfg.stubSecret || '');
  return {
    async mint(ttlSec) {
      const exp = Math.floor(Date.now() / 1000) + ttlSec;
      if (urlList.length && secret) {
        const username = `${exp}:stub-${b64url(randomBytes(6))}`;
        const enc = new TextEncoder();
        const key = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
        const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode(username)));
        let bin = '';
        for (const b of mac) bin += String.fromCharCode(b);
        return { servers: [{ urls: ['stun:stun.cloudflare.com:3478'] }, { urls: urlList, username, credential: btoa(bin) }], username };
      }
      const username = `${exp}:stub-${b64url(randomBytes(6))}`;
      return {
        servers: [
          { urls: ['stun:stun.cloudflare.com:3478'] },
          {
            urls: ['turn:turn.example.invalid:3478?transport=udp', 'turn:turn.example.invalid:53?transport=udp', 'turn:turn.example.invalid:3478?transport=tcp'],
            username,
            credential: b64url(randomBytes(18)),
          },
        ],
        username,
      };
    },
    async revoke() { return true; },
  };
}

export function createProvider(cfg, fetchImpl, sleepImpl) {
  if (cfg.provider === 'stub') return stubProvider(cfg);
  if (cfg.provider === 'cloudflare' && cfg.keyId && cfg.apiToken) return cloudflareProvider(cfg, fetchImpl, sleepImpl);
  return null;
}

/** The Vita has no TCP (SOSocket2 refuses it) and no TLS-to-relay: keep UDP TURN and plain STUN only. */
export function udpOnly(servers) {
  const out = [];
  for (const s of servers) {
    const urls = (Array.isArray(s.urls) ? s.urls : [s.urls]).filter(
      (u) => typeof u === 'string' && /^(stun|turn):/.test(u) && !/transport=tcp/i.test(u),
    );
    if (urls.length) out.push({ urls, ...(s.username ? { username: s.username, credential: s.credential } : {}) });
  }
  return out;
}

/** Self-check revoke: up to 4 retries after a 404, waiting 1, 2, 4, 8 s (15 s; a Worker request has no wall-clock limit while the caller waits, only CPU time). */
const REVOKE_SELFCHECK_RETRIES = 4;

const day = (ms) => new Date(ms).toISOString().slice(0, 10);
const month = (ms) => new Date(ms).toISOString().slice(0, 7);

function counter(c, key) {
  const r = c.db.get(`SELECT value FROM counters WHERE key = ?`, key);
  return r ? r.value : 0;
}
function bumpCounter(c, key, by) {
  c.db.run(
    `INSERT INTO counters (key, value, updated_ms) VALUES (?, ?, ?)
     ON CONFLICT(key) DO UPDATE SET value = value + excluded.value, updated_ms = excluded.updated_ms`,
    key, by, c.now,
  );
}

export { createUsageGuard } from './turn-usage.js';
export function turnStatus(c) {
  const t = c.cfg.turn;
  return {
    enabled: !!c.turn,
    tagUnsupported: !!(c.turn && c.turn.tagUnsupported), // true once the relay API refused the customIdentifier tag (mints go out untagged)
    credsToday: counter(c, `turn:creds:${day(c.now)}`),
    checkPassesToday: counter(c, `turn:check:${day(c.now)}`), // "Check Connection" passes (inside credsToday); counted apart
    checkMaxPerDay: t.checkMaxPerDay,
    maxPerDay: t.maxPerDay,
    estimatedMbThisMonth: counter(c, `turn:mb:${month(c.now)}`),
    monthlyBudgetMb: t.monthlyBudgetMb,
    usage: c.turnGuard ? c.turnGuard.status(c.now) : undefined, // the hard stop: source (cloudflare|estimate), egress bytes, limits, tripped state
  };
}

/**
 * Admin self-check (POST /v1/admin/turn-selfcheck): mint one pass (300 s, identifier "selfcheck"), revoke it at once, read the usage numbers.
 * Reports {mint, revoke (ok|error|skipped; revokeDelayed, revokeTries, revokeWaitedMs when a 404 had to be retried; revokeDetail on error), usage: ok|absent|error, galaxyMonthMb, accountMonthMb, tripped}. NEVER returns the credential or the pass's username.
 */
export async function turnSelfCheck(c) {
  const redact = redactorFor(c.cfg.turn);
  const out = { mint: 'off', revoke: 'skipped', usage: 'absent', galaxyMonthMb: null, accountMonthMb: null, tripped: false, untagged: false };
  if (c.turn) {
    let minted = null;
    try { minted = await c.turn.mint(300, { customIdentifier: 'selfcheck' }); out.mint = minted && Array.isArray(minted.servers) && minted.servers.length ? 'ok' : 'error'; out.untagged = !!(minted && minted.untagged); } catch (e) { out.mint = 'error'; out.mintDetail = redact((e && e.upstream) || 'the relay service is unavailable', 200); }
    if (out.mint === 'ok') {
      const rep = {};
      try {
        const ok = minted.username && (await c.turn.revoke(minted.username, { retries: REVOKE_SELFCHECK_RETRIES, report: rep }));
        out.revoke = ok ? 'ok' : 'error';
        if (ok && rep.tries > 1) { out.revokeDelayed = true; out.revokeTries = rep.tries; out.revokeWaitedMs = rep.waitedMs; }
        if (!ok) out.revokeDetail = 'no username to revoke';
      } catch (e) {
        out.revoke = 'error';
        out.revokeDetail = redact((e && e.upstream) || 'revoke failed', 200);
        out.revokeTries = (e && e.tries) || rep.tries || 1; out.revokeWaitedMs = (e && e.waitedMs) || rep.waitedMs || 0;
      }
    }
  }
  const g = c.turnGuard;
  if (g && g.configured) {
    await g.refresh(c.now, true);
    const s = g.status(c.now);
    out.usage = s.readFailsInARow === 0 && s.galaxyMonthMb !== null ? 'ok' : 'error';
    if (out.usage === 'error') out.usageDetail = redact(s.lastError || 'no numbers', 200);
    out.galaxyMonthMb = s.galaxyMonthMb;
    out.accountMonthMb = s.accountMonthMb;
  }
  out.tripped = !!(g && g.tripped(c.now));
  return out;
}

export async function mintTurn(c, p, body) {
  const t = c.cfg.turn;
  if (!c.turn) throw new ApiError(503, 'turn_unavailable', 'the relay is not configured');
  strict(body, ['room']);
  // The hard usage stop: read Cloudflare's numbers if the last read is old (at most every TURN_USAGE_EVERY_SEC), refuse while tripped
  if (c.turnGuard) {
    const began = await c.turnGuard.refresh(c.now);
    if (began) await c.turnGuard.revokeValid(c);
    c.turnGuard.gate(c.now);
  }
  // Reserve budget and check room membership in one synchronous step, before the network call.
  const reserved = c.tx(() => {
    const r = loadRoom(c, String(body.room || '').toUpperCase());
    // Galaxy rooms (up to 4) keep their members in room_members; Strikers rooms in host_pid / join_pid
    const member = r && (r.game === 'smg' ? turnAllowed(c, r, p.pid) : slotOf(r, p.pid) >= 0 && ['joined', 'ready', 'started'].includes(r.state));
    if (!member) {
      throw new ApiError(403, 'turn_not_allowed', 'relay credentials are only issued inside an active room');
    }
    if (counter(c, `turn:creds:${day(c.now)}`) >= t.maxPerDay) {
      throw new ApiError(503, 'turn_budget', 'the relay budget for today is used up', { retryAfterSec: 3600 });
    }
    if (counter(c, `turn:mb:${month(c.now)}`) >= t.monthlyBudgetMb) {
      throw new ApiError(503, 'turn_budget', 'the relay budget for this month is used up', { retryAfterSec: 86400 });
    }
    if (counter(c, `turn:creds:${day(c.now)}:p${p.pid}`) >= t.maxPerPlayerDay) {
      throw new ApiError(429, 'turn_player_limit', 'too many relay credentials requested today', { retryAfterSec: 3600 });
    }
    // fresh accounts have their own slice of the day, so a swarm of new accounts cannot starve established players
    const fresh = t.newAccountHours > 0 && c.now - p.created_ms < t.newAccountHours * 3600_000;
    if (fresh && counter(c, `turn:creds:${day(c.now)}:new`) >= t.newAccountDaily) {
      throw new ApiError(503, 'turn_budget', 'the relay is busy for new accounts today', { retryAfterSec: 3600 });
    }
    // svcfix3 (S5), Galaxy rooms: per room, per source address, and a smaller slice for credentials without a visible reason
    const extra = [];
    if (r.game === 'smg') {
      const rk = `turn:room:${r.code}:${Math.floor(c.now / 3600_000)}`; // per hour: a credential lasts an hour, so a 4-member room that renews every hour never trips it
      if (t.maxPerRoom && counter(c, rk) >= t.maxPerRoom) {
        throw new ApiError(429, 'turn_room_limit', 'this room has had its relay credentials', { retryAfterSec: 3600 });
      }
      if (t.maxPerRoom) extra.push(rk);
      const addr = memberAddr(c, r.code, p.pid);
      if (t.maxPerAddrDay && addr) {
        const ak = `turn:creds:${day(c.now)}:a${addr}`;
        if (counter(c, ak) >= t.maxPerAddrDay) {
          throw new ApiError(429, 'turn_player_limit', 'too many relay credentials requested from this address today', { retryAfterSec: 3600 });
        }
        extra.push(ak);
      }
      if (t.unneededSharePct < 100 && !turnNeeded(c, r, p.pid)) {
        const sk = `turn:creds:${day(c.now)}:spec`;
        if (counter(c, sk) >= Math.floor((t.maxPerDay * t.unneededSharePct) / 100)) {
          throw new ApiError(503, 'turn_budget', 'the relay is busy today for connections that punch through', { retryAfterSec: 3600 });
        }
        extra.push(sk);
      }
    }
    bumpCounter(c, `turn:creds:${day(c.now)}`, 1);
    bumpCounter(c, `turn:creds:${day(c.now)}:p${p.pid}`, 1);
    if (fresh) bumpCounter(c, `turn:creds:${day(c.now)}:new`, 1);
    for (const k of extra) bumpCounter(c, k, 1);
    // remember the mint: usage reports are only accepted against a credential that is still valid
    const row = c.db.get(`INSERT INTO turn_mints (pid, minted_ms) VALUES (?, ?) RETURNING id`, p.pid, c.now);
    return { mintId: row.id, fresh, extra };
  });
  let minted;
  try {
    minted = await c.turn.mint(t.ttlSec, { customIdentifier: 'galaxy' });
    if (minted && minted.username) c.db.run(`UPDATE turn_mints SET username = ? WHERE id = ?`, minted.username, reserved.mintId);
  } catch (e) {
    // give the reservation back
    c.tx(() => {
      bumpCounter(c, `turn:creds:${day(c.now)}`, -1);
      bumpCounter(c, `turn:creds:${day(c.now)}:p${p.pid}`, -1);
      if (reserved.fresh) bumpCounter(c, `turn:creds:${day(c.now)}:new`, -1);
      for (const k of reserved.extra) bumpCounter(c, k, -1);
      c.db.run(`DELETE FROM turn_mints WHERE id = ?`, reserved.mintId);
    });
    if (e instanceof ApiError) throw e;
    throw new ApiError(502, 'turn_upstream', 'the relay service is unavailable');
  }
  return { ttlSec: t.ttlSec, expiresMs: c.now + t.ttlSec * 1000, servers: udpOnly(minted.servers) };
}

/** The TTL of a "Check Connection" pass (the shortest the relay API takes). */
export const CHECK_TTL_SEC = 300;

/**
 * POST /v1/turn/check: one relay pass for a loopback test on a single console. No room is needed (a normal pass needs a started room). The same ice-server
 * answer as the normal mint, 300 s, tagged "check" (the untagged fallback of the provider applies). Limits: checkPerPlayerDay a player, checkPerAddrDay an
 * address, checkMaxPerDay in all, and every check pass is ALSO one of the day's maxPerDay (and of the month's budget). The hard usage stop, the `turn` kill
 * switch (the route's) and the revoke when the stop trips apply exactly as to normal passes: the pass is written to turn_mints like the others.
 */
export async function checkTurn(c, p, ipk) {
  const t = c.cfg.turn;
  if (!c.turn) throw new ApiError(503, 'turn_unavailable', 'the relay is not configured');
  if (c.turnGuard) {
    const began = await c.turnGuard.refresh(c.now);
    if (began) await c.turnGuard.revokeValid(c);
    c.turnGuard.gate(c.now);
  }
  const reserved = c.tx(() => {
    const d = day(c.now);
    if (counter(c, `turn:creds:${d}`) >= t.maxPerDay) throw new ApiError(503, 'turn_budget', 'the relay budget for today is used up', { retryAfterSec: 3600 });
    if (counter(c, `turn:mb:${month(c.now)}`) >= t.monthlyBudgetMb) throw new ApiError(503, 'turn_budget', 'the relay budget for this month is used up', { retryAfterSec: 86400 });
    if (!t.checkMaxPerDay || counter(c, `turn:check:${d}`) >= t.checkMaxPerDay) throw new ApiError(503, 'turn_budget', 'the connection check is busy today', { retryAfterSec: 3600 });
    // the same new-account gate as a normal pass: a fresh account draws on its own small slice of the day (turn:creds:<day>:new), so a swarm of new accounts cannot starve the others
    const fresh = t.newAccountHours > 0 && c.now - p.created_ms < t.newAccountHours * 3600_000;
    if (fresh && counter(c, `turn:creds:${d}:new`) >= t.newAccountDaily) throw new ApiError(503, 'turn_budget', 'the relay is busy for new accounts today', { retryAfterSec: 3600 });
    if (!t.checkPerPlayerDay || counter(c, `turn:check:${d}:p${p.pid}`) >= t.checkPerPlayerDay) throw new ApiError(429, 'turn_player_limit', 'too many connection checks today', { retryAfterSec: 3600 }, { 'retry-after': '3600' });
    const ak = ipk ? `turn:check:${d}:a${ipk}` : '';
    if (ak && (!t.checkPerAddrDay || counter(c, ak) >= t.checkPerAddrDay)) throw new ApiError(429, 'turn_player_limit', 'too many connection checks from this address today', { retryAfterSec: 3600 }, { 'retry-after': '3600' });
    bumpCounter(c, `turn:creds:${d}`, 1);
    bumpCounter(c, `turn:check:${d}`, 1);
    bumpCounter(c, `turn:check:${d}:p${p.pid}`, 1);
    if (ak) bumpCounter(c, ak, 1);
    if (fresh) bumpCounter(c, `turn:creds:${d}:new`, 1);
    const row = c.db.get(`INSERT INTO turn_mints (pid, minted_ms) VALUES (?, ?) RETURNING id`, p.pid, c.now);
    return { mintId: row.id, ak, fresh };
  });
  let minted;
  try {
    minted = await c.turn.mint(CHECK_TTL_SEC, { customIdentifier: 'check' });
    if (minted && minted.username) c.db.run(`UPDATE turn_mints SET username = ? WHERE id = ?`, minted.username, reserved.mintId);
  } catch (e) {
    c.tx(() => { // give the reservation back
      const d = day(c.now);
      bumpCounter(c, `turn:creds:${d}`, -1);
      bumpCounter(c, `turn:check:${d}`, -1);
      bumpCounter(c, `turn:check:${d}:p${p.pid}`, -1);
      if (reserved.ak) bumpCounter(c, reserved.ak, -1);
      if (reserved.fresh) bumpCounter(c, `turn:creds:${d}:new`, -1);
      c.db.run(`DELETE FROM turn_mints WHERE id = ?`, reserved.mintId);
    });
    if (e instanceof ApiError) throw e;
    throw new ApiError(502, 'turn_upstream', 'the relay service is unavailable');
  }
  return { ttlSec: CHECK_TTL_SEC, expiresMs: c.now + CHECK_TTL_SEC * 1000, servers: udpOnly(minted.servers) };
}

/** Most bytes one credential can plausibly carry: its TTL at TURN_MAX_KBPS (and never more than TURN_MAX_REPORT_MB). */
export function reportCapBytes(t) {
  return Math.min(t.maxReportMb * 1048576, Math.floor((t.ttlSec * t.maxKbps * 1000) / 8));
}

/**
 * Clients report roughly how much they relayed so the monthly budget can trip a breaker. The numbers are untrusted,
 * so a report only counts when (1) the player minted credentials within the credential TTL (plus a short grace for the
 * report that follows the match), (2) it is clamped to what one TTL can carry at a sane bitrate, and the sum over the
 * credentials still valid cannot exceed their combined capacity, and (3) at most TURN_MAX_REPORTS_PER_PLAYER_DAY
 * reports are accepted per player per UTC day. Without this a few accounts could report huge numbers and trip the
 * monthly breaker for everybody.
 */
export function reportTurnUsage(c, p, body) {
  strict(body, ['bytes']);
  if (!isInt(body.bytes, 0, 2 ** 40)) {
    throw new ApiError(400, 'bad_request', 'bytes must be a non-negative integer');
  }
  const t = c.cfg.turn;
  return c.tx(() => {
    const mints = c.db.all(
      `SELECT id, used_bytes FROM turn_mints WHERE pid = ? AND minted_ms >= ? ORDER BY minted_ms`,
      p.pid, c.now - (t.ttlSec * 1000 + t.reportGraceMs),
    );
    if (!mints.length) throw new ApiError(403, 'turn_not_allowed', 'usage can only be reported while relay credentials you minted are valid');
    if (t.maxReportsPerPlayerDay && getDaily(c, p.pid).turn_reports >= t.maxReportsPerPlayerDay) throw dailyCapError(c, 'turn_reports');
    const cap = reportCapBytes(t);
    let left = Math.min(body.bytes, cap); // clamp each report
    const asked = body.bytes;
    let counted = 0;
    for (const m of mints) {
      if (left <= 0) break;
      const room = Math.max(0, cap - m.used_bytes); // and each credential only ever carries `cap`
      const take = Math.min(left, room);
      if (take > 0) c.db.run(`UPDATE turn_mints SET used_bytes = used_bytes + ? WHERE id = ?`, take, m.id);
      left -= take;
      counted += take;
    }
    bumpDaily(c, p.pid, 'turn_reports');
    const mb = Math.ceil(counted / 1048576);
    if (mb > 0) bumpCounter(c, `turn:mb:${month(c.now)}`, mb);
    return { ok: true, countedBytes: counted, clamped: counted < asked };
  });
}
