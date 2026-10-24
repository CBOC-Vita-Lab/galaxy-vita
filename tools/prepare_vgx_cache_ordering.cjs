// Instrument copied production translation units, never the shipping sources.
const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '..');
const out = process.argv[2];
if (!out) throw Error('Expected fixture output directory');
function once(text, needle, replacement) {
  if (text.split(needle).length !== 2) throw Error(`Hook anchor changed: ${needle}`);
  return text.replace(needle, replacement);
}
let guest = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_guestmem.cpp'), 'utf8');
const gens = '    for (uint32_t i = first; i <= last; ++i) b->gen[i].fetch_add(1, std::memory_order_relaxed);';
guest = once(guest, gens, '    ::CacheNotifyBoundary(1);\n'+gens+'\n    ::CacheNotifyBoundary(2);');
const claim = '    LogRec& r = g_log[seq & (kLogSize - 1)];';
guest = once(guest, claim, '    ::CacheNotifyBoundary(4);\n'+claim);
const payload = guest.includes('    r.p = p;') ? '    r.p = p;' : '        r.p.store(p, std::memory_order_relaxed);';
guest = once(guest, payload, '    ::CacheNotifyBoundary(3);\n'+payload);
guest = 'extern void CacheNotifyBoundary(int);\n'+guest;
let dl = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_dlcache.cpp'), 'utf8');
const build = '    const bool pure = Build(*e, list, nbytes);';
dl = once(dl, build, build+'\n    ::CacheDecodedBoundary();');
for (const [needle, stage] of [
  ['    e.hash = XXH3_64bits(list, nbytes);', 1],
  ['    size_t vertexBytes = 0;', 2],
  ['    uint64_t vertexBytes = 0;', 2],
  ['    for (int a = 9; a <= 20; ++a)\n        if (e.idxMask & (1u << a)) {\n            const int i = a - 9;\n            e.hash =', 3],
  ['    e.verts.resize(static_cast<size_t>(vertexBytes));', 4],
]) {
  if (dl.includes(needle)) dl = once(dl, needle, `    ::CacheBuildBoundary(${stage});\n`+needle);
}
for (let stage = 1; stage <= 4; ++stage) {
  if (dl.split(`::CacheBuildBoundary(${stage});`).length !== 2)
    throw Error('Missing or duplicate build hook '+stage);
}
if (dl.includes('    uint64_t vertexBytes = 0;'))
  dl = once(dl, '    uint64_t vertexBytes = 0;', '    uint64_t vertexBytes = ::CacheStartingVertexBytes();');
else if (dl.includes('    size_t vertexBytes = 0;'))
  dl = once(dl, '    size_t vertexBytes = 0;', '    size_t vertexBytes = static_cast<size_t>(::CacheStartingVertexBytes());');
const calls = dl.split('CurrentPlan(').length - 1;
if (calls < 1 || calls > 2) throw Error('Plan lookup anchors changed');
dl = dl.replaceAll('CurrentPlan(', '::CacheCurrentPlan(');
dl = '#include "vgx_state.h"\nextern void CacheDecodedBoundary();\nextern void CacheBuildBoundary(int);\nextern uint64_t CacheStartingVertexBytes();\nextern const vgx::Plan& CacheCurrentPlan(int);\n'+dl;
let texture = fs.readFileSync(path.join(root, 'platform/vgx/src/vgx_texcache.cpp'), 'utf8');
const firstHash = '        e.contentHash = XXH3_64bits(o.data, size);';
const firstGen = '        e.generation = Generation(o.data, size);';
const first = texture.indexOf(firstHash) < texture.indexOf(firstGen) ? firstHash : firstGen;
texture = 'extern void CacheTextureSnapshotBoundary();\n'+once(texture, first, first+'\n        ::CacheTextureSnapshotBoundary();');
fs.writeFileSync(path.join(out, 'guest.cpp'), guest);
fs.writeFileSync(path.join(out, 'dl.cpp'), dl);
fs.writeFileSync(path.join(out, 'texture.cpp'), texture);
