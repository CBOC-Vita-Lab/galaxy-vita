// The numbers the README states about the Galaxy deployment are the numbers in wrangler.smg.toml (review: the README said 100 sign-ups and
// 5,000 accounts a day while the file said 10,000 and 50,000), and every variable the toml sets is one the code reads.
import { describe, it, expect } from 'vitest';
import { readFileSync } from 'node:fs';
import { loadConfig } from '../../src/core/config.js';

const dir = new URL('../../', import.meta.url);
const toml = readFileSync(new URL('wrangler.smg.toml', dir), 'utf8');
const readme = readFileSync(new URL('README.md', dir), 'utf8');
const vars = {};
for (const m of toml.matchAll(/^([A-Z][A-Z0-9_]*) = "([^"]*)"/gm)) vars[m[1]] = m[2];
const fmt = (n) => [String(n), Number(n).toLocaleString('en-US')];

describe('README against wrangler.smg.toml', () => {
  it('the Caps paragraph states the toml\'s REGISTER_MAX_PER_DAY and MAX_PLAYERS', () => {
    const caps = readme.split('\n').find((l) => l.startsWith('**Caps.**')) || '';
    for (const k of ['REGISTER_MAX_PER_DAY', 'MAX_PLAYERS']) {
      expect(fmt(vars[k]).some((f) => new RegExp(`\`${k}\` ${f}\\b`).test(caps)), `${k}=${vars[k]} in: ${caps.slice(0, 160)}`).toBe(true);
    }
  });

  it('the quota variables of the toml are the ones the README documents, with the same values in the Quota defence table', () => {
    for (const k of ['DAILY_REQUEST_BUDGET', 'LIVE_RESERVE_PCT', 'POLL_PACE_MAX_PCT', 'ADDR_DAILY_OPEN', 'ADDR_DAILY_PLAYER', 'ADDR_DAILY_LIVE', 'PID_DAILY_MAX', 'EDGE_IP_BURST', 'EDGE_IP_PER_SEC', 'EDGE_IP_AGG_BURST', 'EDGE_IP_AGG_PER_SEC', 'ADDR_BAD_DAY']) {
      expect(vars[k], k).toBeDefined();
      expect(readme.includes(k), `README mentions ${k}`).toBe(true);
    }
    expect(readme).toMatch(new RegExp(`\`ADDR_DAILY_OPEN\` ${fmt(vars.ADDR_DAILY_OPEN).map((x) => x.replace(',', ',?')).join('|')}`));
    expect(readme).toMatch(new RegExp(`\`PID_DAILY_MAX\` ${fmt(vars.PID_DAILY_MAX)[1].replace(',', ',?')}`));
    expect(readme).toMatch(new RegExp(`${fmt(vars.DAILY_REQUEST_BUDGET)[1].replace(',', ',?')}\\) admitted requests`));
  });

  it('the code reads the toml values as stated (no typo in a variable name: the default would be used silently)', () => {
    const c = loadConfig({ SERVICE_GAME: 'smg', ...vars });
    expect(c.dailyBudget).toBe(Number(vars.DAILY_REQUEST_BUDGET));
    expect(c.liveReservePct).toBe(Number(vars.LIVE_RESERVE_PCT));
    expect(c.paceMax).toBe(Number(vars.POLL_PACE_MAX_PCT) / 100);
    expect(c.abuse).toEqual({ openPerDay: Number(vars.ADDR_DAILY_OPEN), playerPerDay: Number(vars.ADDR_DAILY_PLAYER), livePerDay: Number(vars.ADDR_DAILY_LIVE), pidPerDay: Number(vars.PID_DAILY_MAX), badPerDay: Number(vars.ADDR_BAD_DAY) });
    expect(c.register.maxPerDay).toBe(Number(vars.REGISTER_MAX_PER_DAY));
    expect(c.maxPlayers).toBe(Number(vars.MAX_PLAYERS));
    expect(c.register.evictUnusedMs).toBe(Number(vars.REGISTER_EVICT_UNUSED_HOURS) * 3600_000);
    expect(c.register.pruneUnusedMs).toBe(Number(vars.PRUNE_UNUSED_DAYS) * 86400_000);
  });

  it('the shared account budget: Galaxy 45,000 in the toml and as the code default, so with Strikers\' 45,000 the sum is at most 90,000', () => {
    expect(Number(vars.DAILY_REQUEST_BUDGET)).toBe(45000);
    expect(loadConfig({ SERVICE_GAME: 'smg' }).dailyBudget).toBe(45000);
    expect(Number(vars.DAILY_REQUEST_BUDGET) + 45000).toBeLessThanOrEqual(90000);
    expect(readme).not.toMatch(/DAILY_REQUEST_BUDGET[^\n]{0,40}60,?000/);
  });
});
