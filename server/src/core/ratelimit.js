// In-memory token buckets. The Durable Object is a singleton, so one map is enough; if the object is
// evicted the buckets reset, which only ever errs on the side of allowing a little more (the daily caps that must
// survive eviction live in storage, see limits.js).
// Client addresses are used as bucket keys in memory only; they are never written to storage.
//
// Two instances are used by the app: the ordinary limiter (bounded, evicts the oldest quarter when full) and a
// separate "brake" limiter for wrong-token counters. The brake never evicts a bucket that still remembers something:
// it only drops buckets that have refilled completely (identical to a fresh one) and, if it is still full of live
// ones, sends new keys to one shared overflow bucket. Flooding the ordinary limiter with junk keys can therefore
// never reset the admin brake, and neither can flooding the brake.

const OVERFLOW = '\u0000overflow';

export class RateLimiter {
  /**
   * @param {number} maxKeys
   * @param {{protect?: boolean}} [opts] protect: never evict live buckets (see above)
   */
  constructor(maxKeys = 20000, opts = {}) {
    /** @type {Map<string,{t:number,at:number,full:number,rate:number}>} */
    this.buckets = new Map();
    this.maxKeys = maxKeys;
    this.protect = !!opts.protect;
  }

  /**
   * @param {string} key bucket key
   * @param {number} cap tokens refilled per windowSec (the sustained rate is cap / windowSec)
   * @param {number} windowSec seconds to refill `cap` tokens
   * @param {number} now ms
   * @param {number} [burst] bucket size (default cap): a bigger burst than the sustained rate allows a short spike, then the slow rate
   * @returns {number} 0 when allowed, otherwise seconds to wait
   */
  take(key, cap, windowSec, now, burst = cap) {
    const b = this.#bucket(key, cap, windowSec, now, burst);
    if (b.t >= 1) {
      b.t -= 1;
      return 0;
    }
    return Math.max(1, Math.ceil((1 - b.t) / b.rate / 1000));
  }

  #bucket(key, cap, windowSec, now, burst) {
    const rate = cap / (windowSec * 1000);
    let b = this.buckets.get(key);
    if (!b) {
      if (this.buckets.size >= this.maxKeys) {
        this.sweep(now);
        if (this.protect && this.buckets.size >= this.maxKeys) {
          key = OVERFLOW;
          b = this.buckets.get(key);
        }
      }
      if (!b) {
        b = { t: burst, at: now, full: burst, rate };
        this.buckets.set(key, b);
        return b;
      }
    }
    b.full = burst;
    b.rate = rate;
    b.t = Math.min(burst, b.t + Math.max(0, now - b.at) * rate);
    b.at = now;
    return b;
  }

  sweep(now) {
    if (this.protect) {
      // lossless: a bucket that is back at its full size equals a new one
      for (const [k, b] of this.buckets) {
        if (k !== OVERFLOW && b.t + Math.max(0, now - b.at) * b.rate >= b.full) this.buckets.delete(k);
      }
      return;
    }
    for (const [k, b] of this.buckets) {
      if (now - b.at > 3600_000) this.buckets.delete(k);
    }
    if (this.buckets.size >= this.maxKeys) {
      // still full: drop the oldest quarter
      let n = Math.floor(this.maxKeys / 4);
      for (const k of this.buckets.keys()) {
        this.buckets.delete(k);
        if (--n <= 0) break;
      }
    }
  }
}
