// Match records and result checks.
//
// Every room that reaches "started" has a match record. For ranked matches the outcome is decided
// by what BOTH players report:
//   verified   both reported result 0 with identical scores and checksum -> points, rating, credit.
//   disputed   both reported result 0 but the scores or checksum differ  -> nobody gains or loses.
//   no_contest a desync (result 2) or both sides lost the connection (3/4) -> nobody gains or loses.
//   abandoned  only one player reported within the window                 -> the silent player is the leaver:
//                                                                            forfeit loss, rating loss, queue cooldown;
//                                                                            the other gets a flat forfeit win.
//   void       the match never started                                    -> nothing.
//
// Abuse controls on top (all configurable, see README.md and PROTOCOL.md section 6):
//   * a normal finish (result 0) sooner than MIN_MATCH_SEC after both sides started is refused (429, reason too_early);
//   * a verified ranked match beyond the per-pair / per-account daily cap settles as no_contest (reason daily_cap_*);
//   * a winner whose rated wins come mostly from one opponent gets a stored flag (never an automatic ban);
//   * disputes / no-contests are blamed on the player(s) who caused them; a player blamed too often is flagged and
//     loses part of the rating and the start-penalty restore, so dodging a loss never beats reporting it honestly.
// "Credit" is what makes the leaderboard honest: DWC puts only raise a score by what verified matches earned
// (see ranking.js). The game takes 5 points away when a ranked match starts and gives them back at the end, so
// every settled match also grants that "restore" credit, except for the leaver.

import { ApiError, isInt, strict } from './util.js';
import { bumpDaily, bumpPairRated, getDaily, pairRatedToday } from './limits.js';

export const RESULT_CODES = [0, 2, 3, 4]; // the game's own result codes (CalculateAndReportGameResult)
export const CREDIT_CATEGORIES = [0, 1, 2, 3, 4];

/** Mirror of CalculateNetworkResultPoints (NetworkStatsManager.cpp) for one side. */
export function calcPoints(result, own, opp) {
  let resultPoints = own > opp ? 10 : 1;
  let goalPoints = Math.min(own, 10);
  let won = own > opp;
  if (result === 3 || result === 4) {
    resultPoints = 0;
    won = false;
  } else if (result === 2) {
    resultPoints = 0;
    goalPoints = 0;
    won = false;
  }
  return { points: resultPoints + goalPoints, won };
}

export function eloExpected(ra, rb) {
  return 1 / (1 + Math.pow(10, (rb - ra) / 400));
}

function matchView(c, m, pid) {
  const slot = m.p0 === pid ? 0 : 1;
  const results = c.db.all(`SELECT * FROM results WHERE match_id = ?`, m.match_id);
  const mine = results.find((r) => r.slot === slot);
  const theirs = results.find((r) => r.slot !== slot);
  const deadline = m.first_result_ms && !m.resolved_ms ? m.first_result_ms + c.cfg.matches.resultWindowMs : null;
  const view = {
    matchId: m.match_id,
    kind: m.kind,
    status: m.status,
    final: !!m.resolved_ms,
    slot,
    leaverSlot: m.leaver_slot === null || m.leaver_slot === undefined ? null : m.leaver_slot,
    submitted: { you: !!mine, peer: !!theirs },
    attempts: mine ? mine.attempt : 0,
    // both clients' build fingerprints, once both reported: a mismatch explains a failed agreement
    buildsMatch: mine && theirs && mine.build && theirs.build ? mine.build === theirs.build : null,
    resultDeadlineMs: deadline,
    scores: m.status === 'verified' ? { slot0: m.score0, slot1: m.score1 } : null,
    credit: null,
    // why a settled match was not rated (e.g. daily_cap_pair) and what this player was charged for causing a dispute
    reason: m.reason || null,
    penalty: (slot === 0 ? m.pen0 : m.pen1) > 0 ? { rating: -(slot === 0 ? m.pen0 : m.pen1), restoreWithheld: true } : null,
  };
  if (m.status === 'verified' || m.status === 'abandoned') {
    const own = slot === 0 ? m.score0 : m.score1;
    const opp = slot === 0 ? m.score1 : m.score0;
    if (m.status === 'verified') {
      const r = calcPoints(0, own, opp);
      view.credit = { points: r.points, won: r.won };
    } else {
      view.credit = m.leaver_slot === slot || m.reason ? { points: 0, won: false } : { points: c.cfg.matches.forfeitPoints, won: true };
    }
  }
  return view;
}

