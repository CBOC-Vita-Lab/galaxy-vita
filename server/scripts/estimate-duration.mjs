#!/usr/bin/env node
// Durable Object DURATION against the free plan's 13,000 GB-s a day (svc3, review S4: "held polls can exhaust the duration quota").
// Duration is billed per OBJECT, in wall-clock time while it is active in memory, at 128 MB whatever it really uses (0.125 GB). It is not
// multiplied by the number of requests in flight: 30 held polls at once cost the same as one. So the question is only: for how many seconds
// of the day is anything keeping the object awake? (a request or a pending timer; after the last one it lingers for a grace period).
//   node scripts/estimate-duration.mjs
// The numbers of the code: a started room's console holds a poll HELD_MAX_SEC (8 s) and asks again after 4 s (np_signal_http.cpp), so a
// player in a room has a request in flight about 2/3 of the time and the gap between two requests is 4 s, well under any grace period.
const GB = 0.125;                 // 128 MB
const QUOTA = 13000;              // GB-s per day, the whole account (Galaxy's object and Strikers' object together)
const DAY = 86400;
const GRACE = [10, 70, 140];      // seconds an idle object stays in memory: UNSURE (docs say about 10 s for hibernation, 70-140 s are quoted for
                                  // objects that cannot hibernate); the result is shown for all three
const REQ_PER_PLAYER_HOUR = 354;  // measured
const BUDGET = 60000;             // DAILY_REQUEST_BUDGET
const playerHours = BUDGET / REQ_PER_PLAYER_HOUR;

function activeSeconds(hoursWithPlayers, sessionsPerDay, grace) {
  // each "burst" of play keeps the object up for its length + grace at its end; bursts inside the same hour merge
  return Math.min(DAY, hoursWithPlayers * 3600 + sessionsPerDay * grace);
}
const rows = [];
const scenario = (name, hours, sessions) => {
  for (const g of GRACE) {
    const s = activeSeconds(hours, sessions, g);
    rows.push({ scenario: name, graceSec: g, activeHours: +(s / 3600).toFixed(1), gbSeconds: Math.round(s * GB), pctOfQuota: +((s * GB / QUOTA) * 100).toFixed(0) });
  }
};
console.log(`budget ${BUDGET} requests = ${playerHours.toFixed(0)} player-hours at ${REQ_PER_PLAYER_HOUR} requests per player-hour`);
console.log(`ceiling: one object active ALL day = ${Math.round(DAY * GB)} GB-s = ${((DAY * GB / QUOTA) * 100).toFixed(0)} % of ${QUOTA}`);
scenario('evening peak: all play inside 6 hours', 6, 60);
scenario('spread over 14 hours (three time zones)', 14, 120);
scenario('worst case: someone is playing every minute', 24, 400);
console.table(rows);
console.log('Galaxy alone can never pass the quota (the ceiling above is below it). Together with Strikers it can: if the Strikers object is also awake\nfor more than', ((QUOTA - DAY * GB) / GB / 3600).toFixed(1), 'h on a day when Galaxy never sleeps, the account runs out of duration. Watch "duration" in the dashboard.');
