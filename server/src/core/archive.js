// Season archive (1.3, "status that lasts"): the final standings of every past weekly season of the season board (category 0).
// A row of `scores` only counts while its last update lies in the current season, and the player's first put of a new season overwrites
// it, so the final standing would be lost. Two places keep it:
//   * the alarm sweep, once per season change: every category-0 row last updated before the current season is copied into
//     `season_scores` under the season it belongs to, then each archived season is trimmed to its best ARCHIVE_TOP rows;
//   * a put that overwrites an old-season row before that sweep ran (the first minute of a season) copies the row first.
// Copies use INSERT OR IGNORE: running either path again changes nothing. Only scores above 0 are kept (a 0 is the game's reset row).

import { season } from './calendar.js';

const CATEGORY = 0;
const DONE_KEY = 'archived_before';

function archivedBefore(c) {
  const r = c.db.get(`SELECT value FROM settings WHERE key = ?`, DONE_KEY);
  return r ? Number(r.value) || 0 : 0;
}

/** Copy one old-season row (category 0) into the archive, unless the season change was already archived. */
export function archiveBeforeOverwrite(c, category, row) {
  if (category !== CATEGORY || !row || row.score <= 0) return;
  const cur = season(c.now).startMs;
  if (row.updated_ms >= cur || archivedBefore(c) >= cur) return;
  c.db.run(
    `INSERT OR IGNORE INTO season_scores (season_start_ms, pid, score, data_b64, updated_ms) VALUES (?, ?, ?, ?, ?)`,
    season(row.updated_ms).startMs, row.pid, row.score, row.data_b64, row.updated_ms,
  );
}

/** Alarm: archive the seasons that ended since the last run. Returns the number of rows looked at. */
export function archiveSeasons(c) {
  const cur = season(c.now).startMs;
  const from = archivedBefore(c);
  if (from >= cur) return 0;
  const rows = c.db.all(
    `SELECT pid, score, data_b64, updated_ms FROM scores WHERE category = ? AND updated_ms >= ? AND updated_ms < ? AND score > 0`,
    CATEGORY, from, cur,
  );
  const seasons = new Set();
  for (const r of rows) {
    const start = season(r.updated_ms).startMs;
    seasons.add(start);
    c.db.run(
      `INSERT OR IGNORE INTO season_scores (season_start_ms, pid, score, data_b64, updated_ms) VALUES (?, ?, ?, ?, ?)`,
      start, r.pid, r.score, r.data_b64, r.updated_ms,
    );
  }
  for (const start of c.db.all(`SELECT DISTINCT season_start_ms AS s FROM season_scores WHERE season_start_ms >= ?`, season(from || 0).startMs)) seasons.add(start.s);
  for (const start of seasons) {
    c.db.run(
      `DELETE FROM season_scores WHERE season_start_ms = ?1 AND pid NOT IN (
         SELECT pid FROM season_scores WHERE season_start_ms = ?1 ORDER BY score DESC, updated_ms ASC, pid ASC LIMIT ?2)`,
      start, c.cfg.archiveTop,
    );
  }
  c.db.run(`INSERT INTO settings (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value`, DONE_KEY, String(cur));
  return rows.length;
}
