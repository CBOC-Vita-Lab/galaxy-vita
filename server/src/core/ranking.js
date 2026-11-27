// DWC-style leaderboard service. Mirrors the contract of DWC_RnkPutScoreAsync / DWC_RnkGetScoreAsync that the
// game was built against (native/ref/RVL_SDK/dwc), but over JSON:
//   put   category 0..100, region mask bit, score, data <= 768 bytes (the game sends a 0x80 NetworkRankingSubmission
//         whose last 16 bytes are an HMAC-MD5 over the first 0x70 bytes).
//   get   mode 0 ORDER, 1 TOPLIST (limit 1..10), 2 NEAR (limit 2..10, must contain the caller's row with order >= 1),
//         3 FRIENDS (limit 2..65, up to 64 pids).
// The server decides which rows are returned and which time window counts (see calendar.js); `since` from the client is
// validated and then ignored. Rows carry the opaque userdata blob byte-for-byte.

import { ApiError, b64decode, b64encode, isInt, strict } from './util.js';
import { hmacMd5 } from './md5.js';
import { windowStart } from './calendar.js';
import { archiveBeforeOverwrite } from './archive.js';

export const DATA_MAX = 768;
export const BLOB_SIZE = 0x80;
const DIGEST_OFFSET = 0x70;
const NAME_OFFSET = 0x0e; // u16 mName[11]
export const GET_MAX = 10;
export const FRIENDS_MAX = 64;
export const CATEGORY_MAX = 100;
const REGION_BITS = [1, 2, 4, 8];

export const MODES = { ORDER: 0, TOPLIST: 1, NEAR: 2, FRIENDS: 3 };

function digestOk(key, blob) {
  const want = hmacMd5(key, blob.subarray(0, DIGEST_OFFSET));
  let d = 0;
  for (let i = 0; i < 16; i++) d |= want[i] ^ blob[DIGEST_OFFSET + i];
  return d === 0;
}

/** The name inside the blob (NUL-terminated UTF-16 in either byte order) must be the registered one. */
function blobNameMatches(blob, name) {
  for (const le of [true, false]) {
    const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
    let s = '';
    for (let i = 0; i < 11; i++) {
      const u = dv.getUint16(NAME_OFFSET + i * 2, le);
      if (u === 0) break;
      s += String.fromCharCode(u);
    }
    if (s === name) return true;
  }
  return false;
}

function getAllowance(c, pid, category) {
  const r = c.db.get(`SELECT amount FROM allowance WHERE pid = ? AND category = ?`, pid, category);
  return r ? r.amount : 0;
}

/**
 * Field-level sanity of a NetworkRankingSubmission in either byte order (the game ran big-endian on the Wii,
 * little-endian on the Vita): date 1..31 / 1..12 / 2000..2100, wins and losses 0..9999, region 0..15.
 */
export function blobFieldsSane(blob) {
  return blobByteOrder(blob) !== null;
}

/** The byte order (true = little-endian) in which the fields of a 0x80 blob are sane, or null when neither is. */
export function blobByteOrder(blob) {
  const dv = new DataView(blob.buffer, blob.byteOffset, blob.byteLength);
  for (const le of [true, false]) {
    const day = blob[0], month = blob[1], year = dv.getUint16(2, le);
    const wins = dv.getInt32(4, le), losses = dv.getInt32(8, le), region = dv.getUint16(0x0c, le);
    if (day >= 1 && day <= 31 && month >= 1 && month <= 12 && year >= 2000 && year <= 2100
      && wins >= 0 && wins <= 9999 && losses >= 0 && losses <= 9999 && region <= 15) return le;
  }
  return null;
}