export function submitResult(c, p, matchId, body) {
  strict(body, ['result', 'score0', 'score1', 'checksum', 'attempt', 'build', 'mode']);
  const { result, score0, score1 } = body;
  const attempt = body.attempt === undefined ? 1 : body.attempt;
  if (!isInt(attempt, 1, 100000)) throw new ApiError(400, 'bad_request', 'attempt must be 1..100000', { field: 'attempt' });
  const build = body.build === undefined ? '' : String(body.build).toLowerCase();
  if (build && !/^[0-9a-f]{8,64}$/.test(build)) throw new ApiError(400, 'bad_request', 'build is 8..64 hex characters', { field: 'build' });
  if (body.mode !== undefined && body.mode !== 'ranked' && body.mode !== 'unranked') throw new ApiError(400, 'bad_request', 'mode must be ranked or unranked', { field: 'mode' });
  const checksum = typeof body.checksum === 'string' ? body.checksum.toLowerCase() : '';
  if (!RESULT_CODES.includes(result)) throw new ApiError(400, 'bad_result', 'result must be 0, 2, 3 or 4', { field: 'result' });
  if (!isInt(score0, 0, 99) || !isInt(score1, 0, 99)) throw new ApiError(400, 'bad_score', 'scores are integers 0..99', { field: 'score' });
  if (!/^[0-9a-f]{8,64}$/.test(checksum)) throw new ApiError(400, 'bad_checksum', 'checksum is 8..64 hex characters', { field: 'checksum' });
  if (typeof matchId !== 'string' || !/^[A-Za-z0-9_-]{8,24}$/.test(matchId)) throw new ApiError(400, 'bad_match', 'bad match id');
  // Settle anything overdue first, in its own transaction: the checks below may throw, which would undo it.
  c.tx(() => resolveDue(c, matchId));
  return c.tx(() => {
    const m = c.db.get(`SELECT * FROM matches WHERE match_id = ?`, matchId);
    if (!m || (m.p0 !== p.pid && m.p1 !== p.pid)) throw new ApiError(404, 'match_not_found', 'no such match');
    const slot = m.p0 === p.pid ? 0 : 1;
    if (m.status === 'pending' || m.status === 'void') {
      throw new ApiError(409, 'match_not_started', 'the match never started', { status: m.status });
    }
    const prev = c.db.get(`SELECT * FROM results WHERE match_id = ? AND slot = ?`, matchId, slot);
    if (body.mode !== undefined && body.mode !== m.kind) throw new ApiError(409, 'mode_mismatch', 'the match is not of that mode', { kind: m.kind });
    if (prev) {
      if (prev.result === result && prev.score0 === score0 && prev.score1 === score1 && prev.checksum === checksum) {
        // idempotent retry (the client's persistent retry queue may resend after a crash or suspend): same answer, no change
        c.db.run(`UPDATE results SET attempt = MAX(attempt, ?) WHERE match_id = ? AND slot = ?`, attempt, matchId, slot);
        return { ...matchView(c, m, p.pid), duplicate: true };
      }
      throw new ApiError(409, 'already_submitted', 'a different result was already submitted');
    }
    if (m.resolved_ms) throw new ApiError(409, 'match_closed', 'the match is already settled', { status: m.status });
    // A real match cannot end sooner than its shortest legal length. Answered as 429 (a code every client already
    // knows) with the time left, so a client with a skewed clock just retries; a forged instant result gains nothing.
    const minMs = c.cfg.matches.minMatchMs;
    if (result === 0 && m.kind === 'ranked' && minMs > 0 && m.started_ms && c.now < m.started_ms + minMs) {
      const wait = Math.max(1, Math.ceil((m.started_ms + minMs - c.now) / 1000));
      throw new ApiError(429, 'rate_limited', 'that match cannot be over yet', { retryAfterSec: wait, reason: 'too_early' }, { 'retry-after': String(wait) });
    }
    c.db.run(
      `INSERT INTO results (match_id, slot, pid, result, score0, score1, checksum, submitted_ms, attempt, build, mode)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
      matchId, slot, p.pid, result, score0, score1, checksum, c.now, attempt, build, body.mode || '',
    );
    if (!m.first_result_ms) c.db.run(`UPDATE matches SET first_result_ms = ?, status = 'awaiting_peer' WHERE match_id = ?`, c.now, matchId);
    evaluate(c, matchId, false);
    return matchView(c, c.db.get(`SELECT * FROM matches WHERE match_id = ?`, matchId), p.pid);
  });
}

export function getMatch(c, p, matchId) {
  if (typeof matchId !== 'string' || !/^[A-Za-z0-9_-]{8,24}$/.test(matchId)) throw new ApiError(400, 'bad_match', 'bad match id');
  c.tx(() => resolveDue(c, matchId));
  return c.tx(() => {
    const m = c.db.get(`SELECT * FROM matches WHERE match_id = ?`, matchId);
    if (!m || (m.p0 !== p.pid && m.p1 !== p.pid)) throw new ApiError(404, 'match_not_found', 'no such match');
    return matchView(c, m, p.pid);
  });
}

export function listMyMatches(c, p, limit) {
  const rows = c.db.all(
    `SELECT match_id FROM matches WHERE (p0 = ? OR p1 = ?) ORDER BY created_ms DESC LIMIT ?`, p.pid, p.pid, limit,
  );
  return rows.map((r) => matchView(c, c.db.get(`SELECT * FROM matches WHERE match_id = ?`, r.match_id), p.pid));
}

/** Time-based settling of one match: window expiry, never-started, stuck-in-progress. */
export function resolveDue(c, matchId) {
  const m = c.db.get(`SELECT * FROM matches WHERE match_id = ?`, matchId);
  if (!m || m.resolved_ms) return;
  if (m.status === 'pending' && c.now > m.created_ms + c.cfg.matches.startWaitMs) {
    c.db.run(`UPDATE matches SET status = 'void', resolved_ms = ? WHERE match_id = ?`, c.now, matchId);
  } else if (m.status === 'started' && c.now > m.started_ms + c.cfg.matches.maxMatchMs) {
    settle(c, m, 'no_contest', null, null);
  } else if (m.status === 'awaiting_peer' && c.now > m.first_result_ms + c.cfg.matches.resultWindowMs) {
    evaluate(c, matchId, true);
  }
}

/** Decide the outcome from the stored reports. `windowOver` = nobody else can still report. */
function evaluate(c, matchId, windowOver) {
  const m = c.db.get(`SELECT * FROM matches WHERE match_id = ?`, matchId);
  if (!m || m.resolved_ms) return;
  const rs = c.db.all(`SELECT * FROM results WHERE match_id = ? ORDER BY slot`, matchId);
  if (rs.length === 2) {
    const [a, b] = rs;
    if (a.result === 0 && b.result === 0) {
      if (a.score0 === b.score0 && a.score1 === b.score1 && a.checksum === b.checksum) settle(c, m, 'verified', null, a);
      else settle(c, m, 'disputed', null, null);
    } else {
      settle(c, m, 'no_contest', null, null);
    }
  } else if (rs.length === 1 && windowOver) {
    const only = rs[0];
    if (only.result === 2) settle(c, m, 'no_contest', null, null);
    else settle(c, m, 'abandoned', 1 - only.slot, only);
  }
}

function addAllowance(c, pid, amount) {
  if (amount <= 0) return;
  for (const cat of CREDIT_CATEGORIES) {
    c.db.run(
      `INSERT INTO allowance (pid, category, amount) VALUES (?, ?, ?)
       ON CONFLICT(pid, category) DO UPDATE SET amount = MIN(?, amount + excluded.amount)`,
      pid, cat, Math.min(amount, c.cfg.rank.allowanceCap), c.cfg.rank.allowanceCap,
    );
  }
}

/** Why a verified ranked match must not be rated today ('' = it may be). */
function capReason(c, m) {
  const s = c.cfg.matches;
  if (s.ratedPerPairDay && pairRatedToday(c, m.p0, m.p1) >= s.ratedPerPairDay) return 'daily_cap_pair';
  if (s.ratedPerAccountDay && (getDaily(c, m.p0).rated >= s.ratedPerAccountDay || getDaily(c, m.p1).rated >= s.ratedPerAccountDay)) return 'daily_cap_account';
  return '';
}

function upsertFlag(c, pid, kind, detail) {
  c.db.run(
    `INSERT INTO flags (pid, kind, since_ms, updated_ms, hits, detail) VALUES (?, ?, ?, ?, 1, ?)
     ON CONFLICT(pid, kind) DO UPDATE SET updated_ms = excluded.updated_ms, hits = hits + 1, detail = excluded.detail`,
    pid, kind, c.now, c.now, JSON.stringify(detail),
  );
}

/** Flag a winner whose rated wins (last ONE_OPP_DAYS) come mostly from one opponent. Flag only; an admin decides. */
function checkSingleOpponent(c, pid) {
  const s = c.cfg.matches;
  const rows = c.db.all(
    `SELECT CASE WHEN p0 = ?1 THEN p1 ELSE p0 END AS opp, COUNT(*) AS n FROM matches
      WHERE kind = 'ranked' AND resolved_ms >= ?2 AND (p0 = ?1 OR p1 = ?1)
        AND ((status = 'verified' AND ((p0 = ?1 AND score0 > score1) OR (p1 = ?1 AND score1 > score0)))
          OR (status = 'abandoned' AND reason = '' AND leaver_slot = CASE WHEN p0 = ?1 THEN 1 ELSE 0 END))
      GROUP BY opp ORDER BY n DESC`,
    pid, c.now - s.oneOppDays * 86400_000,
  );
  const total = rows.reduce((a, r) => a + r.n, 0);
  if (total >= s.oneOppMinWins && rows[0].n * 100 >= s.oneOppSharePct * total) {
    upsertFlag(c, pid, 'single_opponent', { opponent: rows[0].opp, winsVsOpponent: rows[0].n, totalWins: total, days: s.oneOppDays });
  }
}

/** Slots blamed for a dispute / no-contest. A victim of someone else's bad report is never blamed. */
function blamedSlots(c, m, status) {
  const rs = c.db.all(`SELECT slot, result, build FROM results WHERE match_id = ? ORDER BY slot`, m.match_id);
  if (status === 'disputed') {
    // different builds explain a mismatch without anyone lying: blame nobody
    if (rs.length === 2 && rs[0].build && rs[1].build && rs[0].build !== rs[1].build) return [];
    return [0, 1];
  }
  if (rs.length === 0) return [0, 1]; // nobody reported in time
  return rs.filter((r) => r.result !== 0).map((r) => r.slot); // the side(s) that claimed a desync / lost link
}

/** Disputes and no-contests blamed on this player over their last DODGE_WINDOW settled ranked matches (this one included). */
export function dodgeStats(c, pid) {
  const s = c.cfg.matches;
  const rows = c.db.all(
    `SELECT p0, blame FROM matches
      WHERE (p0 = ?1 OR p1 = ?1) AND kind = 'ranked' AND resolved_ms IS NOT NULL AND reason = ''
        AND status IN ('verified','disputed','no_contest','abandoned')
      ORDER BY resolved_ms DESC, rowid DESC LIMIT ?2`,
    pid, s.dodgeWindow,
  );
  const blamed = rows.filter((r) => r.blame & (r.p0 === pid ? 1 : 2)).length;
  const flagged = rows.length >= s.dodgeMinSample && blamed * 100 >= s.dodgeRatePct * rows.length;
  return { matches: rows.length, blamed, flagged };
}

function settle(c, m, status, leaverSlot, agreed) {
  const s = c.cfg.matches;
  let reason = '';
  if (m.kind === 'ranked' && (status === 'verified' || status === 'abandoned')) {
    // forfeit wins count as rated wins too, or abandoning on purpose would be an uncapped way to feed a friend
    reason = capReason(c, m);
    if (reason && status === 'verified') { status = 'no_contest'; agreed = null; } // answered like any no-contest: nobody gains or loses, the 5 points come back
  }
  c.db.run(
    `UPDATE matches SET status = ?, leaver_slot = ?, score0 = ?, score1 = ?, resolved_ms = ?, reason = ? WHERE match_id = ?`,
    status, leaverSlot, agreed ? agreed.score0 : null, agreed ? agreed.score1 : null, c.now, reason, m.match_id,
  );
  if (m.kind !== 'ranked') return; // unranked: record only
  // a settled ranked match frees its players: the room ends and they may queue again
  c.db.run(
    `UPDATE rooms SET state = 'closed', closed_reason = 'match_over', expires_ms = ?, version = version + 1
     WHERE match_id = ? AND state IN ('open','joined','ready','started')`,
    c.now, m.match_id,
  );
  const pids = [m.p0, m.p1];
  const prof = pids.map((pid) => c.db.get(`SELECT * FROM profiles WHERE pid = ?`, pid));
  const updateRating = (winnerIdx) => {
    // winnerIdx: 0, 1 or -1 for a draw
    if (!prof[0] || !prof[1]) return;
    const e0 = eloExpected(prof[0].rating, prof[1].rating);
    const s0 = winnerIdx === 0 ? 1 : winnerIdx === 1 ? 0 : 0.5;
    const d0 = Math.round(s.eloK * (s0 - e0));
    c.db.run(`UPDATE profiles SET rating = MAX(100, rating + ?) WHERE pid = ?`, d0, m.p0);
    c.db.run(`UPDATE profiles SET rating = MAX(100, rating - ?) WHERE pid = ?`, d0, m.p1);
  };
  if (status === 'verified') {
    const r0 = calcPoints(0, agreed.score0, agreed.score1);
    const r1 = calcPoints(0, agreed.score1, agreed.score0);
    addAllowance(c, m.p0, r0.points + s.restoreCredit);
    addAllowance(c, m.p1, r1.points + s.restoreCredit);
    const tie = agreed.score0 === agreed.score1;
    updateRating(tie ? -1 : agreed.score0 > agreed.score1 ? 0 : 1);
    bumpDaily(c, m.p0, 'rated');
    bumpDaily(c, m.p1, 'rated');
    bumpPairRated(c, m.p0, m.p1);
    // status that lasts: all-time verified points and the date of the last verified ranked match (web leaderboard)
    c.db.run(`UPDATE profiles SET points_total = points_total + ?, last_played_ms = ? WHERE pid = ?`, r0.points, c.now, m.p0);
    c.db.run(`UPDATE profiles SET points_total = points_total + ?, last_played_ms = ? WHERE pid = ?`, r1.points, c.now, m.p1);
    if (tie) {
      c.db.run(`UPDATE profiles SET draws = draws + 1 WHERE pid IN (?, ?)`, m.p0, m.p1);
    } else {
      const w = agreed.score0 > agreed.score1 ? m.p0 : m.p1;
      c.db.run(`UPDATE profiles SET wins = wins + 1 WHERE pid = ?`, w);
      c.db.run(`UPDATE profiles SET losses = losses + 1 WHERE pid = ?`, w === m.p0 ? m.p1 : m.p0);
      checkSingleOpponent(c, w);
    }
    // a clean finish clears the abandon streak
    c.db.run(`UPDATE profiles SET abandon_streak = 0 WHERE pid IN (?, ?)`, m.p0, m.p1);
  } else if (status === 'disputed' || status === 'no_contest') {
    // Nobody wins or loses, but whoever caused it is blamed. A player blamed too often (dodge rule, PROTOCOL.md 6) is
    // flagged and then pays part of what an abandon costs: no restore of the start penalty and a rating loss of
    // DODGE_PENALTY_PCT % of an abandon's. Everyone else gets the restore as before.
    const blamed = reason ? [] : blamedSlots(c, m, status);
    const mask = (blamed.includes(0) ? 1 : 0) | (blamed.includes(1) ? 2 : 0);
    if (mask) c.db.run(`UPDATE matches SET blame = ? WHERE match_id = ?`, mask, m.match_id);
    for (const slot of [0, 1]) {
      const pid = pids[slot];
      if (!blamed.includes(slot)) { addAllowance(c, pid, s.restoreCredit); continue; }
      const st = dodgeStats(c, pid);
      if (!st.flagged) { addAllowance(c, pid, s.restoreCredit); continue; }
      const mine = prof[slot];
      const other = prof[1 - slot];
      const full = mine && other ? s.eloK * eloExpected(mine.rating, other.rating) : s.eloK / 2; // what losing to this opponent would cost
      const pen = s.dodgePenaltyPct > 0 ? Math.max(1, Math.round((full * s.dodgePenaltyPct) / 100)) : 0;
      if (pen) {
        c.db.run(`UPDATE profiles SET rating = MAX(100, rating - ?) WHERE pid = ?`, pen, pid);
        c.db.run(`UPDATE matches SET ${slot === 0 ? 'pen0' : 'pen1'} = ? WHERE match_id = ?`, pen, m.match_id);
      }
      upsertFlag(c, pid, 'dodger', { blamed: st.blamed, of: st.matches, windowMatches: s.dodgeWindow, ratePct: s.dodgeRatePct });
    }
  } else if (status === 'abandoned') {
    const leaver = leaverSlot === 0 ? m.p0 : m.p1;
    const stayer = leaverSlot === 0 ? m.p1 : m.p0;
    // leaver gets no restore: the points taken when the match began stay taken
    if (reason) {
      // over the daily cap: the leaver is still charged, the stayer gets no forfeit win (only the restore)
      addAllowance(c, stayer, s.restoreCredit);
      if (prof[0] && prof[1]) {
        const e = eloExpected(prof[leaverSlot].rating, prof[1 - leaverSlot].rating);
        c.db.run(`UPDATE profiles SET rating = MAX(100, rating - ?) WHERE pid = ?`, Math.round(s.eloK * e), leaver);
      }
      c.db.run(`UPDATE profiles SET losses = losses + 1 WHERE pid = ?`, leaver);
    } else {
      addAllowance(c, stayer, s.forfeitPoints + s.restoreCredit);
      updateRating(leaverSlot === 0 ? 1 : 0);
      c.db.run(`UPDATE profiles SET losses = losses + 1 WHERE pid = ?`, leaver);
      c.db.run(`UPDATE profiles SET wins = wins + 1 WHERE pid = ?`, stayer);
      bumpDaily(c, m.p0, 'rated');
      bumpDaily(c, m.p1, 'rated');
      bumpPairRated(c, m.p0, m.p1);
      checkSingleOpponent(c, stayer);
    }
    const p = prof[leaverSlot];
    // streak counts abandons within 24 h of each other
    const streak = p && c.now - p.last_abandon_ms < 24 * 3600_000 ? p.abandon_streak + 1 : 1;
    const cool = Math.min(s.cooldownMaxSec, s.cooldownBaseSec * streak) * 1000;
    c.db.run(
      `UPDATE profiles SET abandons = abandons + 1, abandon_streak = ?, last_abandon_ms = ?, cooldown_until_ms = ? WHERE pid = ?`,
      streak, c.now, c.now + cool, leaver,
    );
  }
}

/** Housekeeping for the alarm: settle everything that is due. */
export function sweepMatches(c) {
  const due = c.db.all(
    `SELECT match_id FROM matches WHERE resolved_ms IS NULL AND (
        (status = 'pending' AND created_ms + ? < ?) OR
        (status = 'started' AND started_ms + ? < ?) OR
        (status = 'awaiting_peer' AND first_result_ms + ? < ?))`,
    c.cfg.matches.startWaitMs, c.now, c.cfg.matches.maxMatchMs, c.now, c.cfg.matches.resultWindowMs, c.now,
  );
  for (const r of due) resolveDue(c, r.match_id);
  c.db.run(`DELETE FROM results WHERE match_id IN (SELECT match_id FROM matches WHERE resolved_ms IS NOT NULL AND resolved_ms < ?)`, c.now - c.cfg.matches.keepMs);
  c.db.run(`DELETE FROM matches WHERE resolved_ms IS NOT NULL AND resolved_ms < ?`, c.now - c.cfg.matches.keepMs);
}
