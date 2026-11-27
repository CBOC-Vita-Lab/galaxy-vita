// Edge entry point. Everything real happens in the single Durable Object; the Worker rejects junk before waking it
// (unknown paths, wrong methods, oversized bodies, malformed Authorization / version headers: see core/edge.js) and
// forwards the rest, including the client address header, which the object uses for in-memory rate limits and, while the member is in a room, stores with the member's row (room_members.src_ip: LAN
// addresses go only to members behind the same address); it is removed when the member leaves or the room is deleted. docs/PRIVACY.md says the same.
// Every request the object sees costs a Durable Object request (and often rows) on the free tier; a request answered here does not.
//
// Two public web routes are answered here as far as possible: GET /leaderboard is static (never wakes the object), and
// GET /v1/public/leaderboard is held in this isolate's memory for 60 s per (board, limit), so a busy page costs the object
// about one request a minute per isolate. (The Cache API is not used: it only works on custom domains, not on workers.dev.)

export { GameServer } from './do.js';
import { edgeCheck, requestClass, routeKey, isFreeing, isMaybeLive } from './core/edge.js';
import { INTERNAL_ADDRESS } from './core/budget.js';
import { ApiError, errorResponse, ipKey, dailyAddrKey } from './core/util.js';
import { createMemoGuard } from './core/memo_guard.js';
import { pageResponse } from './core/leaderboard_page.js';
import { parseQuery, queryKey, MEMO_MS } from './core/publicboard.js';
import { timesPageResponse } from './core/times_page.js';
import { parsePublicQuery, publicKey, PUBLIC_MEMO_MS } from './core/times.js';
import { rawGame } from './core/config.js';
import { createEdgeLimiter } from './core/edge_bucket.js';

// Best-effort limiter in front of the object (core/edge_bucket.js). EDGE_IP_BURST = 0 switches all of it off.
// svc3: 120 burst and 1 a second per bucket (was 200 and 3: one address could still pass 254,000 requests a day to the object through it). An honest
// console asks about once a second at its busiest (a lobby), 0.1 a second in a started room.
// A request with a player token is charged to its own (address + token) bucket, 120 / 1, and to the address's aggregate, 300 burst and 5 a second
// (about four consoles behind one NAT at their busiest, a household or a small gathering; Strikers 2e8378c6: the single per-address bucket throttled them together), so a venue is not
// throttled while a flood of invented tokens still is. A request with no token has the plain per-address bucket (120 / 1).
// EDGE_IP_BURST / EDGE_IP_PER_SEC change the first, EDGE_IP_AGG_BURST / EDGE_IP_AGG_PER_SEC the aggregate. The hard limit is the object's daily
// ceiling per address (core/budget.js), whose verdict is remembered below: the aggregate is a rate, not an allowance (5 a second all day would be
// 430,000 requests, which the ceilings of the object stop long before).
let edgeLimiter = null;
function edgeLimit(request, env) {
  const num = (v, d) => (v === undefined || !Number.isFinite(Number(v)) ? d : Number(v));
  const burst = num(env.EDGE_IP_BURST, 120);
  const perSec = num(env.EDGE_IP_PER_SEC, 1) > 0 ? num(env.EDGE_IP_PER_SEC, 1) : 1;
  const aggBurst = num(env.EDGE_IP_AGG_BURST, 300);
  const aggPerSec = num(env.EDGE_IP_AGG_PER_SEC, 5) > 0 ? num(env.EDGE_IP_AGG_PER_SEC, 5) : 5;
  const cfg = `${burst}:${perSec}:${aggBurst}:${aggPerSec}`;
  if (!edgeLimiter || edgeLimiter.cfg !== cfg) {
    edgeLimiter = createEdgeLimiter({ burst, perSec, aggBurst, aggPerSec });
    edgeLimiter.cfg = cfg;
  }
  const scale = Number(env.RATE_LIMIT_SCALE) > 0 ? Number(env.RATE_LIMIT_SCALE) : 1;
  const wait = edgeLimiter.take(request.headers.get('cf-connecting-ip') || 'unknown', request.headers.get('authorization') || '', Date.now(), scale);
  if (wait <= 0) return null;
  return errorResponse(new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) }));
}

