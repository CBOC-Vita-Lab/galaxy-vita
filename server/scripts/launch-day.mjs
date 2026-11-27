// Launch-day arithmetic of the request budget (README "Request budget", svcfix2). Usage: node scripts/launch-day.mjs [demandPlayerHours=3000]
// An ASSUMED demand curve (share of a day's player-hours begun in each UTC hour, Americas-heavy: the evening in the Americas is 23:00-04:00 UTC);
// the per-player-hour request costs are the measured ones: 354 (before, held 8 s polls) and ~60 (after: the service's poll plan, 4-player room
// about 45, a pair about 60; test/core/svcfix2-cadence.test.js). New sessions are refused once the day's admitted count reaches 80 % of the budget.
const BUDGET = 60000, NEW_LIMIT = 0.8 * BUDGET;
const D = Number(process.argv[2]) || 3000;
const w = [8, 8, 7, 6, 4, 3, 2, 1, 1, 1, 1, 2, 2, 3, 3, 4, 5, 6, 7, 8, 9, 9, 8, 8]; // hour 0..23 UTC, relative
const sum = w.reduce((a, b) => a + b, 0);
function run(cost) {
  let used = 0, closed = null, served = 0;
  const rows = [];
  for (let h = 0; h < 24; h++) {
    const want = (D * w[h]) / sum;
    let ph = want;
    if (closed !== null) ph = 0;
    else if (used + want * cost > NEW_LIMIT) { ph = Math.max(0, (NEW_LIMIT - used) / cost); closed = h; }
    used += ph * cost; served += ph;
    rows.push({ h, want, ph, used });
  }
  return { rows, closed, served, used };
}
const before = run(354), after = run(60);
const f = (x) => Math.round(x).toLocaleString('en-US');
console.log(`demand ${D} player-hours/day; budget ${BUDGET}; new sessions stop at ${NEW_LIMIT}`);
console.log('UTC hour | wanted | before (354/h): served, used | after (60/h): served, used');
for (let h = 0; h < 24; h++) {
  const b = before.rows[h], a = after.rows[h];
  console.log(`${String(h).padStart(2, '0')}:00    | ${f(b.want).padStart(5)} | ${f(b.ph).padStart(5)} ${f(b.used).padStart(7)}${before.closed === h ? '  <- new sessions refused from here' : ''} | ${f(a.ph).padStart(5)} ${f(a.used).padStart(7)}${after.closed === h ? '  <- new sessions refused from here' : ''}`);
}
console.log(`served: before ${f(before.served)} player-hours (budget gone at ${before.closed === null ? 'no hour' : String(before.closed).padStart(2, '0') + ':00 UTC'}); after ${f(after.served)} (${after.closed === null ? 'never closed' : 'closed at ' + String(after.closed).padStart(2, '0') + ':00 UTC'})`);
console.log(`player-hours a day the budget buys: before ${f(NEW_LIMIT / 354)} (new sessions) / ${f(BUDGET / 354)} (whole budget); after ${f(NEW_LIMIT / 60)} / ${f(BUDGET / 60)}`);
