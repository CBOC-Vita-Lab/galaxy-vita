// Extracts the real text of vitaGL's per-picture data pool and of the buffer functions that use it when the library is built with
// HAVE_SCRATCH_MEMORY, from a patched vitaGL tree (the nhustdx set: patches 0001-0012; ALIGNED_POOL is applied here exactly as
// build-vitagl.sh applies it to its copy), into a host fixture include for tests/vitagl_scratch_pool_test.cpp:
//   vgl.c      the pool arrays, vgl_reserve_data_pool, the pool set-up of vglInit, vglSetupScratchMemory
//   buffers.c  glNamedBufferData, glDeleteBuffers, glNamedBufferSubData
//   gxm.c      the pool rotation of vglSwapBuffers (frame count up, pool index, pool pointer reset)
//   shared.h   the vbo struct
// It also audits the tree: only buffers.c may reserve from the pool on behalf of a buffer object, and the library must not be
// built with CIRCULAR_POOL_SPEEDHACK for this test (the test is about the per-display-buffer pools).
const fs = require('node:fs');
const path = require('node:path');
const [sourceRoot, output] = process.argv.slice(2);
if (!sourceRoot || !output) throw new Error('usage: preparer source-root output');
const dir = fs.existsSync(path.join(sourceRoot, 'gxm.c')) ? sourceRoot : path.join(sourceRoot, 'source');
const read = (f) => fs.readFileSync(path.join(dir, f), 'utf8');
const vgl = read('vgl.c'), buffers = read('buffers.c'), gxm = read('gxm.c'), shared = read('shared.h');

// The build of the nhustdx set has these defined; the test compiles the same text with the same definitions.
const DEFINED = new Set(['HAVE_SCRATCH_MEMORY', 'LOG_ERRORS']);
function condition(expr) {
    const js = expr.replace(/defined\(\s*(\w+)\s*\)/g, (_, n) => (DEFINED.has(n) ? '1' : '0')).replace(/\bdefined\s+(\w+)/g, (_, n) => (DEFINED.has(n) ? '1' : '0'));
    if (/[A-Za-z_]/.test(js)) throw new Error('cannot evaluate #if ' + expr);
    return !!Function('return (' + js + ')')();
}
// The text with the lines of inactive conditional branches blanked (same length, same offsets), so braces are counted once.
function mask(text) {
    const lines = text.split('\n'), stack = []; // each: {outer, taken, active}
    let active = true;
    return lines.map((line) => {
        const d = /^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$/.exec(line);
        if (d) {
            const [, kind, rest] = d;
            if (kind === 'if' || kind === 'ifdef' || kind === 'ifndef') {
                const v = kind === 'if' ? condition(rest.replace(/\/\/.*$/, '')) : (DEFINED.has(rest.trim().split(/\s/)[0]) === (kind === 'ifdef'));
                stack.push({ outer: active, taken: v });
                active = active && v;
            } else if (kind === 'elif') {
                const top = stack[stack.length - 1];
                const v = !top.taken && condition(rest);
                top.taken = top.taken || v;
                active = top.outer && v;
            } else if (kind === 'else') {
                const top = stack[stack.length - 1];
                active = top.outer && !top.taken;
                top.taken = true;
            } else {
                active = stack.pop().outer;
            }
            return ' '.repeat(line.length);
        }
        return active ? line : ' '.repeat(line.length);
    }).join('\n');
}
function matchBrace(text, open) {
    const m = mask(text);
    let depth = 1, end = open + 1;
    for (; end < m.length && depth; ++end) {
        if (m[end] === '{') ++depth;
        else if (m[end] === '}') --depth;
    }
    if (depth) throw new Error('unbalanced braces');
    return end;
}
function functionText(text, headerRegex, label) {
    const match = new RegExp(headerRegex).exec(text);
    if (!match) throw new Error('patched source lacks ' + label);
    return text.slice(match.index, matchBrace(text, match.index + match[0].length - 1));
}
function between(text, from, to, label) {
    const a = text.indexOf(from);
    if (a < 0) throw new Error('patched source lacks the start of ' + label);
    const b = text.indexOf(to, a);
    if (b < 0) throw new Error('patched source lacks the end of ' + label);
    return text.slice(a, b);
}
function fail(why) { throw new Error('audit: ' + why); }