export function putScore(c, p, body) {
  strict(body, ['category', 'region', 'score', 'data']);
  const { category, region, score } = body;
  const rk = c.cfg.rank;
  if (!isInt(category, 0, CATEGORY_MAX)) throw new ApiError(400, 'bad_category', 'category must be 0..100', { field: 'category' });
  if (!REGION_BITS.includes(region)) throw new ApiError(400, 'bad_region', 'region must be 1, 2, 4 or 8', { field: 'region' });
  if (!isInt(score, 0, rk.maxScore)) throw new ApiError(400, 'bad_score', `score must be 0..${rk.maxScore}`, { field: 'score' });
  const blob = b64decode(body.data === undefined ? '' : body.data);
  if (!blob) throw new ApiError(400, 'bad_blob', 'data must be base64', { field: 'data' });
  if (blob.length > DATA_MAX) throw new ApiError(400, 'bad_blob', `data is at most ${DATA_MAX} bytes`, { field: 'data' });
  if (rk.requireHmac) {
    if (blob.length !== BLOB_SIZE) throw new ApiError(400, 'bad_blob', `data must be exactly ${BLOB_SIZE} bytes`, { field: 'data' });
    if (!rk.hmacKey) throw new ApiError(503, 'server_misconfigured', 'ranking is not configured');
    if (!digestOk(rk.hmacKey, blob)) throw new ApiError(400, 'bad_digest', 'digest check failed', { field: 'data' });
  }
  if (blob.length === BLOB_SIZE && !blobFieldsSane(blob)) {
    throw new ApiError(400, 'bad_blob', 'fields inside data are out of range', { field: 'data' });
  }
  if (rk.checkName && blob.length === BLOB_SIZE && !blobNameMatches(blob, p.name)) {
    throw new ApiError(400, 'bad_blob_name', 'the name inside data does not match your registered name', { field: 'data' });
  }
  return c.tx(() => {
    const win = windowStart(category, c.now);
    const row = c.db.get(`SELECT pid, score, data_b64, updated_ms FROM scores WHERE category = ? AND pid = ?`, category, p.pid);
    const prev = row && row.updated_ms >= win ? row.score : 0;
    if (row && row.updated_ms < win) archiveBeforeOverwrite(c, category, row); // the final standing of the season that ended
    let stored = score;
    let clamped = false;
    if (score > prev) {
      const delta = score - prev;
      let allowed = delta;
      if (rk.putPolicy === 'credit') {
        allowed = Math.min(delta, getAllowance(c, p.pid, category));
        if (allowed > 0) {
          c.db.run(`UPDATE allowance SET amount = amount - ? WHERE pid = ? AND category = ?`, allowed, p.pid, category);
        }
      } else if (rk.putPolicy === 'delta') {
        allowed = Math.min(delta, rk.maxDelta);
      }
      clamped = allowed < delta;
      stored = prev + allowed;
    }
    c.db.run(
      `INSERT INTO scores (category, pid, region, score, data_b64, updated_ms) VALUES (?, ?, ?, ?, ?, ?)
       ON CONFLICT(category, pid) DO UPDATE SET region = excluded.region, score = excluded.score,
         data_b64 = excluded.data_b64, updated_ms = excluded.updated_ms`,
      category, p.pid, region, stored, b64encode(blob), c.now,
    );
    return { ok: true, stored, clamped };
  });
}

// ---------------------------------------------------------------- queries