// Verdicts of the object (core/budget.js, app.js): when the object refuses a request it names how long the same refusal will stand in
// `x-msc-block: <kind>:<seconds>`; this isolate then answers the same caller itself until that time, so the rest of a flood costs Worker
// requests (which the platform counts whatever we do) but no Durable Object requests. A verdict is keyed by WHO is refused, with the
// address reduced to the object's own key (ipKey: the /64) and the account by the first characters of its token (never the whole secret):
//   open     (address)                    the open-route daily ceiling of the address
//   player   (address, account)           the daily ceiling of ordinary token traffic
//   live     (address, account)           the daily ceiling of a live room member's traffic
//   pid      (account)                    the account's own daily ceiling, wherever it comes from
//   bad      (address)                    the address's daily ceiling of requests that FAIL authentication: it refuses the address's requests
//                                         except those whose token this isolate has seen succeed (`good`: a NAT's honest consoles keep working)
//   reserve  (address, account)           a room or an address has used its share of the reserve (503 over_budget)
//   rl       (address, account, route)    a rate limit of the object (429 slow down)
// A request on a room route is a 'live' request when its caller is a member and a 'player' request when not: the object decides, the Worker
// cannot, so for those routes it consults both. Memory only, bounded.
/** @type {Map<string, number>} verdict key -> ms until which it stands */
const blocks = new Map();
const MAX_BLOCKS = 6000;
const VERDICT = /^(open|player|live|pid|bad|reserve|rl|adm):(\d{1,6})$/;
const tokenOf = (request) => {
  const a = request.headers.get('authorization') || '';
  return a.length >= 19 && a.startsWith('Bearer ') ? a.slice(7, 19) : '';
};
const addrOf = (request) => ipKey(request.headers.get('cf-connecting-ip') || 'unknown');
function verdictKey(kind, request, route) {
  switch (kind) {
    case 'open': return `open|${dailyAddrKey(addrOf(request))}`; // (sec34: the object's daily open / bad ceilings count the IPv6 /48, so the remembered verdict covers it too)
    case 'pid': return `pid|${tokenOf(request)}`;
    case 'bad': return `bad|${dailyAddrKey(addrOf(request))}`;
    case 'adm': return `adm|${addrOf(request)}`; // secreview32 (F1): admin routes, by address (a wrong bearer has no account to pin it on)
    case 'rl': return `rl|${addrOf(request)}|${tokenOf(request)}|${route}`;
    default: return `${kind}|${addrOf(request)}|${tokenOf(request)}`;
  }
}
function noteBlock(request, resp, route) {
  const v = resp.headers.get('x-msc-block');
  const m = v && VERDICT.exec(v);
  if (!m) return;
  if (m[1] !== 'open' && m[1] !== 'bad' && m[1] !== 'adm' && !tokenOf(request)) return; // no account to pin it on (a failed sign-in is the address's, token or none)
  if (blocks.size >= MAX_BLOCKS) {
    const now = Date.now();
    for (const [k, t] of blocks) if (t <= now) blocks.delete(k);
    if (blocks.size >= MAX_BLOCKS) blocks.delete(blocks.keys().next().value);
  }
  blocks.set(verdictKey(m[1], request, route), Date.now() + Number(m[2]) * 1000);
}
/** tokens (first characters, like the verdict keys) that got a good answer from the object on a route that CHECKS the token (class player / live, never
 *  an open route, which does not look at Authorization): they pass a `bad` block */
const good = new Set();
/** tokens the object refused as unknown (401) or that were over the failed-authentication ceiling: a known-bad token is refused here under a `bad` block
 *  without spending the allowance below. token -> ms until which it stands */
const badTokens = new Map();
/** Under a `bad` block an address still gets a few UNKNOWN tokens through to the object, so a genuinely registered console behind the same NAT
 *  (a household, a carrier's or a venue's shared address) can prove itself and be marked good, however many invented tokens the attack uses.
 *  A continuous refill per address (one slot per BAD_ALLOW_REFILL_MS, burst BAD_ALLOW_BURST), not a wall-clock minute: an attacker that spends the
 *  slots at every minute start cannot hold them all, a console that keeps retrying gets the next slot that comes free. address -> {tokens, at} */
