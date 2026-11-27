// A cheap per-address token bucket the Worker applies BEFORE it wakes the Durable Object (every request that reaches the object costs a
// Durable Object request on the free plan; the object's own limits come too late for that budget).
//
// BEST EFFORT: the table lives in this isolate's memory. Cloudflare may run many isolates and recycles them, so a flood spread over
// many isolates (or after a restart) gets through in part, and the object's per-account and per-address limits still apply behind this
// one. It stops the cheap case: one address hammering one isolate. For a hard limit use the Workers Rate Limiting binding (README.md,
// "Optional hardening").

import { ipKey } from './util.js';

const MAX_KEYS = 4000;

/**
 * @param {{burst: number, perSec: number}} opts bucket size and refill; burst 0 = off
 */
export function createEdgeBucket({ burst, perSec } = { burst: 200, perSec: 3 }) {
  /** @type {Map<string, {tokens: number, at: number}>} */
  const table = new Map();
  return {
    /** Seconds to wait (>= 1) when the address is out of tokens, else 0. */
    take(rawIp, now, scale = 1, extra = '') {
      if (!(burst > 0)) return 0;
      const cap = Math.max(1, burst * scale);
      const rate = Math.max(0.001, perSec * scale);
      const key = extra ? `${ipKey(rawIp)}|${extra}` : ipKey(rawIp);
      let b = table.get(key);
      if (!b) {
        if (table.size >= MAX_KEYS) table.delete(table.keys().next().value); // oldest first (Map keeps insertion order)
        b = { tokens: cap, at: now };
      } else {
        table.delete(key); // re-insert: most recently used last
        b.tokens = Math.min(cap, b.tokens + ((now - b.at) / 1000) * rate);
        b.at = now;
      }
      table.set(key, b);
      if (b.tokens >= 1) {
        b.tokens -= 1;
        return 0;
      }
      return Math.max(1, Math.ceil((1 - b.tokens) / rate));
    },
    size: () => table.size,
    clear: () => table.clear(),
  };
}

/**
 * The Worker's limiter (core/edge_bucket.js buckets, one isolate's memory). A request carrying a player token is charged to TWO buckets: its own
 * (address + token, `burst`/`perSec`) and the address's aggregate (`aggBurst`/`aggPerSec`, several times larger), so a few consoles behind one NAT or
 * CGNAT, each polling a room or the queue every second or two, are not throttled together (the old single per-address bucket was shared by all of
 * them). A flood that invents tokens to get fresh per-token buckets is still stopped by the aggregate; a request with no token (register, info,
 * time, leaderboard) keeps the plain per-address bucket. Burst 0 = off for everything; aggregate burst 0 = no aggregate cap for token requests.
 * (Strikers 2e8378c6, with Galaxy's numbers: see worker.js.)
 * @param {{burst: number, perSec: number, aggBurst?: number, aggPerSec?: number}} opts
 */
export function createEdgeLimiter({ burst, perSec, aggBurst = 300, aggPerSec = 5 }) {
  const plain = createEdgeBucket({ burst, perSec });
  const perToken = createEdgeBucket({ burst, perSec });
  const aggregate = createEdgeBucket({ burst: aggBurst, perSec: aggPerSec });
  return {
    /** Seconds to wait (>= 1) when refused, else 0. `authorization` is the raw header (may be empty). */
    take(rawIp, authorization, now, scale = 1) {
      if (!(burst > 0)) return 0;
      const m = /^Bearer\s+(\S{8,})$/.exec(authorization || '');
      if (!m) return plain.take(rawIp, now, scale);
      const wait = perToken.take(rawIp, now, scale, m[1].slice(0, 48));
      if (wait > 0) return wait;
      return aggregate.take(rawIp, now, scale);
    },
    size: () => plain.size() + perToken.size() + aggregate.size(),
    clear() { plain.clear(); perToken.clear(); aggregate.clear(); },
  };
}
