#!/usr/bin/env node
'use strict';
const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const root = path.resolve(__dirname, '..');
const output = process.argv[2];
if (!output) throw new Error('usage: prepare_vgx_material_view_inverse_fixture.cjs output');
const blockPath = 'decomp/src/JSystem/J3DGraphBase/J3DMatBlock.cpp';
const read = p => fs.readFileSync(path.join(root, p), 'utf8');
function extract(source, signature) {
  const start = source.indexOf(signature);
  if (start < 0 || source.indexOf(signature, start + signature.length) >= 0) throw new Error(`ambiguous signature: ${signature}`);
  let i = source.indexOf('{', start), depth = 0, mode = '';
  for (; i < source.length; ++i) {
    const c = source[i], n = source[i + 1];
    if (mode === '//') { if (c === '\n') mode = ''; continue; }
    if (mode === '/*') { if (c === '*' && n === '/') { mode = ''; ++i; } continue; }
    if (mode === '"' || mode === "'") { if (c === '\\') ++i; else if (c === mode) mode = ''; continue; }
    if (c === '/' && (n === '/' || n === '*')) { mode = c + n; ++i; continue; }
    if (c === '"' || c === "'") { mode = c; continue; }
    if (c === '{') ++depth;
    if (c === '}' && --depth === 0) return source.slice(start, i + 1);
  }
  throw new Error(`unclosed function: ${signature}`);
}
const method = 'void J3DTexGenBlockPatched::calcPostTexMtx(const Mtx modelMtx)';
const old = cp.execFileSync('git', ['show', `fc78e5e3a50ab2bc7fb52867796d66a8be278da2:${blockPath}`], {cwd: root, encoding: 'utf8'});
const mtx = read('platform/src/sdk/mtx.c');
const transform = read('decomp/src/JSystem/J3DGraphBase/J3DTransform.cpp');
const tevs = read('decomp/src/JSystem/J3DGraphBase/J3DTevs.cpp');
const concatStart = mtx.indexOf('#if defined(__ARM_NEON)');
const concatEnd = mtx.indexOf('void C_MTXConcatArray', concatStart);
if (concatStart < 0 || concatEnd < 0) throw new Error('matrix concat source boundary changed');
const parts = [
  '// Generated from real owning bodies; do not edit.',
  extract(mtx, 'void C_MTXCopy(const Mtx src, Mtx dst)'),
  extract(mtx, 'void PSMTXCopy(const Mtx src, Mtx dst)'),
  mtx.slice(concatStart, concatEnd),
  extract(mtx, 'u32 C_MTXInverse(const Mtx src, Mtx inv)'),
  ...['J3DGetTextureMtx', 'J3DGetTextureMtxOld'].map(name => extract(transform, `void ${name}(const J3DTextureSRTInfo& srt, const Vec& center, Mtx dst)`)),
  ...['J3DGetTextureMtxMaya', 'J3DGetTextureMtxMayaOld'].map(name => extract(transform, `void ${name}(const J3DTextureSRTInfo& srt, Mtx dst)`)),
  extract(transform, 'void J3DMtxProjConcat(Mtx mtx1, Mtx mtx2, Mtx dst)'),
  extract(tevs, 'void J3DTexMtx::calcPostTexMtx(const Mtx param_0)').replace('J3DTexMtx::calcPostTexMtx', 'J3DTexMtx::realCalcPost'),
  extract(old, method).replace('J3DTexGenBlockPatched::', 'BaselineBlock::'),
  extract(read(blockPath), method).replace('J3DTexGenBlockPatched::', 'OptimizedBlock::'),
];
fs.writeFileSync(output, parts.join('\n\n') + '\n');