const BAD_ALLOW_BURST = 3;
const BAD_ALLOW_REFILL_MS = 20_000;
const badAllow = new Map();
function badAllowance(addr, now) {
  let a = badAllow.get(addr);
  if (!a) {
    if (badAllow.size >= MAX_BLOCKS) badAllow.clear();
    a = { tokens: BAD_ALLOW_BURST, at: now };
    badAllow.set(addr, a);
  } else {
    a.tokens = Math.min(BAD_ALLOW_BURST, a.tokens + Math.max(0, now - a.at) / BAD_ALLOW_REFILL_MS);
    a.at = now;
  }
  if (a.tokens < 1) return false;
  a.tokens -= 1;
  return true;
}
/** Answers that prove the token authenticated: everything below 400, and the 4xx a route handler gives AFTER authentication (not found, conflict, gone).
 *  Never 401/403, and not the other 4xx / 5xx: a wrong version header (400, 426), a rate limit (429), a switched-off service (503) are answered BEFORE
 *  the token is looked at, so an invented token would get them too. */
const AFTER_AUTH_4XX = new Set([404, 409, 410]);
function noteAuth(request, resp, cls) {
  const t = tokenOf(request);
  if (!t) return;
  if (resp.status === 401 || /^bad:/.test(resp.headers.get('x-msc-block') || '')) {
    if (badTokens.size >= MAX_BLOCKS) badTokens.clear();
    badTokens.set(t, Date.now() + 3600_000); // an hour: a console that registers anew has a new token
  } else if ((resp.status < 400 || AFTER_AUTH_4XX.has(resp.status)) && (cls === 'player' || cls === 'live')) {
    if (good.size >= MAX_BLOCKS) good.clear();
    good.add(t);
  }
}
/** Which verdicts can apply to a request of this class (see the table above). */
const KINDS = { open: ['open', 'rl'], player: ['bad', 'player', 'pid', 'rl'], live: ['bad', 'live', 'player', 'reserve', 'pid', 'rl'] };
function blocked(request, cls, route, freeing) {
  const now = Date.now();
  for (const kind of KINDS[cls] || []) {
    if (freeing && kind === 'reserve') continue;
    if (kind === 'bad') {
      const t = tokenOf(request);
      if (t && good.has(t)) continue; // a token seen to succeed passes the failed-authentication block
      const until0 = blocks.get(verdictKey('bad', request, route));
      if (!until0) continue; // no verdict: nothing to do (an expired one is dropped below)
      if (until0 <= now) { blocks.delete(verdictKey('bad', request, route)); continue; }
      const bt = t && badTokens.get(t);
      if (t && !(bt && bt > now) && badAllowance(addrOf(request), now)) continue; // an unknown token: a few a minute reach the object to prove themselves
    }
    const k = verdictKey(kind, request, route);
    const until = blocks.get(k);
    if (!until) continue;
    if (until <= now) { blocks.delete(k); continue; }
    const wait = Math.max(1, Math.ceil((until - now) / 1000));
    if (kind === 'reserve') {
      return errorResponse(new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: wait, reason: 'room_share' }, { 'retry-after': String(wait) }));
    }
    const extra = kind === 'rl' ? { retryAfterSec: wait } : { retryAfterSec: wait, reason: 'daily_cap', scope: kind === 'pid' ? 'account' : 'address' };
    return errorResponse(new ApiError(429, 'rate_limited', kind === 'rl' ? 'slow down' : 'too many requests today', extra, { 'retry-after': String(wait) }));
  }
  return null;
}

const BOARD_PATH = '/v1/public/leaderboard';
/** @type {Map<string, {at: number, status: number, body: string, headers: [string, string][]}>} */
const boardMemo = new Map();

/** Canonical key (board, limit and archived season only, so extra query noise cannot dodge the memo), or null when the query is invalid (the object answers 400). */
function boardKey(url, smg) {
  try {
    return smg ? `t:${publicKey(parsePublicQuery(url.searchParams))}` : queryKey(parseQuery(url.searchParams));
  } catch {
    return null;
  }
}

function fromMemo(m) {
  const headers = new Headers(m.headers);
  headers.set('x-msc-cache', 'hit');
  return new Response(m.body, { status: m.status, headers });
}

