// How full the account table is, without scanning it. A COUNT(*) over `profiles` reads every row (on the free plan each row scanned is a
// "row read", 5,000,000 a day for the whole account): done per sign-up it costs 50,000 rows at 50,000 accounts, and a sign-up farm would
// turn that into the quota. The count is kept in memory (this object is a singleton): read once, moved by the sign-ups it sees, read again
// when it may be near the cap or after ten minutes, and dropped (re-read) whenever accounts are removed.

const REFRESH_MS = 600_000;

export function createFill(db, cfg) {
  const memo = { n: -1, at: 0 };
  const read = (now) => {
    memo.n = db.get(`SELECT COUNT(*) AS n FROM profiles WHERE pid <> 100000`).n;
    memo.at = now;
    return memo.n;
  };
  return {
    /** Accounts in total. exact: the cap decision itself (re-reads when the memo says the table is within 64 of the cap). */
    count(now, exact = false) {
      if (memo.n < 0 || now - memo.at >= REFRESH_MS || now < memo.at) return read(now);
      if (exact && memo.n >= cfg.maxPlayers - 64) return read(now);
      return memo.n;
    },
    /** A sign-up happened. */
    added(n = 1) { if (memo.n >= 0) memo.n += n; },
    /** Accounts were removed (the count is read again at the next question). */
    invalidate() { memo.n = -1; },
    /** 0 below half full, 1 from half, 2 from 80 %. Galaxy only (the Strikers service has no such tightening). */
    level(now) {
      if (!cfg.register.storedDaily || !cfg.maxPlayers) return 0;
      const f = this.count(now) / cfg.maxPlayers;
      return f >= 0.8 ? 2 : f >= 0.5 ? 1 : 0;
    },
  };
}
