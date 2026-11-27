// The whole service as one function: Request -> Response, over an abstract SQL handle.
// Used unchanged by the Durable Object (production), by the vitest suites and by the Node fake server.

import { ApiError, json, errorResponse, sha256hex, safeEqual, envInt, noBody, cmpVersion, ipKey, dailyAddrKey, hex, randomBytes, te, strict, readBodyCapped } from './util.js';
import { loadConfig, PROTO, killNamesFor } from './config.js';
import { migrate } from './schema.js';
import { RateLimiter } from './ratelimit.js';
import { dayIndex, secondsToMidnight } from './limits.js';
import { createFill } from './fill.js';
import { createBudget, CLASS_OPEN, CLASS_PLAYER, CLASS_LIVE, LIVE_PATTERNS, MAYBE_LIVE, INTERNAL_ADDRESS } from './budget.js';
import { versionHeaderError, compilePattern, gameHeaderError, bodyCapFor } from './edge.js';
import * as pow from './pow.js';
import { dateParts, dayStart, season } from './calendar.js';
import { NAME_MIN, NAME_MAX } from './names.js';
import * as accounts from './accounts.js';
import * as rooms from './rooms.js';
import * as matches from './matches.js';
import * as ranked from './ranked.js';
import * as ranking from './ranking.js';
import * as publicboard from './publicboard.js';
import { pageResponse } from './leaderboard_page.js';
import * as turn from './turn.js';
import * as admin from './admin.js';
import { archiveSeasons } from './archive.js';
import * as rooms4 from './rooms4.js';
import * as invites from './invites.js';
import * as friends from './friends.js';
import * as times from './times.js';
import * as reports from './reports.js';
import * as summaries from './summaries.js';
import { timesPageResponse } from './times_page.js';

/**
 * @param {object} deps
 * @param {{exec: (q: string, ...b: any[]) => {toArray(): any[]}}} deps.sql
 * @param {(fn: () => any) => any} deps.tx       synchronous transaction wrapper
 * @param {Record<string, any>} [deps.env]       wrangler vars + secrets
 * @param {() => number} [deps.now]              clock in ms (tests inject their own)
 * @param {typeof fetch} [deps.fetchImpl]        for the TURN provider
 * @param {(ms: number) => Promise<void>} [deps.sleep]  the TURN revoke retry wait (tests inject a no-wait one)
 * @param {import('./turn.js').TurnProvider|null} [deps.turnProvider]
 */
/** The network a client address belongs to, for the per-network registration cap: IPv4 /24, IPv6 /48 (ipKey gives /64). */
export function netKey(ipk) {
  const s = String(ipk);
  if (s.startsWith('v6:')) return s.split(':').slice(0, 4).join(':');
  const m = /^(\d{1,3}\.\d{1,3}\.\d{1,3})\.\d{1,3}$/.exec(s);
  return m ? m[1] : s;
}