// ---- audit: who takes memory from the pool
for (const f of fs.readdirSync(dir).filter((n) => n.endsWith('.c'))) {
    const text = read(f);
    const count = (text.match(/\bvgl_reserve_data_pool\(/g) || []).length;
    const allowed = { 'buffers.c': 2, 'vgl.c': 1 }[f] || 0; // buffers.c: glNamedBufferData, glNamedBufferSubData; vgl.c: the definition
    if (count !== allowed) fail('vgl_reserve_data_pool is used ' + count + ' times in ' + f + ', expected ' + allowed);
}
if (!/static inline __attribute__\(\(always_inline\)\) void \*gpu_alloc_mapped_temp/.test(read('utils/gpu_utils.h'))) fail('gpu_alloc_mapped_temp moved');
if (/\bvgl_reserve_data_pool\(/.test(read('utils/gpu_utils.h')) === false) fail('gpu_alloc_mapped_temp no longer draws from the pool');

// ---- vgl.c: the pool
const decls = between(vgl, '#define CIRCULAR_POOL_SIZE_DEF', 'uint8_t *vgl_reserve_data_pool(uint32_t size) {', 'the pool declarations');
let reserve = functionText(vgl, '(?:^|\\n)uint8_t \\*vgl_reserve_data_pool\\(uint32_t size\\) \\{', 'vgl_reserve_data_pool').replace(/^\n/, '');
// ALIGNED_POOL, as build-vitagl.sh applies it to its copy.
reserve = reserve.replace(/^uint8_t \*vgl_reserve_data_pool\(uint32_t size\) \{/, '$&\n\tsize = (size + 15u) & ~15u; /* ALIGNED_POOL */');
if (!reserve.includes('ALIGNED_POOL */')) throw new Error('ALIGNED_POOL did not apply');
const init = between(vgl, '#ifndef DISABLE_CIRCULAR_POOL\n#ifndef CIRCULAR_POOL_SPEEDHACK\n\tfor (int i = 0; i < gxm_display_buffer_count; i++) {',
                     '\n\t// Init constant index buffers', 'the pool set-up in vglInit');
const setup = functionText(vgl, 'void vglSetupScratchMemory\\(GLboolean scratch_for_dynamic, GLboolean scratch_for_stream\\) \\{', 'vglSetupScratchMemory');

// ---- buffers.c
const fns = ['void glDeleteBuffers\\(GLsizei n, const GLuint \\*gl_buffers\\) \\{', 'inline void glNamedBufferData\\(GLuint buffer, GLsizei size, const void \\*data, GLenum usage\\) \\{',
             'inline void glNamedBufferSubData\\(GLuint buffer, GLintptr offset, GLsizeiptr size, const void \\*data\\) \\{']
    .map((h) => functionText(buffers, h, h));

// ---- gxm.c: the rotation at a swap
const rotation = between(gxm, '\tvgl_framecount++;\n', '\n\t// Marking uniform values as dirty at each frame end', 'the pool rotation in vglSwapBuffers');
if (!/vgl_circular_idx = vgl_framecount % gxm_display_buffer_count;/.test(rotation) || !/circular_data_pool_ptr\[vgl_circular_idx\] = circular_data_pool\[vgl_circular_idx\];/.test(rotation))
    fail('the pool rotation of vglSwapBuffers changed');

// ---- shared.h
const vboStruct = between(shared, '// VBO struct\ntypedef struct {', '} vbo;', 'the vbo struct') + '} vbo;\n';
const objNotUsed = /#define OBJ_NOT_USED [^\n]*/.exec(shared), purge = /#define FRAME_PURGE_FREQ [^\n]*/.exec(shared);
if (!objNotUsed || !purge) throw new Error('shared.h lacks OBJ_NOT_USED / FRAME_PURGE_FREQ');

// The text is C; the host test is C++, which wants explicit casts from void * in two places. Nothing else is changed.
function cxx(text) {
    const a = text.replace('circular_data_pool[i] = gpu_alloc_mapped_for_cpu(', 'circular_data_pool[i] = (uint8_t *)gpu_alloc_mapped_for_cpu(');
    const b = a.replace(': gpu_buf->alloc_func(gpu_buf->size);', ': (uint8_t *)gpu_buf->alloc_func(gpu_buf->size);');
    return b;
}
const out = cxx([
    '// generated by tools/prepare_vitagl_scratch_fixture.cjs from the patched vitaGL source; do not edit',
    objNotUsed[0], purge[0],
    '#define HAVE_SCRATCH_MEMORY 1',
    vboStruct,
    '#ifndef DISABLE_CIRCULAR_POOL', decls, reserve, '#endif',
    'static void fixture_pool_init(void) {\n' + init.replace(/^#ifndef DISABLE_CIRCULAR_POOL\n/, '').replace(/#endif\s*$/, '') + '\n}',
    setup,
    ...fns,
    'static void fixture_swap_rotation(void) {\n' + rotation + '\n}',
].join('\n') + '\n');
if (!out.includes('(uint8_t *)gpu_alloc_mapped_for_cpu(') || !out.includes('(uint8_t *)gpu_buf->alloc_func(')) throw new Error('the C++ casts did not apply');
fs.writeFileSync(output, out);
