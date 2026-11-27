// Stand-in for the platform module "cloudflare:workers" so src/worker.js (and src/do.js) can be imported by the node suites (test/core).
// The workerd suites (test/worker*, @cloudflare/vitest-plugin) use the real module.
export class DurableObject {
  constructor(ctx, env) { this.ctx = ctx; this.env = env; }
}
