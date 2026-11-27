// The Durable Object: one instance ("main") owns all state, so every request is serialized and storage stays
// consistent without any locking. SQLite lives in the object (free plan: use new_sqlite_classes).

import { DurableObject } from 'cloudflare:workers';
import { createApp } from './core/app.js';

const SWEEP_MS = 60_000;
const SETTLED_SWEEP_MS = 120_000; // only finished rooms left to delete: no hurry

export class GameServer extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.app = createApp({
      sql: ctx.storage.sql,
      tx: (fn) => ctx.storage.transactionSync(fn),
      env,
    });
  }

  async fetch(request) {
    const resp = await this.app.handle(request);
    if ((await this.ctx.storage.getAlarm()) === null) await this.ctx.storage.setAlarm(Date.now() + SWEEP_MS);
    return resp;
  }

  async alarm() {
    this.app.touchAlarm(); // an alarm run is a Durable Object request on the platform's bill: counted in the object count
    const more = this.app.sweep();
    await this.app.turnTick();
    if (more) await this.ctx.storage.setAlarm(Date.now() + (more === 'settled' ? SETTLED_SWEEP_MS : SWEEP_MS));
  }
}