// A memo hit must not outlive the kill switch or the per-address rate limit the object applies to this route (the object is where
// both live, and a hit never reaches it). So a hit is served only after (1) the static KILL_SWITCH variable and the isolate's view of
// the object's switches say the route is on, (2) a per-address bucket here (same numbers as the object's) lets it through
// (core/memo_guard.js). The object's switches are re-read (one cheap GET /v1/info, which is never switched off) at most every
// RECHECK_MS (60 s since svc3: it was 5 s, i.e. 12 object requests a minute per isolate; measured in test/core/svc3-quota.test.js:
// 600 hits in 600 s cost 120 object requests before, 10 now) per isolate and whenever a request misses the memo; when the route is off
// the memo is dropped and the request goes to the object, which answers with its own 503 (message included). An admin switch change
// through this Worker is seen at once by this isolate; other isolates see it within RECHECK_MS (their memo, at most 60 s old, is the
// same age bound).
const guard = createMemoGuard();

async function refreshKill(request, env, now) {
  try {
    const url = new URL(request.url);
    url.pathname = '/v1/info';
    url.search = '';
    // (the look at the switches is the Worker's own request: it carries a fixed internal address, which the object does not count against
    // the open-address ceiling; a client cannot send it, Cloudflare overwrites cf-connecting-ip)
    const info = new Request(url, { method: 'GET', headers: { 'x-msc-game': rawGame(env) === 'smg' ? 'smg' : 'msc', 'cf-connecting-ip': INTERNAL_ADDRESS } });
    const resp = await env.GAME.get(env.GAME.idFromName('main')).fetch(info);
    if (resp.status === 200) {
      const j = await resp.json();
      guard.seen(Array.isArray(j.kill) ? j.kill : [], now);
      return;
    }
  } catch { /* keep the last known state and try again shortly */ }
  guard.retrySoon(now);
}

async function publicBoard(request, env, smg) {
  const key = boardKey(new URL(request.url), smg);
  const now = Date.now();
  const killName = smg ? 'times' : 'ranking';
  const hit = key && boardMemo.get(key);
  if (hit && now - hit.at < (smg ? PUBLIC_MEMO_MS : MEMO_MS)) {
    if (guard.stale(now)) await refreshKill(request, env, now);
    if (guard.off(env, killName)) {
      boardMemo.clear(); // switched off: the object answers (503 with its message)
    } else {
      const wait = guard.take(env, killName, request.headers.get('cf-connecting-ip'), now);
      if (wait > 0) return errorResponse(new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) }));
      return fromMemo(hit);
    }
  }
  const resp = await forward(request, env);
  if (resp.status === 503) guard.forget(); // switched off (or overloaded): look again at the next hit
  if (key && resp.status === 200) {
    const body = await resp.clone().text();
    guard.routeOn(now); // the object just served this route, so it is not switched off
    if (boardMemo.size >= 300) boardMemo.clear();
    boardMemo.set(key, { at: now, status: 200, body, headers: [...resp.headers] });
  }
  return resp;
}

