// SqlStorage-compatible adapter over node:sqlite (Node >= 22.5). Used by the unit tests and the Node fake server;
// the Worker itself uses the Durable Object's built-in SQLite.

import { DatabaseSync } from 'node:sqlite';

export function openNodeSql(path = ':memory:') {
  const db = new DatabaseSync(path);
  const cache = new Map();
  let depth = 0;
  const sql = {
    exec(query, ...binds) {
      let st = cache.get(query);
      if (!st) {
        st = db.prepare(query);
        cache.set(query, st);
      }
      const rows = st.all(...binds).map((r) => ({ ...r }));
      return { toArray: () => rows, one: () => rows[0] };
    },
  };
  const tx = (fn) => {
    if (depth > 0) return fn();
    depth++;
    db.exec('BEGIN');
    try {
      const r = fn();
      db.exec('COMMIT');
      return r;
    } catch (e) {
      db.exec('ROLLBACK');
      throw e;
    } finally {
      depth--;
    }
  };
  return { sql, tx, close: () => db.close(), raw: db };
}
