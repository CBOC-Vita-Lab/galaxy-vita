// The request set of the 1.2 test builds (native/platform/sdk/dwc/np_online_http.cpp and net/online/np_signal_http.cpp: the same paths,
// headers and bodies), replayed against an application. Returns, per request, the status and the sorted key paths of the JSON answer.
// test/core/compat.test.js runs it against the new server and compares with test/fixtures/compat-1.2-shapes.json, which
// test/make-compat-fixture.mjs recorded from the deployed server code (e46c3e3).

const APP = { 'x-msc-proto': '1', 'x-msc-app': '1.2.0', 'x-msc-build': 'a865ff18' };

function keyPaths(v, prefix = '', out = new Set()) {
  if (Array.isArray(v)) { for (const x of v) keyPaths(x, `${prefix}[]`, out); return out; }
  if (v && typeof v === 'object') {
    for (const [k, x] of Object.entries(v)) { out.add(`${prefix}.${k}`); keyPaths(x, `${prefix}.${k}`, out); }
  }
  return out;
}

/**
 * @param {(method: string, path: string, body: any, headers: Record<string,string>) => Promise<{status:number, data:any}>} call
 * @param {(ms: number) => void} advance
 * @param {string} blobFor  name -> base64 0x80 submission with a valid digest
 */
export async function runOldClient(call, advance, blobFor) {
  const out = [];
  const step = async (name, method, path, body, headers = {}) => {
    const r = await call(method, path, body, { ...APP, ...headers });
    out.push({ name, status: r.status, keys: [...keyPaths(r.data)].sort() });
    return r;
  };
  const auth = (t) => ({ authorization: `Bearer ${t}` });
  await step('info', 'GET', '/v1/info');
  await step('time', 'GET', '/v1/time');
  const ra = await step('register', 'POST', '/v1/register', { name: 'OldHost', region: 2 });
  const rb = await step('register 2', 'POST', '/v1/register', { name: 'OldGuest', region: 2 });
  const a = ra.data.token; const b = rb.data.token;
  await step('me', 'GET', '/v1/me', undefined, auth(a));
  await step('me put', 'PUT', '/v1/me', { relayOnly: false }, auth(a));
  const room = await step('room create', 'POST', '/v1/rooms', {}, auth(a));
  const code = room.data.code;
  await step('room join', 'POST', '/v1/rooms/join', { code }, auth(b));
  const g = await step('room get', 'GET', `/v1/rooms/${code}`, undefined, auth(a));
  await step('room get v', 'GET', `/v1/rooms/${code}?v=${g.data.version}`, undefined, auth(a));
  const ep = (n) => ({ public: `203.0.113.${n}:41000`, local: `192.168.1.${n}:41000`, nonce: 'AAAAAAAAAAA=', nat: 'cone', candidates: [{ type: 'host', addr: `192.168.1.${n}:41000` }] });
  await step('endpoint host', 'POST', `/v1/rooms/${code}/endpoint`, ep(1), auth(a));
  await step('endpoint guest', 'POST', `/v1/rooms/${code}/endpoint`, ep(2), auth(b));
  await step('punch', 'POST', `/v1/rooms/${code}/punch`, { result: 'ok', path: 'direct', ms: 120 }, auth(a));
  await step('turn', 'POST', '/v1/turn', { room: code }, auth(a));
  await step('turn usage', 'POST', '/v1/turn/usage', { bytes: 1000 }, auth(a));
  await step('started host', 'POST', `/v1/rooms/${code}/started`, {}, auth(a));
  const st = await step('started guest', 'POST', `/v1/rooms/${code}/started`, {}, auth(b));
  const matchId = st.data.matchId;
  advance(200_000);
  const rh = { 'x-msc-attempt': '1', 'x-msc-mode': 'unranked' };
  const body = { result: 0, score0: 2, score1: 1, checksum: '00112233aabbccdd', attempt: 1, mode: 'unranked' };
  await step('result host', 'POST', `/v1/matches/${matchId}/result`, body, { ...auth(a), ...rh });
  await step('result guest', 'POST', `/v1/matches/${matchId}/result`, body, { ...auth(b), ...rh });
  await step('result again', 'POST', `/v1/matches/${matchId}/result`, body, { ...auth(b), ...rh });
  await step('match', 'GET', `/v1/matches/${matchId}`, undefined, auth(a));
  await step('leave', 'POST', `/v1/rooms/${code}/leave`, {}, auth(b));
  await step('rank put', 'POST', '/v1/rank/put', { category: 0, region: 2, score: 0, data: blobFor('OldHost') }, auth(a));
  await step('rank get near', 'POST', '/v1/rank/get', { mode: 2, category: 0, region: 255, sort: 1, since: 60, limit: 10 }, auth(a));
  await step('rank get top', 'POST', '/v1/rank/get', { mode: 1, category: 1, region: 255, sort: 1, since: 60, limit: 10 }, auth(a));
  await step('rank get friends', 'POST', '/v1/rank/get', { mode: 3, category: 0, region: 255, sort: 1, since: 0, limit: 3, friends: [1, 2] }, auth(a));
  await step('rank get order', 'POST', '/v1/rank/get', { mode: 0, category: 0, region: 255, sort: 1, since: 0 }, auth(a));
  await step('public board', 'GET', '/v1/public/leaderboard', undefined, {});
  await step('public daily', 'GET', '/v1/public/leaderboard?board=daily&limit=50', undefined, {});
  await step('ranked queue', 'POST', '/v1/ranked/queue', undefined, auth(a));
  await step('ranked poll', 'GET', '/v1/ranked/queue', undefined, auth(a));
  await step('ranked leave', 'DELETE', '/v1/ranked/queue', undefined, auth(a));
  await step('old version', 'GET', '/v1/me', undefined, { ...auth(a), 'x-msc-app': 'garbage!' });
  await step('no token', 'GET', '/v1/me', undefined, {});
  return out;
}
