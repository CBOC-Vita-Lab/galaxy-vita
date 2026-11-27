// Season / day windows. The game's calendar (NetworkSeasonCalendar.cpp) has 52 "seasons" that
// start on fixed month/day pairs (weekly, the last one running to Dec 31). The server owns the
// windows: a ranking row only counts while its last update lies inside the category's window.
// All in UTC.

export const SEASON_STARTS = [
  [1, 1], [1, 8], [1, 15], [1, 22], [1, 29], [2, 5], [2, 12], [2, 19], [2, 26], [3, 5], [3, 12], [3, 19], [3, 26],
  [4, 2], [4, 9], [4, 16], [4, 23], [4, 30], [5, 7], [5, 14], [5, 21], [5, 28], [6, 4], [6, 11], [6, 18], [6, 25],
  [7, 2], [7, 9], [7, 16], [7, 23], [7, 30], [8, 6], [8, 13], [8, 20], [8, 27], [9, 3], [9, 10], [9, 17], [9, 24],
  [10, 1], [10, 8], [10, 15], [10, 22], [10, 29], [11, 5], [11, 12], [11, 19], [11, 26], [12, 3], [12, 10], [12, 17], [12, 24],
];

const DAY = 86400000;

export function dateParts(ms) {
  const d = new Date(ms);
  const y = d.getUTCFullYear();
  const start = Date.UTC(y, 0, 1);
  return {
    year: y,
    month: d.getUTCMonth() + 1, // 1..12
    mday: d.getUTCDate(),
    hour: d.getUTCHours(),
    min: d.getUTCMinutes(),
    sec: d.getUTCSeconds(),
    wday: d.getUTCDay(),
    yday: Math.floor((Date.UTC(y, d.getUTCMonth(), d.getUTCDate()) - start) / DAY),
  };
}

export function dayStart(ms) {
  return Math.floor(ms / DAY) * DAY;
}

/** Returns {index, startMs, endMs} of the season containing ms. */
export function season(ms) {
  const p = dateParts(ms);
  let index = 0;
  for (let i = 0; i < SEASON_STARTS.length; i++) {
    const [m, d] = SEASON_STARTS[i];
    if (m < p.month || (m === p.month && d <= p.mday)) index = i;
    else break;
  }
  const startMs = Date.UTC(p.year, SEASON_STARTS[index][0] - 1, SEASON_STARTS[index][1]);
  const endMs = index + 1 < SEASON_STARTS.length
    ? Date.UTC(p.year, SEASON_STARTS[index + 1][0] - 1, SEASON_STARTS[index + 1][1])
    : Date.UTC(p.year + 1, 0, 1);
  return { index, startMs, endMs };
}

/** Categories: 0 season, 1 start-of-day (daily), 2 friends (season), 3 EU alt season, 4 EU alt daily. */
export function categoryKind(category) {
  return category === 1 || category === 4 ? 'daily' : 'season';
}

export function windowStart(category, ms) {
  return categoryKind(category) === 'daily' ? dayStart(ms) : season(ms).startMs;
}
