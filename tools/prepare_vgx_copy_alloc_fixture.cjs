const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const [root, out] = process.argv.slice(2);
if (!root || !out) throw Error('checkout and output directory required');
const file = path.join(root, 'platform/vgx/src/vgx_backend_gl.cpp');
const source = fs.readFileSync(file, 'utf8');
function body(start, end) {
  const a = source.indexOf(start), b = source.indexOf(end, a + start.length);
  if (a < 0 || b < 0 || source.indexOf(start, a + 1) >= 0) throw Error('Fixture marker changed: ' + start);
  return source.slice(a, b);
}
const pieces = [
  body('struct Tex {', '\nint g_fixedW'),
  body('struct PoolFbo {', '\nnamespace PoolHeld'),
];
fs.writeFileSync(path.join(out, 'copy-alloc-types.inc'), pieces.join('\n'));
fs.writeFileSync(path.join(out, 'copy-alloc-production.inc'), [
  body('int g_simFailAt = 0, g_simChecks = 0;', '\nvoid RetireTexture('),
  body('Tex& TexSlotFor(', '// DXT5 -> RGBA8'),
  body('int AcquireFbo(', '// CMD_PASS_BEGIN:'),
  body('void EndPass() {', '\nbool VerifyRead('),
].join('\n'));
fs.writeFileSync(path.join(out, 'inputs.json'), JSON.stringify({
  kind: 'unmodified-production-function-extraction',
  file, sha256: crypto.createHash('sha256').update(source).digest('hex'),
}, null, 2) + '\n');
