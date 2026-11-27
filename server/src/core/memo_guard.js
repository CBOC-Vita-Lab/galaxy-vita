// What a Worker-side memo hit must check before it is served (worker.js): the route's kill switch and the per-address rate limit,
// both of which the Durable Object applies to a miss. Pure logic (no Workers APIs) so it is unit tested in node.
//
// The object's switches are known here only as of the last look (killState.at): a hit is allowed while that look is younger than
// RECHECK_MS; otherwise the caller re-reads them (worker.js: GET /v1/info on the object) before serving.

import { ipKey } from './util.js';
import { RateLimiter } from './ratelimit.js';

// svc3 (review S5): was 5 s, i.e. 12 object requests a minute per isolate under steady traffic. The memo itself lives 60 s and a
// miss re-reads the switches (routeOn), so a hit can only be older than the last look by the memo's own age: 60 s costs nothing more.
export const RECHECK_MS = 60_000;
export const HIT_CAP = 30;        // same numbers as the object's rl for /v1/public/times and /v1/public/leaderboard: 30 per 60 s per address
export const HIT_WINDOW_SEC = 60;

export function createMemoGuard() {
  const state = { at: -Infinity, names: new Set() }; // (-Infinity: never looked, whatever the clock says)
  const limiter = new RateLimiter(5000);
  const envNames = (env) => String(env.KILL_SWITCH || '').split(',').map((x) => x.trim()).filter(Boolean);
  return {
    /** true when the isolate's view of the object's switches is too old (or the clock went backwards) */
    stale: (now) => now - state.at >= RECHECK_MS || now < state.at,
    /** record the object's switches as read at `now` (names = info.kill) */
    seen: (names, now) => { state.names = new Set(names.map(String)); state.at = now; },
    /** the object answered a request for this route with 200 (so `killName` and `all` are off) at `now` */
    routeOn: (now) => { state.names = new Set(); state.at = now; },
    /** forget what was seen: look again before the next hit is served */
    forget: () => { state.at = -Infinity; },
    /** retry a failed look shortly instead of at every hit */
    retrySoon: (now) => { state.at = now - RECHECK_MS + 1000; },
    /** is the route switched off (by the KILL_SWITCH variable or by an admin switch seen at the last look)? */
    off: (env, killName) => {
      const n = new Set([...envNames(env), ...state.names]);
      return n.has('all') || n.has(killName);
    },
    /** rate limit for one hit; returns 0 when allowed, else seconds to wait */
    take: (env, killName, ip, now) => {
      const scale = Number(env.RATE_LIMIT_SCALE) > 0 ? Number(env.RATE_LIMIT_SCALE) : 1;
      return limiter.take(`${killName}:${ipKey(ip)}`, Math.max(1, Math.ceil(HIT_CAP * scale)), HIT_WINDOW_SEC, now);
    },
  };
}