function validateGet(body) {
  strict(body, ['mode', 'category', 'region', 'sort', 'since', 'limit', 'friends']);
  const bad = (field, msg) => new ApiError(400, 'bad_param', msg, { field });
  const mode = typeof body.mode === 'string' ? MODES[body.mode.toUpperCase()] : body.mode;
  if (![0, 1, 2, 3].includes(mode)) throw new ApiError(400, 'bad_mode', 'mode must be 0 ORDER, 1 TOPLIST, 2 NEAR or 3 FRIENDS');
  if (!isInt(body.category, 0, CATEGORY_MAX)) throw bad('category', 'category must be 0..100');
  const region = body.region === undefined ? 0xff : body.region;
  if (!isInt(region, 1, 0xff)) throw bad('region', 'region must be a mask 1..255');
  const sort = body.sort === undefined ? 1 : body.sort;
  if (sort !== 0 && sort !== 1) throw bad('sort', 'sort must be 0 (asc) or 1 (desc)');
  const since = body.since === undefined ? 0 : body.since;
  if (!isInt(since, 0, 0xffffffff)) throw bad('since', 'since must be a u32');
  const out = { mode, category: body.category, region, sort, since, limit: 0, friends: [] };
  if (mode === 1) {
    if (!isInt(body.limit, 1, GET_MAX)) throw bad('limit', 'TOPLIST limit must be 1..10');
    out.limit = body.limit;
  } else if (mode === 2) {
    if (!isInt(body.limit, 2, GET_MAX)) throw bad('limit', 'NEAR limit must be 2..10');
    out.limit = body.limit;
  } else if (mode === 3) {
    if (!isInt(body.limit, 2, FRIENDS_MAX + 1)) throw bad('limit', 'FRIENDS limit must be 2..65');
    const f = body.friends === undefined ? [] : body.friends;
    if (!Array.isArray(f) || f.length > FRIENDS_MAX) throw bad('friends', 'friends must be an array of at most 64 profile ids');
    if (!f.every((x) => Number.isInteger(x) && x >= 0 && x <= 2147483647)) throw bad('friends', 'bad friend id');
    out.limit = body.limit;
    out.friends = [...new Set(f.filter((x) => x > 0))];
  }
  return out;
}

function filters(c, q) {
  const win = windowStart(q.category, c.now);
  let where = `category = ? AND updated_ms >= ?`;
  const binds = [q.category, win];
  if (c.cfg.rank.regionMode === 'mask') {
    where += ` AND (region & ?) <> 0`;
    binds.push(q.region);
  }
  return { where, binds };
}

const ORDER_DESC = `score DESC, updated_ms ASC, pid ASC`;
const ORDER_ASC = `score ASC, updated_ms ASC, pid ASC`;

function toRow(r, order) {
  const blob = r.data_b64;
  return {
    order,
    pid: r.pid,
    score: r.score,
    region: r.region,
    lastupdate: Math.floor(r.updated_ms / 1000),
    size: Math.floor(blob.replace(/=+$/, '').length * 3 / 4),
    data: blob,
  };
}

const ZERO_BLOB = b64encode(new Uint8Array(BLOB_SIZE));

/** Row for a player with no (current) score: score 0, empty userdata (the client treats it as "no metadata"). */
function emptyRow(pid, order, region, now) {
  return { order, pid, score: 0, region, lastupdate: Math.floor(now / 1000), size: BLOB_SIZE, data: ZERO_BLOB };
}

/** Competition rank ("1224") of `score` in the filtered set. */
function rankOf(c, f, score, sort) {
  const cmp = sort === 1 ? '>' : '<';
  return 1 + c.db.get(`SELECT COUNT(*) AS n FROM scores WHERE ${f.where} AND score ${cmp} ?`, ...f.binds, score).n;
}

/** Assign competition ranks to an already ordered list, anchored on the first row's true rank. */
function assignOrders(c, f, rows, sort, startPos) {
  if (!rows.length) return [];
  const out = [];
  let rank = rankOf(c, f, rows[0].score, sort);
  out.push(toRow(rows[0], rank));
  for (let i = 1; i < rows.length; i++) {
    if (rows[i].score !== rows[i - 1].score) rank = startPos + i + 1;
    out.push(toRow(rows[i], rank));
  }
  return out;
}

function ownRowOrNull(c, f, pid) {
  return c.db.get(`SELECT pid, score, region, data_b64, updated_ms FROM scores WHERE ${f.where} AND pid = ?`, ...f.binds, pid);
}