export function createApp(deps) {
  const env = deps.env || {};
  const cfg = loadConfig(env);
  for (const w of cfg.warnings || []) console.warn(`config: ${w}`);
  const clock = deps.now || (() => Date.now());
  const limiter = deps.limiter || new RateLimiter();
  // wrong-token counters live in their own map so no flood of ordinary keys can ever evict them (see ratelimit.js)
  const wrongBlock = new Map(); // address key -> ms until which wrong room codes are refused (svcfix2 item 8)
  const brake = deps.brake || new RateLimiter(50000, { protect: true });
  const powUsed = new Map();
  const regSeen = deps.regSeen || new Map(); // ipk -> registration times of the last 24 h (pow.js bitsFor), memory only
  const tx = deps.tx;
  // Every statement goes through here, so the day's SQLite rows written (the free plan's 100,000 a day, account-wide) are counted where the code writes:
  // the cursor's own `rowsWritten` on the platform; elsewhere (the node test adapter) one row per write statement (an estimate, marked as such in the stats).
  let rowsSink = null;
  const WRITE_RE = /^\s*(INSERT|UPDATE|DELETE|REPLACE)\b/i;
  const exec = (q, b) => {
    const cur = deps.sql.exec(q, ...b);
    const rows = cur.toArray();
    if (rowsSink) {
      if (typeof cur.rowsWritten === 'number') { if (cur.rowsWritten > 0) rowsSink(cur.rowsWritten, false); }
      else if (WRITE_RE.test(q)) rowsSink(1, true);
    }
    return rows;
  };
  const db = {
    all: (q, ...b) => exec(q, b),
    get: (q, ...b) => exec(q, b)[0] ?? null,
    run: (q, ...b) => { exec(q, b); },
  };
  const fill = createFill(db, cfg);
  const presence = deps.presence || new Map();
  const direct = deps.direct || new Map(); // svcfix2: `${room}:${slot}` -> { at, mask } the member's report of its direct heartbeat links (rooms.seenAt)
  const PRUNE_EVERY_MS = 3600_000;
  let lastPruneMs = 0;
  let lastBetaSweep = 0;
  // GAME=smg: pid -> ms of the player's last authenticated call (friends' "online"); memory only, pruned by the sweep
  const online = deps.online || new Map();
  const game = cfg.game;
  const smg = game === 'smg';
  const KILL_NAMES = killNamesFor(game);
  const MAX_BODY = cfg.maxBody;
  const board = publicboard.createBoard();
  const pubTimes = times.createPublicTimes();
  const timesMemo = times.createTimesMemo();
  const turnProvider = deps.turnProvider !== undefined ? deps.turnProvider : turn.createProvider(cfg.turn, deps.fetchImpl, deps.sleep);

  migrate(deps.sql, clock(), deps.tx);
  const turnGuard = turnProvider ? turn.createUsageGuard(cfg.turn, deps.fetchImpl || ((...a) => fetch(...a))) : null;

  let settings = null;
  const loadSettings = () => {
    if (!settings) {
      const rows = db.all(`SELECT key, value FROM settings WHERE key IN ('kill','message')`);
      const m = Object.fromEntries(rows.map((r) => [r.key, r.value]));
      let kill = [];
      try { kill = JSON.parse(m.kill || '[]'); } catch { kill = []; }
      settings = { kill: new Set([...cfg.killEnv, ...kill]), message: m.message || cfg.maintenanceMessage };
    }
    return settings;
  };

  function ctxFor(now) {
    return {
      db, tx, now, cfg, reportsDb: env.REPORTS_DB || null, turn: turnProvider, turnGuard, presence, direct, powUsed, regSeen, online, killNames: KILL_NAMES,
      killSwitches: () => loadSettings().kill,
      invalidateSettings: () => { settings = null; },
      budgetStatus: () => bud.status(now),
      fill,
      timesMemo,
      clearBoard: () => { board.clear(); pubTimes.clear(); timesMemo.clear(); }, // a ban or an account deletion must leave the public leaderboard quickly
    };
  }

  // ------------------------------------------------------------ rate limiting
  function tooMany(wait) {
    // x-msc-block: the Worker answers the same (address, account, route) itself for `wait` seconds instead of asking the object again
    return new ApiError(429, 'rate_limited', 'slow down', { retryAfterSec: wait }, { 'retry-after': String(wait), ...(wait <= 3600 ? { 'x-msc-block': `rl:${wait}` } : {}) });
  }
  function limit(c, key, cap, windowSec, now, burst) {
    const capScaled = Math.max(1, Math.ceil(cap * cfg.rateScale));
    const burstScaled = burst === undefined ? capScaled : Math.max(capScaled, Math.ceil(burst * cfg.rateScale));
    const wait = limiter.take(key, capScaled, windowSec, now, burstScaled);
    if (wait > 0) throw tooMany(wait);
  }
  /** The brake limiter: wrong-token counters. Never scaled by RATE_LIMIT_SCALE (tests that want it exercise the real numbers). */
  function brakeTake(key, cap, windowSec, now) {
    const wait = brake.take(key, cap, windowSec, now);
    if (wait > 0) throw tooMany(wait);
  }

  // ------------------------------------------------------------ routes
  // [method, pattern, options, handler]. options: auth, kill (switch name), rl: [name, cap, windowSec, scope]
  const R = (method, pattern, opts, handler) => ({ method, pattern, re: compilePattern(pattern), opts, handler });
  const mscRoutes = () => [
    R('GET', '/v1/info', { rl: ['info', 120, 60, 'ip'], open: true }, (c) => info(c)),
    R('GET', '/v1/servers', { rl: ['servers', 60, 60, 'ip'], open: true }, (c) => serverList(c)),
    R('GET', '/v1/time', { rl: ['time', 120, 60, 'ip'], open: true }, (c) => timeInfo(c.now)),

    R('GET', '/v1/register/challenge', { kill: 'register', rl: ['regchallenge', 60, 60, 'ip'] }, (c, r) => pow.issueChallenge(c, r.ipk)),
    // registration: the body is validated first and only a request that can succeed is charged to the per-address and global buckets (see prepareRegister)
    R('POST', '/v1/register', { kill: 'register', register: true }, (c, r) => accounts.register(c, r.body)),
    R('GET', '/v1/me', { auth: true, rl: ['me', 60, 60, 'pid'] }, (c, r) => accounts.me(c, r.player)),
    R('PUT', '/v1/me', { auth: true, rl: ['rename', 6, 3600, 'pid'] }, (c, r) => accounts.rename(c, r.player, r.body)),
    R('DELETE', '/v1/me', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, (c, r) => { noBody(r.body); return accounts.deleteAccount(c, r.player); }),
    R('GET', '/v1/me/matches', { auth: true, rl: ['mymatches', 30, 60, 'pid'] }, (c, r) => ({
      matches: matches.listMyMatches(c, r.player, Math.min(50, Math.max(1, envInt({ n: r.query.get('limit') }, 'n', 10)))),
    })),

    R('GET', '/v1/players/:code', { auth: true, rl: ['bycode', 30, 60, 'pid'] }, (c, r) => accounts.byCode(c, r.params.code)),

    R('POST', '/v1/rooms', { auth: true, kill: 'rooms', rl: ['roomnew', 10, 600, 'pid'] }, (c, r) => { noBody(r.body); return rooms.createRoom(c, r.player); }),
    R('POST', '/v1/rooms/join', { auth: true, kill: 'rooms', rl: ['roomjoin', 20, 600, 'pid'] }, (c, r) => rooms.joinRoom(c, r.player, r.body)),
    R('GET', '/v1/rooms/:code', { auth: true, rl: ['roomget', 150, 60, 'pid'] }, (c, r) => {
      const v = r.query.get('v');
      return rooms.getRoom(c, r.player, r.params.code, v === null ? undefined : Number(v));
    }),
    R('POST', '/v1/rooms/:code/endpoint', { auth: true, rl: ['roomep', 20, 60, 'pid'] }, (c, r) => rooms.postEndpoint(c, r.player, r.params.code, r.body)),
    R('POST', '/v1/rooms/:code/punch', { auth: true, rl: ['roompunch', 20, 60, 'pid'] }, (c, r) => rooms.reportPunch(c, r.player, r.params.code, r.body)),
    R('POST', '/v1/rooms/:code/started', { auth: true, rl: ['roomstart', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return rooms.markStarted(c, r.player, r.params.code); }),
    R('POST', '/v1/rooms/:code/leave', { auth: true, rl: ['roomleave', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return rooms.leaveRoom(c, r.player, r.params.code); }),
    R('DELETE', '/v1/rooms/:code', { auth: true, rl: ['roomleave', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return rooms.leaveRoom(c, r.player, r.params.code); }),

    R('POST', '/v1/ranked/queue', { auth: true, kill: 'ranked', rl: ['qjoin', 10, 600, 'pid'] }, (c, r) => { noBody(r.body); return ranked.joinQueue(c, r.player); }),
    R('POST', '/v1/ranked/queue/resume', { auth: true, kill: 'ranked', rl: ['qresume', 20, 600, 'pid'] }, (c, r) => ranked.resumeQueue(c, r.player, r.body)),
    R('GET', '/v1/ranked/queue', { auth: true, rl: ['qpoll', 120, 60, 'pid'] }, (c, r) => ranked.pollQueue(c, r.player)),
    R('DELETE', '/v1/ranked/queue', { auth: true, rl: ['qleave', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return ranked.leaveQueue(c, r.player); }),

    R('POST', '/v1/matches/:id/result', { auth: true, rl: ['mresult', 20, 600, 'pid'] }, (c, r) => matches.submitResult(c, r.player, r.params.id, r.body)),
    R('GET', '/v1/matches/:id', { auth: true, rl: ['mget', 60, 60, 'pid'] }, (c, r) => matches.getMatch(c, r.player, r.params.id)),

    R('POST', '/v1/turn', { auth: true, kill: 'turn', rl: ['turn', 10, 600, 'pid'], async: true }, (c, r) => turn.mintTurn(c, r.player, r.body)),
    R('POST', '/v1/turn/usage', { auth: true, kill: 'turn', rl: ['turnuse', 30, 600, 'pid'] }, (c, r) => turn.reportTurnUsage(c, r.player, r.body)),

    R('POST', '/v1/rank/put', { auth: true, kill: 'ranking', rl: ['rankput', 12, 60, 'pid'] }, (c, r) => ranking.putScore(c, r.player, r.body)),
    R('POST', '/v1/rank/get', { auth: true, kill: 'ranking', rl: ['rankget', 40, 60, 'pid'] }, (c, r) => ranking.getScores(c, r.player, r.body)),

    // Public, read-only web leaderboard: no token and no version header (a browser sends neither); the `ranking` / `all` kill switch applies.
    R('GET', '/v1/public/leaderboard', { open: true, web: true, kill: 'ranking', rl: ['pubboard', 30, 60, 'ip'] }, (c, r) => {
      const body = board.get(c, publicboard.parseQuery(r.query));
      return json(200, body, { 'cache-control': `public, max-age=${publicboard.MEMO_MS / 1000}` });
    }),
    R('GET', '/leaderboard', { open: true, rl: ['lbpage', 60, 60, 'ip'] }, () => pageResponse()),

    R('GET', '/v1/admin/stats', { admin: true }, (c) => admin.stats(c)),
    R('GET', '/v1/admin/export', { admin: true }, (c, r) => admin.exportTable(c, r.query)),
    R('POST', '/v1/admin/import', { admin: true }, (c, r) => admin.importRows(c, r.body)),
    R('POST', '/v1/admin/switches', { admin: true }, (c, r) => admin.setSwitches(c, r.body)),
    R('POST', '/v1/admin/ban', { admin: true }, (c, r) => admin.ban(c, r.body)),
    R('GET', '/v1/admin/flags', { admin: true }, (c, r) => admin.listFlags(c, r.query)),
    R('POST', '/v1/admin/flags/clear', { admin: true }, (c, r) => admin.clearFlags(c, r.body)),
  ];

  // GAME=smg (Galaxy 3.0): SMG-PROTOCOL.txt. Same accounts, TURN and admin; rooms of up to 4; no DWC ranking or ranked queue.
  const ver = (r) => rooms4.versionTag(r);
  /** svcfix2 item 8 / code5: the address is blocked for a while after too many wrong codes (rooms and invites share the allowance). */
  const wrongCheck = (r, c) => {
    const hit = wrongBlock.get(r.ipk);
    if (hit && hit > c.now) throw tooMany(Math.ceil((hit - c.now) / 1000));
  };
  const wrongCharge = (r, c) => {
    const w = Math.max(brake.take(`wrongcode:${r.ipk}`, 10, 600, c.now), brake.take(`wrongcodeday:${r.ipk}`, c.cfg.rooms.wrongCodesPerDay, 86400, c.now));
    if (w > 0) { wrongBlock.set(r.ipk, c.now + w * 1000); if (wrongBlock.size > 5000) wrongBlock.clear(); throw tooMany(w); }
  };
  /** Leave / close with the optional {"summary"} (summaries.js): the leave happens first; a summary that cannot be stored never fails it. */
  async function leaveWithSummary(c, r) {
    const summary = summaries.summaryOf(r.body);
    const who = { pid: r.player.pid, code: String(r.params.code).toUpperCase() };
    let out;
    try {
      out = rooms4.leaveRoom(c, r.player, r.params.code);
    } catch (e) {
      // removed before it sent its summary (kicked, host lost): the member row is gone and the call is refused, but the summary of that session is still wanted from a former member
      if (summary !== undefined && e instanceof ApiError && e.status === 403 && rooms4.formerMember(c, r.player, r.params.code)) await summaries.recordSummary(c, summary, r.app, who);
      throw e;
    }
    // only a member (or a member of a room that has since closed) records one; once per player and room and the daily caps meter it (recordSummary), so it is no free D1 write
    if (summary !== undefined && (out.removedMember === true || out.wasMember === true)) await summaries.recordSummary(c, summary, r.app, who);
    return out;
  }
  /**
   * GET /v1/prove?n=<32 lowercase hex>: the deploy script's proof that an address is bound to THIS service before it sends that address the admin token.
   * proof = HMAC-SHA256(key = ADMIN_TOKEN, "smg-online prove v1\n" + the request's host name, lowercased + "\n" + n), 64 hex. Open, no token; 404 exactly like the admin routes when
   * ADMIN_TOKEN is missing or too short. Neither n nor the proof is ever logged. The work is the same for every n.
   */
  const prove = async (c, r) => {
    if (!cfg.adminEnabled) throw new ApiError(404, 'not_found', 'no such endpoint');
    const n = r.query.get('n');
    if (typeof n !== 'string' || !/^[0-9a-f]{32}$/.test(n)) throw new ApiError(400, 'bad_request', 'n must be 32 lowercase hex digits');
    const enc = new TextEncoder();
    const key = await crypto.subtle.importKey('raw', enc.encode(cfg.adminToken), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const mac = new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode(`smg-online prove v1\n${r.host}\n${n}`)));
    return { proof: [...mac].map((b) => b.toString(16).padStart(2, '0')).join('') };
  };
  const smgRoutes = () => [
    R('GET', '/v1/info', { rl: ['info', 120, 60, 'ip'], open: true }, (c) => info(c)),
    R('GET', '/v1/prove', { rl: ['prove', 10, 60, 'ip'], open: true }, prove),
    R('GET', '/v1/servers', { rl: ['servers', 60, 60, 'ip'], open: true }, (c) => serverList(c)),
    R('GET', '/v1/time', { rl: ['time', 120, 60, 'ip'], open: true }, (c) => timeInfo(c.now)),
    R('GET', '/v1/register/challenge', { kill: 'register', rl: ['regchallenge', 60, 60, 'ip'] }, (c, r) => pow.issueChallenge(c, r.ipk)),
    R('POST', '/v1/register', { kill: 'register', register: true }, async (c, r) => {
      const out = await accounts.register(c, r.body);
      pow.noteRegistered(c, r.ipk);
      return out;
    }),
    R('GET', '/v1/me', { auth: true, rl: ['me', 60, 60, 'pid'] }, (c, r) => accounts.me(c, r.player)),
    R('PUT', '/v1/me', { auth: true, rl: ['rename', 6, 3600, 'pid'] }, (c, r) => accounts.rename(c, r.player, r.body)),
    R('DELETE', '/v1/me', { auth: true, rl: ['delete', 3, 3600, 'pid'] }, async (c, r) => { noBody(r.body); const out = accounts.deleteAccount(c, r.player); await reports.drainPurges(c); return out; }),
    R('GET', '/v1/players/:code', { auth: true, rl: ['bycode', 30, 60, 'pid'] }, (c, r) => accounts.byCode(c, r.params.code)),

    R('POST', '/v1/rooms', { auth: true, kill: 'rooms', rl: ['roomnew', 10, 600, 'pid'] }, (c, r) => rooms4.createRoom(c, r.player, r.body, ver(r), r.ipk)),
    R('POST', '/v1/rooms/join', { auth: true, kill: 'rooms', rl: ['roomjoin', 20, 600, 'pid'] }, (c, r) => {
      // svcfix2 item 8: wrong codes are charged to the ADDRESS (not the account: accounts are cheap), so guessing room codes costs 40 a day
      wrongCheck(r, c);
      try {
        return rooms4.joinRoom(c, r.player, r.body, ver(r), r.ipk);
      } catch (e) {
        if (e instanceof ApiError && e.status === 404 && e.code === 'room_not_found' && r.body && r.body.code !== undefined) wrongCharge(r, c);
        throw e;
      }
    }),
    R('POST', '/v1/rooms/:code/accept', { auth: true, kill: 'rooms', rl: ['roomacc', 30, 60, 'pid'] }, (c, r) => rooms4.acceptMember(c, r.player, r.params.code, r.body)),
    R('GET', '/v1/rooms/public', { auth: true, kill: 'rooms', rl: ['roomlist', 12, 60, 'pid'] }, (c, r) => rooms4.publicRooms(c, r.player, r.query, ver(r))),
    R('GET', '/v1/rooms/:code', { auth: true, rl: ['roomget', 150, 60, 'pid'] }, async (c, r) => {
      const v = r.query.get('v');
      const since = v === null ? undefined : Number(v);
      const code0 = rooms.normalizeCode(r.params.code);
      const v0 = roomVersion(code0);
      // svcfix2 (`p=1`): the client wants the service's poll plan (rooms4.pollPlan) and may report its direct links (`l=<hex digit>`, bit n = slot n)
      const l = r.query.get('l');
      const opts = r.query.get('p') === '1' ? { plan: true, mask: l !== null && /^[0-9a-f]$/.test(l) ? parseInt(l, 16) : undefined } : {};
      const first = rooms4.getRoom(c, r.player, r.params.code, since, opts);
      if (v0 !== null && roomVersion(code0) !== v0) wakeRoom(code0); // this poll moved the host role on (a lost host): tell the others
      // Held poll (service review, request budget): ?v=<version>&wait=<1..60 s>. Nothing changed: the answer waits until the room's version
      // changes (another member's call) or the time is up, then answers as usual (presence is noted again at the end). One request per
      // `wait` instead of one per 1.7 s, and the change arrives at once. Old clients send no `wait` and see no difference.
      // S2: presence is stamped every HELD_TICK_MS (4 s) while the poll is held, so a long hold does not look like silence (LINK_PING_SEC 12,
      // LINK_LOST_SEC 20). The longest hold is the service's own plan (pollPlan: 25 s forming, 55 s for a settled host); a client that does not
      // ask for the plan (old) is held at most HELD_MAX_SEC (8 s, 11 s while the budget asks for pacing).
      const wait = Number(r.query.get('wait'));
      if (first.changed !== false || first.peersQuiet || !(wait >= 1 && wait <= (opts.plan ? 60 : 20)) || since === undefined) return withPace(first);
      // quota32: a backed-off hold (longer than the busy base) ends early when a peer goes quiet, so a lost host is still noticed within ~4 s of
      // the 12 s quiet limit and the takeover (checkHostLost, 20 s) is not later than with the old 8 s polls.
      const longHold = first.poll && first.poll.holdSec > cfg.rooms.poll.busyHoldSec && first.poll.holdSec <= cfg.rooms.poll.busyMaxHoldSec && first.poll.nextSec < 1 && first.state === 'started'; // fix33 (S1): the backed-off busy plan only (nextSec 0.3), never the settled host's 55 s plan
      const tick = () => { const cx = ctxFor(clock()); rooms4.holdPresence(cx, r.player.pid, first.code); return longHold && rooms4.peersQuietNow(cx, r.player.pid, first.code); };
      const cap = first.poll ? first.poll.holdSec : bud.pace() > 1 ? HELD_MAX_PACED_SEC : HELD_MAX_SEC;
      if (!cap || !(await waitForRoom(first.code, Math.min(Math.floor(wait), cap) * 1000, r.player.pid, tick))) return withPace(first);
      return withPace(rooms4.getRoom(ctxFor(clock()), r.player, r.params.code, since, opts));
    }),
    R('POST', '/v1/rooms/:code/endpoint', { auth: true, rl: ['roomep', 20, 60, 'pid'] }, (c, r) => rooms4.postEndpoint(c, r.player, r.params.code, r.body, r.ipk)),
    R('POST', '/v1/rooms/:code/punch', { auth: true, rl: ['roompunch', 30, 60, 'pid'] }, (c, r) => rooms4.reportPunch(c, r.player, r.params.code, r.body)),
    R('POST', '/v1/rooms/:code/started', { auth: true, rl: ['roomstart', 20, 60, 'pid'] }, (c, r) => { noBody(r.body); return rooms4.markStarted(c, r.player, r.params.code); }),
    // leave / close: the body may carry {"summary": {...}} (summaries.js), the beta's session summary
    R('POST', '/v1/rooms/:code/leave', { auth: true, rl: ['roomleave', 20, 60, 'pid'] }, (c, r) => leaveWithSummary(c, r)),
    R('DELETE', '/v1/rooms/:code', { auth: true, rl: ['roomleave', 20, 60, 'pid'] }, (c, r) => leaveWithSummary(c, r)),
    // code5: far-away invites (invites.js). A wrong 5-character code, and a probe of the host's registration at a live code, cost the SAME
    // per-address allowance as a wrong room code: one allowance for every kind of guessing.
    R('POST', '/v1/invites', { auth: true, kill: 'rooms', rl: ['invnew', 10, 600, 'pid'] }, (c, r) => {
      wrongCheck(r, c);
      return invites.registerInvite(c, r.player, r.body, r.app, () => wrongCharge(r, c));
    }),
    R('POST', '/v1/invites/resolve', { auth: true, kill: 'rooms', rl: ['invres', 20, 600, 'pid'] }, (c, r) => {
      wrongCheck(r, c);
      try {
        return invites.resolveInvite(c, r.player, r.body, r.app);
      } catch (e) {
        if (e instanceof ApiError && e.status === 404 && e.code === 'invite_not_found') wrongCharge(r, c);
        throw e;
      }
    }),
    R('POST', '/v1/invites/:code/keep', { auth: true, kill: 'rooms', rl: ['invkeep', 20, 600, 'pid'] }, (c, r) => { noBody(r.body); return invites.keepInvite(c, r.player, r.params.code); }),
    R('POST', '/v1/invites/:code/close', { auth: true, kill: 'rooms', rl: ['invclose', 30, 600, 'pid'] }, (c, r) => { noBody(r.body); return invites.closeOwnInvite(c, r.player, r.params.code); }),
    R('DELETE', '/v1/invites/:code', { auth: true, kill: 'rooms', rl: ['invclose', 30, 600, 'pid'] }, (c, r) => { noBody(r.body); return invites.closeOwnInvite(c, r.player, r.params.code); }),
    R('POST', '/v1/quick', { auth: true, kill: 'quick', rl: ['quick', 10, 600, 'pid'] }, (c, r) => rooms4.quick(c, r.player, r.body, ver(r), r.ipk)),

    R('GET', '/v1/friends', { auth: true, kill: 'friends', rl: ['friends', 6, 60, 'pid'] }, (c, r) => friends.listFriends(c, r.player, ver(r))),
    R('POST', '/v1/friends', { auth: true, kill: 'friends', rl: ['friendadd', 20, 3600, 'pid'] }, (c, r) => friends.addFriend(c, r.player, r.body)),
    R('POST', '/v1/friends/remove', { auth: true, kill: 'friends', rl: ['friendrm', 20, 3600, 'pid'] }, (c, r) => {
      strict(r.body, ['code']);
      return friends.removeFriend(c, r.player, r.body.code);
    }),
    R('POST', '/v1/friends/dismiss', { auth: true, kill: 'friends', rl: ['frienddis', 60, 3600, 'pid'] }, (c, r) => friends.dismissIncoming(c, r.player, r.body)),
    R('POST', '/v1/friends/block', { auth: true, kill: 'friends', rl: ['friendblock', 30, 3600, 'pid'] }, (c, r) => friends.blockPlayer(c, r.player, r.body)),
    R('POST', '/v1/friends/unblock', { auth: true, kill: 'friends', rl: ['friendunblock', 30, 3600, 'pid'] }, (c, r) => friends.unblockPlayer(c, r.player, r.body)),
    R('DELETE', '/v1/friends/:code', { auth: true, kill: 'friends', rl: ['friendrm', 20, 3600, 'pid'] }, (c, r) => {
      noBody(r.body);
      return friends.removeFriend(c, r.player, r.params.code);
    }),

    // per account (and stored: times.js), and per address across accounts (finding 13: many accounts behind one address)
    R('POST', '/v1/times', { auth: true, kill: 'times', rl: ['timeput', 30, 3600, 'pid'], rlIp: ['timeputip', 60, 3600] }, (c, r) => times.submitTime(c, r.player, r.body)),
    R('POST', '/v1/times/get', { auth: true, kill: 'times', rl: ['timeget', 40, 60, 'pid'] }, (c, r) => times.getTimes(c, r.player, r.body)),

    R('POST', '/v1/turn', { auth: true, kill: 'turn', rl: ['turn', 10, 600, 'pid'], async: true }, (c, r) => turn.mintTurn(c, r.player, r.body)),
    R('POST', '/v1/turn/usage', { auth: true, kill: 'turn', rl: ['turnuse', 30, 600, 'pid'] }, (c, r) => turn.reportTurnUsage(c, r.player, r.body)),
    // "Check Connection": one relay pass (300 s, tag "check") for a loopback test on one console, no room needed (turn.js checkTurn)
    R('POST', '/v1/turn/check', { auth: true, kill: 'turn', rl: ['turncheck', 6, 600, 'pid'], async: true }, (c, r) => { noBody(r.body); return turn.checkTurn(c, r.player, r.ipk); }),
    // the beta's problem report, sent with the player's consent (reports.js); its own, larger body cap (edge.js ROUTE_BODY_CAPS)
    R('POST', '/v1/report', { auth: true, kill: 'report', rl: ['report', 10, 3600, 'pid'] }, async (c, r) => json(201, await reports.submitReport(c, r.player, r.body, r.ipk, r.app, [r.token]))),

    R('GET', '/v1/public/times', { open: true, web: true, kill: 'times', rl: ['pubtimes', 30, 60, 'ip'] }, (c, r) => {
      const body = pubTimes.get(c, times.parsePublicQuery(r.query));
      return json(200, body, { 'cache-control': `public, max-age=${times.PUBLIC_MEMO_MS / 1000}` });
    }),
    R('GET', '/times', { open: true, rl: ['timespage', 60, 60, 'ip'] }, () => timesPageResponse()),

    R('POST', '/v1/admin/turn-selfcheck', { admin: true }, (c, r) => { noBody(r.body); return turn.turnSelfCheck(c); }),
    R('GET', '/v1/admin/stats', { admin: true }, (c) => admin.stats(c)),
    R('GET', '/v1/admin/export', { admin: true }, (c, r) => admin.exportTable(c, r.query)),
    R('POST', '/v1/admin/import', { admin: true }, (c, r) => admin.importRows(c, r.body)),
    R('POST', '/v1/admin/switches', { admin: true }, (c, r) => admin.setSwitches(c, r.body)),
    R('POST', '/v1/admin/ban', { admin: true }, (c, r) => admin.ban(c, r.body)),
    R('GET', '/v1/admin/flags', { admin: true }, (c, r) => admin.listFlags(c, r.query)),
    R('POST', '/v1/admin/flags/clear', { admin: true }, (c, r) => admin.clearFlags(c, r.body)),
    R('GET', '/v1/admin/times', { admin: true }, (c, r) => admin.listTimes(c, r.query)),
    R('POST', '/v1/admin/times/remove', { admin: true }, (c, r) => admin.removeTimes(c, r.body)),
    R('POST', '/v1/admin/times/approve', { admin: true }, (c, r) => admin.approveTime(c, r.body)),
    R('POST', '/v1/admin/times/ban', { admin: true }, (c, r) => admin.boardBan(c, r.body)),
    R('GET', '/v1/admin/reports', { admin: true }, (c, r) => reports.listReports(c, r.query)),
    R('GET', '/v1/admin/reports/:id', { admin: true }, (c, r) => reports.getReport(c, r.params.id)),
    R('GET', '/v1/admin/summaries', { admin: true }, (c) => summaries.listSummaries(c)),
  ];
  const routes = smg ? smgRoutes() : mscRoutes();

  // ------------------------------------------------------------ held polls (GET /v1/rooms/:code?v=&wait=)
  /** room code -> resolvers of the polls waiting for it to change. Memory only; a restarted object simply has none. */
  const waiters = new Map();
  const MAX_WAITERS = deps.maxWaiters || 2000;
  const MAX_WAITERS_PER_PID = deps.maxWaitersPerPid || 4;
  const HELD_MAX_SEC = 8;
  const HELD_MAX_PACED_SEC = 11;
  const HELD_TICK_MS = deps.heldTickMs || 4000;
  const waitersByPid = new Map();
  let nWaiters = 0;
  /** Resolves true when woken (the room may have changed) or false at the timeout. false at once when too many are waiting already. */
  function waitForRoom(code, ms, pid, tick) {
    if (nWaiters >= MAX_WAITERS || (waitersByPid.get(pid) || 0) >= MAX_WAITERS_PER_PID) return Promise.resolve(false);
    return new Promise((resolve) => {
      waitersByPid.set(pid, (waitersByPid.get(pid) || 0) + 1);
      let set = waiters.get(code);
      if (!set) { set = new Set(); waiters.set(code, set); }
      nWaiters++;
      const done = (woken) => {
        if (!set.delete(entry)) return;
        nWaiters--;
        if ((waitersByPid.get(pid) || 0) <= 1) waitersByPid.delete(pid); else waitersByPid.set(pid, waitersByPid.get(pid) - 1);
        clearInterval(ticker);
        if (!set.size && waiters.get(code) === set) waiters.delete(code);
        clearTimeout(timer);
        resolve(woken);
      };
      const entry = { done };
      const timer = setTimeout(() => done(false), ms);
      const ticker = setInterval(() => { try { if (tick() === true) done(true); } catch { /* presence is best effort */ } }, HELD_TICK_MS);
      set.add(entry);
    });
  }
  function wakeRoom(code) {
    const set = code && waiters.get(code);
    if (set) for (const e of [...set]) e.done(true);
  }
  const roomVersion = (code) => {
    if (!code || !waiters.has(code)) return null;
    const row = db.get(`SELECT version FROM rooms WHERE code = ?`, code);
    return row ? row.version : -1;
  };
  /** The room a request is about (path or join body), normalised; null when none or not a valid code. */
  function roomOf(rq, raw) {
    try {
      if (rq.params.code) return rooms.normalizeCode(rq.params.code);
      const b = raw && raw.length < 512 ? JSON.parse(raw) : null;
      if (b && typeof b.code === 'string') return rooms.normalizeCode(b.code);
    } catch { /* none */ }
    return null;
  }

  function info(c) {
    const s = loadSettings();
    if (smg) {
      const on = (k) => !s.kill.has(k) && !s.kill.has('all');
      return {
        proto: PROTO,
        minProto: cfg.minProto,
        game: 'smg',
        serverMs: c.now,
        kill: [...s.kill],
        message: s.message || null,
        features: { rooms: on('rooms'), quick: on('quick'), friends: on('friends'), times: on('times'), turn: !!turnProvider && on('turn'), ranked: false, ranking: false },
        limits: {
          nameMin: NAME_MIN, nameMax: NAME_MAX, maxMembers: cfg.smg.maxMembers, friendsMax: cfg.smg.friendsMax, settingsMaxBytes: rooms4.SETTINGS_MAX_BYTES,
          timesPerHour: cfg.smg.times.perHour, timeFloorFrames: cfg.smg.times.floorFrames, timeCapFrames: cfg.smg.times.capFrames,
        },
        stun: cfg.stunServers,
        punch: cfg.rooms.punch,
        roomCodeAlphabet: rooms.CODE_ALPHABET,
        app: { min: cfg.minApp || null, latest: cfg.latestApp || null },
        link: { pingAfterSec: cfg.rooms.quietMs / 1000, lostAfterSec: cfg.rooms.lostMs / 1000, joinAckSec: cfg.rooms.ackMs / 1000, natLifeSec: cfg.rooms.natLifeMs / 1000, onlineSec: cfg.smg.onlineMs / 1000 },
        servers: cfg.serverList,
      };
    }
    return {
      proto: PROTO,
      minProto: cfg.minProto,
      serverMs: c.now,
      kill: [...s.kill],
      message: s.message || null,
      features: { ranked: !s.kill.has('ranked') && !s.kill.has('all'), turn: !!turnProvider && !s.kill.has('turn') && !s.kill.has('all'), ranking: !s.kill.has('ranking') && !s.kill.has('all') },
      limits: { nameMin: NAME_MIN, nameMax: NAME_MAX, dataMax: ranking.DATA_MAX, blobSize: ranking.BLOB_SIZE },
      stun: cfg.stunServers,
      punch: cfg.rooms.punch,
      roomCodeAlphabet: rooms.CODE_ALPHABET,
      resultWindowSec: cfg.matches.resultWindowMs / 1000,
      app: { min: cfg.minApp || null, latest: cfg.latestApp || null },
      link: { pingAfterSec: cfg.rooms.quietMs / 1000, lostAfterSec: cfg.rooms.lostMs / 1000, queueDropSec: cfg.queue.staleMs / 1000, joinAckSec: cfg.rooms.ackMs / 1000, natLifeSec: cfg.rooms.natLifeMs / 1000 },
      servers: cfg.serverList,
    };
  }

  /** Where the service lives. Lets the Worker URL move without a client update: the client keeps the last list it saw. */
  function serverList(c) {
    return {
      version: 1,
      ttlSec: 86400,
      servers: cfg.serverList,
      stun: cfg.stunServers,
      app: { min: cfg.minApp || null, latest: cfg.latestApp || null },
      serverMs: c.now,
    };
  }

  function timeInfo(now) {
    const p = dateParts(now);
    const s = season(now);
    return {
      utcMs: now,
      iso: new Date(now).toISOString(),
      year: p.year, month: p.month, month0: p.month - 1, mday: p.mday,
      hour: p.hour, min: p.min, sec: p.sec, wday: p.wday, yday: p.yday,
      dayStartMs: dayStart(now),
      season: { index: s.index, startMs: s.startMs, endMs: s.endMs },
    };
  }

  async function authenticate(c, request) {
    const h = request.headers.get('authorization') || '';
    const m = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(h);
    if (!m) throw new ApiError(401, 'unauthorized', 'missing or malformed token', {}, { 'www-authenticate': 'Bearer' });
    const hash = await sha256hex(m[1]);
    const p = db.get(`SELECT * FROM profiles WHERE token_hash = ?`, hash);
    if (!p) throw new ApiError(401, 'unauthorized', 'unknown token', {}, { 'www-authenticate': 'Bearer' });
    if (p.banned) throw new ApiError(403, 'banned', 'this account is blocked');
    if (smg) online.set(p.pid, c.now);
    if (c.now - p.last_seen_ms > 300_000) db.run(`UPDATE profiles SET last_seen_ms = ? WHERE pid = ?`, c.now, p.pid);
    return p;
  }

  // The raw body is always consumed first (the platform dislikes unread request streams); it is parsed only
  // after authentication and rate limits, so junk from strangers costs as little as possible.
  async function readRaw(request, cap = MAX_BODY) {
    if (request.method === 'GET' || request.method === 'HEAD') return '';
    const len = Number(request.headers.get('content-length') || 0);
    if (len > cap) throw new ApiError(413, 'too_large', 'request body too large');
    const text = await readBodyCapped(request, cap);
    if (text === null) throw new ApiError(413, 'too_large', 'request body too large');
    return text;
  }

  function parseBody(text) {
    if (!text.trim()) return undefined;
    try {
      return JSON.parse(text);
    } catch {
      throw new ApiError(400, 'bad_json', 'body is not valid JSON');
    }
  }

  /**
   * POST /v1/register, in this order: parse and validate the body (no state touched), proof of work if enabled, then the
   * per-address bucket (a burst for a venue behind one NAT, then a slow rate) and the global daily bucket, then the insert.
   * A request that fails validation (or asks for a taken name) is charged only to a separate, cheap "bad request" bucket:
   * garbage can no longer use up the global daily cap (REGISTER_MAX_PER_DAY).
   */
  async function prepareRegister(c, rq, raw, ipk, now) {
    const rg = cfg.register;
    try {
      rq.body = parseBody(raw);
      accounts.checkRegisterBody(c, rq.body);
      await pow.verifyPow(c, ipk, rq.body.pow);
    } catch (e) {
      if (e instanceof ApiError && e.status >= 400 && e.status < 500) limit(c, `regbad:${ipk}`, rg.badPerHour, 3600, now);
      throw e;
    }
    limit(c, `register:ip:${ipk}`, rg.ipPerHour, 3600, now, rg.ipBurst);
    if (rg.netPerDay) limit(c, `register:net:${netKey(ipk)}`, rg.netPerDay, 86400, now);
    if (!rg.storedDaily) limit(c, 'register:all', rg.maxPerDay, 86400, now); // (Galaxy: the stored daily count, accounts.register)
  }

  // ------------------------------------------------------------ the service's own daily request budget (budget.js)
  // Counted per UTC day: requests ADMITTED (the budget), requests REFUSED for abuse (per-address and per-account daily ceilings) and
  // requests refused because the budget is spent are three separate counts. New sessions stop at (100 - LIVE_RESERVE_PCT) % of the
  // budget, members of a live room go on to the whole of it. See budget.js and README.md "Quota defence".
  rowsSink = (n, est) => bud.addRows(clock(), n, est);
  const SIGNUP_PATTERNS = new Set(['/v1/register/challenge', '/v1/register']);
  const bud = createBudget(db, cfg, cfg.rateScale, { dayIndex, secondsToMidnight });
  /**
   * Is this request a live room member's session traffic? A member of THE room the route names counts (a route without a code: a member of any
   * live room). Anybody else asking about a room is an ordinary player, in the Worker's class too (worker.js tries both). One indexed read; only
   * asked for the routes above.
   */
  function isLiveMember(route, pid, params, raw) {
    if (!smg) return false;
    if (route.pattern === '/v1/rooms/join') { // a dropped player coming back to a started game is part of that session, not a new one
      let code = null;
      try { const b = raw && raw.length < 512 ? JSON.parse(raw) : null; if (b && typeof b.code === 'string') code = rooms.normalizeCode(b.code); } catch { /* not a code join */ }
      return !!code && !!db.get(
        `SELECT 1 AS x FROM rooms r WHERE r.code = ? AND r.state = 'started' AND r.game = 'smg'
           AND (EXISTS (SELECT 1 FROM room_members m WHERE m.code = r.code AND m.pid = ?) OR EXISTS (SELECT 1 FROM room_left l WHERE l.code = r.code AND l.pid = ?))`,
        code, pid, pid,
      );
    }
    if (route.pattern === '/v1/invites/resolve') { // a friend using a code issued before the budget ran short: part of a session already being formed
      let code = null;
      try { const b = raw && raw.length < 512 ? JSON.parse(raw) : null; if (b && typeof b.code === 'string') code = invites.normalizeInvite(b.code); } catch { /* not a code */ }
      return !!code && !!db.get(`SELECT 1 AS x FROM invites WHERE code = ? AND state = 'open'`, code);
    }
    // a time posted from inside a game belongs to the game in progress; any live room of the caller will do (the time's own checks follow)
    if (!LIVE_PATTERNS.has(route.pattern) && route.pattern !== '/v1/times') return false;
    const code = params && params.code ? String(params.code).toUpperCase() : null;
    return !!db.get(
      `SELECT 1 AS x FROM room_members m JOIN rooms r ON r.code = m.code WHERE m.pid = ? AND r.state IN ('open','joined','ready','started') AND (? IS NULL OR m.code = ?) LIMIT 1`,
      pid, code, code,
    );
  }
  /** A request that frees resources (leave, close): never refused by the budget. */
  const freeing = (route) => smg && ((route.method === 'POST' && (route.pattern === '/v1/rooms/:code/leave' || route.pattern === '/v1/invites/:code/close')) || (route.method === 'DELETE' && (route.pattern === '/v1/rooms/:code' || route.pattern === '/v1/invites/:code')));
  /** Room answers tell the clients how much to stretch their polls (1 = not at all); absent at 1 so the usual answer is unchanged. */
  const withPace = (view) => {
    const p = bud.pace();
    return p > 1 && view && typeof view === 'object' ? { ...view, pollScale: p } : view;
  };

  // sec34: the daily per-address counters are stored (budget.js addr_day), under a keyed hash of the address's /48 (IPv4: the address) and the day, never the address itself.
  let addrHmacKey = null;
  async function addrCounterKey(ipk, now) {
    if (!addrHmacKey) {
      let r = db.get(`SELECT value FROM settings WHERE key = 'addr_salt'`);
      if (!r) { r = { value: hex(randomBytes(32)) }; db.run(`INSERT INTO settings (key, value) VALUES ('addr_salt', ?)`, r.value); }
      addrHmacKey = await crypto.subtle.importKey('raw', Uint8Array.from(r.value.match(/../g).map((h) => parseInt(h, 16))), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    }
    const mac = new Uint8Array(await crypto.subtle.sign('HMAC', addrHmacKey, te.encode(`${dayIndex(now)}|${dailyAddrKey(ipk)}`)));
    return hex(mac).slice(0, 24);
  }

  async function handle(request) {
    let now = clock();
    bud.touch(now); // every request that reaches the object is one Durable Object request on the platform's bill: counted before anything is decided
    try {
      const url = new URL(request.url);
      if (cfg.allowTestClock && request.headers.get('x-test-time')) now = Number(request.headers.get('x-test-time'));
      const c = ctxFor(now);
      const ip = (cfg.allowTestClock && request.headers.get('x-test-ip')) || request.headers.get('cf-connecting-ip') || 'unknown';
      const ipk = ipKey(ip); // IPv6 by /64
      const dk = ip === INTERNAL_ADDRESS ? ip : await addrCounterKey(ipk, now); // the daily ceilings: by the /48, stored as a hash

      let route = null;
      let params = {};
      let pathMatched = false;
      for (const r of routes) {
        const m = r.re.exec(url.pathname);
        if (!m) continue;
        pathMatched = true;
        if (r.method === request.method) { route = r; params = m.groups || {}; break; }
      }
      if (!route) {
        const refused = bud.gateAddress(CLASS_OPEN, dk, now); // junk paths count against the address
        if (refused) throw refused;
        if (pathMatched) throw new ApiError(405, 'method_not_allowed', 'method not allowed');
        throw new ApiError(404, 'not_found', 'no such endpoint');
      }
      const o = route.opts;
      if (!o.admin && !o.auth) { // no token: the address ceiling, then the budget (new sessions only)
        const refused = bud.gateAddress(CLASS_OPEN, dk, now) || bud.admit(now, false, { signup: SIGNUP_PATTERNS.has(route.pattern) });
        if (refused) throw refused;
      }
      const raw = await readRaw(request, bodyCapFor(game, route.method, route.pattern));

      if (o.admin) {
        if (!cfg.adminEnabled) throw new ApiError(404, 'not_found', 'no such endpoint');
        // secreview32 (F1): a refusal here names the ADDRESS (`x-msc-block: adm:<seconds>`): the Worker then answers that address's admin
        // requests itself for that long. Before, the Worker never consulted its verdict memory for admin routes, so a loop of wrong bearers
        // reached the object 5 times a second from one address, all day (each one a Durable Object request and one of DAILY_OBJECT_BUDGET).
        const admBlock = (e) => { if (e instanceof ApiError && e.status === 429 && e.extra && e.extra.retryAfterSec > 0) e.headers['x-msc-block'] = `adm:${Math.min(3600, e.extra.retryAfterSec)}`; return e; };
        try { limit(c, `adminip:${ipk}`, 30, 60, now); } catch (e) { throw admBlock(e); }
        const h = request.headers.get('authorization') || '';
        const given = h.startsWith('Bearer ') ? h.slice(7) : '';
        if (!(await safeEqual(given, cfg.adminToken))) {
          try { brakeTake(`adminfail:${ipk}`, 5, 600, now); } catch (e) { throw admBlock(e); }
          throw new ApiError(401, 'unauthorized', 'bad admin token');
        }
      } else {
        // Version gating fails closed: a malformed header is an error, never "no header" (edge.js has the format rules).
        // secreview32 (F1): a player route refused BEFORE its token is looked at (wrong or old version headers, a switched-off feature) had no
        // per-address ceiling at all (the open routes are charged above, the others only after authentication). Such a refusal now counts
        // against the address like a failed sign-in (ADDR_BAD_DAY, budget.js gateBadAuth): past it the address gets the `bad` verdict, which
        // the Worker enforces without waking the object (tokens it has seen succeed still pass: a NAT's honest consoles keep working).
        const preAuth = (err) => {
          if (o.auth) { const refused = bud.gateBadAuth(dk, now); if (refused) return refused; }
          return err;
        };
        const verr = versionHeaderError(request.headers, { requireApp: cfg.requireApp, open: !!o.open });
        if (verr) throw preAuth(verr);
        const gerr = gameHeaderError(request.headers, game, { open: !!o.open });
        if (gerr) throw preAuth(gerr);
        const protoHeader = request.headers.get('x-msc-proto');
        if ((protoHeader === null ? PROTO : Number(protoHeader)) < cfg.minProto) {
          throw preAuth(new ApiError(426, 'upgrade_required', 'this client is too old for the server', { minProto: cfg.minProto }));
        }
        const app = request.headers.get('x-msc-app');
        if (app !== null && cfg.minApp && cmpVersion(app, cfg.minApp) < 0) {
          throw preAuth(new ApiError(426, 'update_required', 'please update the game', { minVersion: cfg.minApp }));
        }
        if (!o.open || o.web) {
          const kill = loadSettings().kill;
          if (kill.has('all') || (o.kill && kill.has(o.kill))) {
            throw preAuth(new ApiError(503, 'service_disabled', loadSettings().message || 'this service is switched off for now', { retryAfterSec: 300 }));
          }
        }
        if (o.rl && o.rl[3] === 'ip') limit(c, `${o.rl[0]}:ip:${ipk}`, o.rl[1], o.rl[2], now);
        if (o.globalRl) limit(c, `${o.globalRl[0]}:all`, o.globalRl[1], o.globalRl[2], now);
      }

      const query = url.searchParams;
      const rq = {
        body: undefined, query, params, player: null, ip: ipk, ipk, host: url.hostname.toLowerCase(),
        app: request.headers.get('x-msc-app') || '', build: request.headers.get('x-msc-build') || '',
      };
      if (o.auth) {
        try {
          rq.player = await authenticate(c, request);
          rq.token = (/^Bearer ([A-Za-z0-9_-]{43})$/.exec(request.headers.get('authorization') || '') || [])[1] || ''; // (only to mask it in a report's text)
        } catch (e) {
          if (e instanceof ApiError && e.status === 401) {
            // a wrong token costs the address like any other request of its class (the Worker's class: by route), then the guessing brake
            // (and the failed-authentication ceiling of the address: Strikers d330d612)
            const refused = bud.gateBadAuth(dk, now) || bud.gateAddress(LIVE_PATTERNS.has(route.pattern) ? CLASS_LIVE : CLASS_PLAYER, dk, now);
            if (refused) throw refused;
            brakeTake(`authfail:${ipk}`, 20, 600, now);
          }
          throw e;
        }
        // the account is known: live-room member or not decides which ceiling and which part of the budget it is served from
        const live = isLiveMember(route, rq.player.pid, params, raw);
        const refused = bud.gateAddress(live ? CLASS_LIVE : CLASS_PLAYER, dk, now) || bud.gatePid(rq.player.pid, now) || bud.admit(now, live, { leave: freeing(route), room: params.code || `p${rq.player.pid}`, ipk: dk });
        if (refused) {
          // join / times may be live traffic (the Worker lets them through a "new sessions refused" state to ask us); a refusal of a caller who is
          // not part of a live session stands for 30 s, so a non-member's retries do not each cost an object request
          if (!live && refused.status === 503 && MAYBE_LIVE.has(route.pattern)) { refused.headers['x-msc-block'] = 'rl:30'; refused.headers['retry-after'] = '30'; refused.extra.retryAfterSec = 30; }
          throw refused;
        }
        if (o.rl && o.rl[3] === 'pid') limit(c, `${o.rl[0]}:pid:${rq.player.pid}`, o.rl[1], o.rl[2], now);
        if (o.rlIp) limit(c, `${o.rlIp[0]}:ip:${ipk}`, o.rlIp[1], o.rlIp[2], now);
      }

      if (o.register) await prepareRegister(c, rq, raw, ipk, now);
      else rq.body = parseBody(raw);
      // held polls: a call that changes a room wakes the polls waiting on it (a version check, so pollers do not wake each other)
      const watched = smg && waiters.size && route.method !== 'GET' ? roomOf(rq, raw) : null;
      const before = watched ? roomVersion(watched) : null;
      let result;
      try {
        result = await route.handler(c, rq);
      } finally {
        if (smg && waiters.size) {
          if (watched && roomVersion(watched) !== before) wakeRoom(watched);
          if (route.pattern === '/v1/quick' || route.pattern === '/v1/rooms/join') wakeRoom(result && result.code);
          if (o.admin || route.pattern === '/v1/me') for (const k of [...waiters.keys()]) wakeRoom(k); // bans, deletions: rare
        }
      }
      const resp = result instanceof Response ? result : json(200, result);
      resp.headers.set('x-msc-proto', String(PROTO));
      return resp;
    } catch (e) {
      if (e instanceof ApiError) {
        if (cfg.latestApp) e.extra.latestVersion = cfg.latestApp;
        return errorResponse(e);
      }
      // Unexpected: never echo internals or request data.
      const internal = new ApiError(500, 'internal', 'internal error');
      if (cfg.latestApp) internal.extra.latestVersion = cfg.latestApp;
      return errorResponse(internal);
    }
  }

  /** Housekeeping, run from the Durable Object alarm and opportunistically. Returns whether more work is pending: true, false or (Galaxy) 'settled' (only finished rooms remain). */
  function sweep() {
    // expiry and lost members change rooms here: held polls on a room whose version moved look again (only those)
    const held = [...waiters.keys()].map((k) => [k, roomVersion(k)]);
    try { return sweepNow(); } finally { for (const [k, v] of held) if (roomVersion(k) !== v) wakeRoom(k); }
  }
  function sweepNow() {
    const now = clock();
    const c = ctxFor(now);
    c.tx(() => {
      const live = db.all(
        `SELECT code FROM rooms WHERE (state IN ('open','joined','ready','started') AND expires_ms < ?1)
            OR (kind = 'ranked' AND state = 'joined' AND (host_ack = 0 OR join_ack = 0) AND created_ms + ?2 < ?1)`,
        now, cfg.rooms.ackMs,
      );
      for (const r of live) rooms.loadRoom(c, r.code);
      db.run(`DELETE FROM rooms WHERE state IN ('closed','expired') AND expires_ms + ? < ?`, cfg.rooms.keepClosedMs, now);
      if (smg) {
        // members (and their addresses) go with their room; old submissions are only needed for the hourly cap and verification
        for (const r of db.all(`SELECT * FROM rooms WHERE game = 'smg' AND state IN ('open','joined','ready','started')`)) rooms4.checkHostLost(c, r);
        db.run(`DELETE FROM room_members WHERE code NOT IN (SELECT code FROM rooms)`);
        invites.sweepInvites(c);
        db.run(`DELETE FROM room_left WHERE left_ms < ?`, now - 60_000);
        db.run(`DELETE FROM time_submits WHERE submitted_ms < ?`, now - cfg.smg.times.keepSubmitsMs);
        for (const [k, t] of online) if (now - t > 3600_000) online.delete(k);
      } else {
        ranked.sweepQueue(c);
        matches.sweepMatches(c);
      }
      db.run(`DELETE FROM counters WHERE updated_ms < ? AND key NOT LIKE 'purge:%'`, now - 40 * 86400_000);
      // the per-address and per-room relay counters hold a raw source address / a room code: kept 2 days, not 40 (svcfix3)
      db.run(`DELETE FROM counters WHERE (key LIKE 'turn:creds:%:a%' OR key LIKE 'turn:check:%:a%' OR key LIKE 'turn:room:%' OR key LIKE 'summary:%') AND updated_ms < ?`, now - 2 * 86400_000);
      db.run(`DELETE FROM pair_stats WHERE updated_ms < ?`, now - cfg.rooms.pairFailKeepMs);
      db.run(`DELETE FROM turn_mints WHERE minted_ms < ?`, now - 2 * 86400_000);
      db.run(`DELETE FROM daily WHERE day < ?`, dayIndex(now) - 7);
      db.run(`DELETE FROM pair_daily WHERE day < ?`, dayIndex(now) - 1);
      if (!smg) archiveSeasons(c);
      // the idle-account scan (one pass over profiles.last_seen_ms, indexed) runs at most hourly; a restarted object runs it at its first sweep
      if (now - lastPruneMs >= PRUNE_EVERY_MS || now < lastPruneMs) {
        lastPruneMs = now;
        accounts.pruneIdle(c);
      }
      for (const [k, t] of presence) if (now - t > 3600_000) presence.delete(k);
      for (const [k, d] of direct) if (now - d.at > 3600_000) direct.delete(k);
    });
    // The alarm (60 s) keeps the object awake and costs a Durable Object request each time: it runs only while something is live.
    // Galaxy: live rooms only (finished rooms are deleted by the next sweep that does run; nothing else needs a timer).
    // Galaxy: true = a live room (the alarm comes back in 60 s); 'settled' = only finished rooms still waiting to be deleted (120 s is enough);
    // false = nothing pending (no alarm at all).
    if (smg) {
      if (db.get(`SELECT COUNT(*) AS n FROM rooms WHERE state IN ('open','joined','ready','started')`).n > 0) return true;
      return db.get(`SELECT COUNT(*) AS n FROM rooms`).n > 0 ? 'settled' : false;
    }
    const pending =
      db.get(`SELECT COUNT(*) AS n FROM rooms`).n + db.get(`SELECT COUNT(*) AS n FROM queue`).n +
      db.get(`SELECT COUNT(*) AS n FROM matches WHERE resolved_ms IS NULL`).n;
    return pending > 0;
  }

  const routeMeta = routes.map((r) => [r.method, r.pattern, r.opts.admin ? 'admin' : r.opts.auth ? 'player' : 'none', !!r.opts.open]);
  // routeOpts: [method, pattern, options] of every route, so the audit tests can check that each one is rate limited and switchable
  const routeOpts = routes.map((r) => [r.method, r.pattern, r.opts]);
  return { handle, sweep, cfg, killNames: KILL_NAMES, routeMeta, routeOpts, limiter, brake, waiting: () => nWaiters, budgetUsed: () => bud.used(), budget: bud, touchAlarm: () => bud.touch(clock()),
    /** The alarm's TURN work: a usage read when due, and the revoke sweep while a hard limit is tripped. Never throws. */
    turnTick: async () => {
      try { if (turnGuard) await turnGuard.tick(ctxFor(clock())); } catch { /* best effort */ }
      // the beta's D1 work (Galaxy): queued account purges, and once an hour the reports' expiry / recount and the old daily aggregates
      try {
        const c = ctxFor(clock());
        if (smg && c.reportsDb) {
          await reports.drainPurges(c);
          if (c.now - lastBetaSweep >= 3600_000 || c.now < lastBetaSweep) {
            lastBetaSweep = c.now;
            const ms = c.cfg.reports.d1TimeoutMs; // a stalled D1 must not hold the alarm (the hourly sweep is tried again next hour)
            try { await reports.withTimeout(reports.sweepReports(c), ms); } catch { /* best effort */ }
            try { await reports.withTimeout(summaries.sweepSummaries(c), ms); } catch { /* best effort */ }
          }
        }
      } catch { /* best effort */ }
    } };
}
