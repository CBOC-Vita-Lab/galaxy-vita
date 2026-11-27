#!/usr/bin/env node
// Restores a backup made by backup.mjs: ADMIN_TOKEN=... node scripts/restore.mjs <base-url> < backup.json
import { readFileSync } from 'node:fs';
const base = (process.argv[2] || '').replace(/\/$/, '');
const token = process.env.ADMIN_TOKEN;
if (!base || !token) {
  console.error('usage: ADMIN_TOKEN=... node scripts/restore.mjs https://your-worker.workers.dev < backup.json');
  process.exit(2);
}
const backup = JSON.parse(readFileSync(0, 'utf8'));
if (backup.format !== 'msc-online-backup-1') throw new Error('not a backup file');
// parents before children is not needed (no foreign keys), but profiles first keeps the output readable
const order = ['profiles', 'scores', 'allowance', 'matches', 'results', 'rooms', 'queue', 'counters', 'settings'];
// every other table the backup holds too (flags, season_scores, pair_stats; Galaxy: friends, star_times, friend_blocks; room_members is not exported)
for (const t of Object.keys(backup.tables)) if (!order.includes(t)) order.push(t);
for (const table of order) {
  const rows = backup.tables[table] || [];
  for (let i = 0; i < rows.length; i += 500) {
    const r = await fetch(`${base}/v1/admin/import`, {
      method: 'POST',
      headers: { authorization: `Bearer ${token}`, 'content-type': 'application/json' },
      body: JSON.stringify({ table, rows: rows.slice(i, i + 500) }),
    });
    if (!r.ok) throw new Error(`${table}: HTTP ${r.status} ${await r.text()}`);
  }
  console.error(`${table}: ${rows.length} rows`);
}