export function getScores(c, p, body) {
  const q = validateGet(body);
  const f = filters(c, q);
  const ord = q.sort === 1 ? ORDER_DESC : ORDER_ASC;
  const total = c.db.get(`SELECT COUNT(*) AS n FROM scores WHERE ${f.where}`, ...f.binds).n;
  const head = { mode: q.mode, category: q.category, total, serverMs: c.now };

  if (q.mode === MODES.ORDER) {
    const own = ownRowOrNull(c, f, p.pid);
    return { ...head, count: own ? 1 : 0, order: rankOf(c, f, own ? own.score : 0, q.sort), score: own ? own.score : 0, rows: [] };
  }

  if (q.mode === MODES.TOPLIST) {
    const rows = c.db.all(
      `SELECT pid, score, region, data_b64, updated_ms FROM scores WHERE ${f.where} ORDER BY ${ord} LIMIT ?`, ...f.binds, q.limit,
    );
    const out = assignOrders(c, f, rows, q.sort, 0);
    return { ...head, count: out.length, rows: out };
  }

  const own = ownRowOrNull(c, f, p.pid);

  if (q.mode === MODES.NEAR) {
    // with no current row of their own the caller is added on top of what is stored
    const want = Math.min(q.limit, own ? total : total + 1);
    let rows;
    let startPos;
    let out;
    if (own) {
      const before = c.db.get(
        sortBeforeSql(f, q.sort), ...f.binds, ...beforeBinds(own, q.sort),
      ).n; // rows ahead of me in the total order
      const above = Math.floor((want - 1) / 2);
      startPos = Math.max(0, Math.min(before - above, total - want));
      rows = c.db.all(
        `SELECT pid, score, region, data_b64, updated_ms FROM scores WHERE ${f.where} ORDER BY ${ord} LIMIT ? OFFSET ?`,
        ...f.binds, want, startPos,
      );
      out = assignOrders(c, f, rows, q.sort, startPos);
    } else {
      // The caller has no current row: show the closest rows plus a synthesized score-0 row for them, so the
      // client always finds its own row with order >= 1.
      const n = Math.min(want - 1, total);
      startPos = q.sort === 1 ? Math.max(0, total - n) : 0;
      rows = n > 0
        ? c.db.all(
          `SELECT pid, score, region, data_b64, updated_ms FROM scores WHERE ${f.where} ORDER BY ${ord} LIMIT ? OFFSET ?`,
          ...f.binds, n, startPos,
        )
        : [];
      out = assignOrders(c, f, rows, q.sort, startPos);
      const mine = emptyRow(p.pid, rankOf(c, f, 0, q.sort), p.region, c.now);
      if (q.sort === 1) out.push(mine);
      else out.unshift(mine);
    }
    return { ...head, count: out.length, rows: out };
  }

  // FRIENDS: the caller plus the listed profile ids, best first.
  const ids = [p.pid, ...q.friends.filter((x) => x !== p.pid)];
  const ph = ids.map(() => '?').join(',');
  const rows = c.db.all(
    `SELECT pid, score, region, data_b64, updated_ms FROM scores WHERE ${f.where} AND pid IN (${ph}) ORDER BY ${ord} LIMIT ?`,
    ...f.binds, ...ids, q.limit,
  );
  const hasOwn = rows.some((r) => r.pid === p.pid);
  const outRows = [];
  let rank = 1;
  rows.forEach((r, i) => {
    if (i > 0 && r.score !== rows[i - 1].score) rank = i + 1;
    outRows.push(toRow(r, rank));
  });
  if (!hasOwn) {
    const mine = emptyRow(p.pid, q.sort === 1 ? rows.length + 1 : 1, p.region, c.now);
    if (q.sort === 1) {
      outRows.push(mine);
      if (outRows.length > q.limit) outRows.splice(outRows.length - 2, 1); // keep own row, drop the weakest friend
    } else {
      outRows.unshift(mine);
      if (outRows.length > q.limit) outRows.splice(outRows.length - 1, 1);
    }
  }
  return { ...head, count: outRows.length, rows: outRows };
}

// "rows strictly before `own` in the total order" (score, then older update, then lower pid)
function sortBeforeSql(f, sort) {
  const cmp = sort === 1 ? '>' : '<';
  return `SELECT COUNT(*) AS n FROM scores WHERE ${f.where} AND (score ${cmp} ? OR (score = ? AND (updated_ms < ? OR (updated_ms = ? AND pid < ?))))`;
}
function beforeBinds(own) {
  return [own.score, own.score, own.updated_ms, own.updated_ms, own.pid];
}