// The service's daily request budget (core/budget.js): the object answers "over_budget" with `x-msc-budget: over` (everything is spent) or
// `new-over` (new sessions are refused, members of a live room are still served from the reserve). This isolate then answers the
// refused requests itself until the time the object named (one request every 10 minutes still goes through, to notice a raised budget),
// so a spent budget costs no Durable Object requests. For `new-over` that means every request EXCEPT the room and relay routes a running
// session uses (requestClass 'live'). Every request here still counts as a Worker request: this protects the object's quota and makes
// the game's clients back off and give up; it cannot stop traffic from anyone else (README.md "Quota defence").
const over = { hard: { until: 0, probeAt: 0, body: null }, soft: { until: 0, probeAt: 0, body: null } };
function overAnswer(st, now) {
  if (!st.until) return null;
  if (now >= st.until) { st.until = 0; return null; }
  if (now >= st.probeAt) { st.probeAt = now + 600_000; return null; }
  const wait = Math.max(60, Math.ceil((st.until - now) / 1000));
  return new Response(st.body, { status: 503, headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store', 'retry-after': String(wait), 'x-msc-budget': 'over' } });
}
async function forward(request, env) {
  let resp;
  try {
    resp = await env.GAME.get(env.GAME.idFromName('main')).fetch(request);
  } catch (e) {
    // quota32 / fix33 (S2): the object refused the call. Only an error that NAMES a limit or quota (the free plan's daily Durable Object
    // limit: "Exceeded allowed ...", "... daily limit ...") is answered 503 over_budget (a long wait in the game). Every other throw (a
    // deploy resetting the object, overload, a transient internal error) is answered 503 `busy` with a short retry (3 s): both game
    // clients treat it as an ordinary transient and ask again within seconds, so a live room is not left silent for a minute. The
    // text match is a heuristic (Cloudflare documents no error code for it); an unrecognised quota error just costs a few more requests.
    const msg = String((e && e.message) || e || '');
    const quota = !(e && (e.overloaded || e.retryable)) && /exceeded|daily|quota|free tier/i.test(msg) && /limit|quota|allowed|free tier|volume/i.test(msg) && !/reset because|code was updated/i.test(msg);
    if (quota) return errorResponse(new ApiError(503, 'over_budget', 'online play is busy, please try again later', { retryAfterSec: 120, reason: 'object_unavailable' }, { 'retry-after': '120' }));
    return errorResponse(new ApiError(503, 'busy', 'online play is busy, please try again in a moment', { retryAfterSec: 3, reason: 'object_error' }, { 'retry-after': '3' }));
  }
  const flag = resp.headers.get('x-msc-budget');
  if (flag === 'over' || flag === 'new-over') {
    const wait = Number(resp.headers.get('retry-after')) || 600;
    const st = flag === 'over' ? over.hard : over.soft;
    st.until = Date.now() + wait * 1000;
    st.probeAt = Date.now() + 600_000;
    st.body = await resp.clone().text();
  }
  noteBlock(request, resp, routeKey(request, env));
  noteAuth(request, resp, requestClass(request, env));
  return resp;
}

export default {
  /** @param {Request} request @param {{GAME: DurableObjectNamespace}} env */
  async fetch(request, env) {
    const err = edgeCheck(request, env);
    if (err) {
      if (env.LATEST_APP_VERSION) err.extra.latestVersion = String(env.LATEST_APP_VERSION);
      return errorResponse(err);
    }
    const path = new URL(request.url).pathname;
    const cls = requestClass(request, env);
    if (cls === 'admin') {
      // secreview32 (F1): the object's refusal of this address's admin requests (too many wrong bearers, or too many admin calls a minute)
      // stands here for the time it named: the rest of a guessing loop never wakes the object.
      const k = verdictKey('adm', request, null);
      const until = blocks.get(k);
      if (until) {
        const now = Date.now();
        if (until <= now) blocks.delete(k);
        else {
          const wait = Math.max(1, Math.ceil((until - now) / 1000));
          return errorResponse(new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait) }));
        }
      }
    }
    if (cls && cls !== 'admin') {
      const now = Date.now();
      const freeing = isFreeing(request, env); // leave / close free resources: a spent budget never turns them away
      const hard = !freeing && over.hard.until && overAnswer(over.hard, now);
      if (hard) return hard;
      const soft = !freeing && over.soft.until && cls !== 'live' && !isMaybeLive(request, env) && overAnswer(over.soft, now);
      if (soft) return soft;
      const refused = blocked(request, cls, routeKey(request, env), freeing);
      if (refused) return refused;
    }
    if (path !== '/times' && path !== '/leaderboard') {
      const slow = edgeLimit(request, env); // before anything that wakes the object
      if (slow) return slow;
    }
    if (path === '/v1/admin/switches') { // an admin changed the switches: this isolate looks again at the next public hit
      const resp = await forward(request, env);
      guard.forget();
      boardMemo.clear();
      return resp;
    }
    if (rawGame(env) === 'smg') { // Galaxy (smg-online): the times page and its data instead of the Strikers board
      if (path === '/times') return timesPageResponse();
      if (path === '/v1/public/times') return publicBoard(request, env, true);
      return forward(request, env);
    }
    if (path === '/leaderboard') return pageResponse();
    if (path === BOARD_PATH) return publicBoard(request, env, false);
    return forward(request, env);
  },
};
