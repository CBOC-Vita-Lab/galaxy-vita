// Generate an opt-in local driver variant. Never edits the supplied driver/archive.
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const [driver, output] = process.argv.slice(2);
if (!driver || !output) throw new Error('driver and new output directory required');
const original = fs.readFileSync(path.join(driver, 'src/custom_shaders.c'), 'utf8');
const sha = text => crypto.createHash('sha256').update(text).digest('hex');
const pinned = '38d45a9467f12ba4e56f72106daea81d6ad48076215684331b829df3e9b718ec';
if (sha(original) !== pinned) throw new Error('Frozen custom_shaders.c changed; review before patching');
if (fs.existsSync(output)) throw new Error('Refuse to overwrite driver experiment');
function once(text, before, after) {
  if (text.indexOf(before) < 0 || text.indexOf(before) !== text.lastIndexOf(before)) throw new Error('Patch context is not unique');
  return text.replace(before, after);
}
const declaration = `#ifdef GXV_SHADER_CACHE_READER
#include "vgx_shader_reader.h"
void port_vgx_shader_cache_reader_driver_v1(void) {}
#endif

`;
const marker = '\n\tSceUID f = sceIoOpen(fname, SCE_O_RDONLY, 0777);';
const compileStart = original.indexOf('void glCompileShader(GLuint handle) {');
const compileEnd = original.indexOf('void glDeleteShader(', compileStart);
const compile = original.slice(compileStart, compileEnd);
if (compileStart < 0 || compileEnd < 0 || !compile.includes(marker)) throw new Error('Reader marker is outside glCompileShader');
const lineAt = offset => original.slice(0, offset).split('\n').length;
const resumeFunction = `#line ${lineAt(compileStart)} "custom_shaders.c"\n`;
const resumeReader = `#line ${lineAt(original.indexOf(marker) + 1)} "custom_shaders.c"\n`;
const cleanupStart = compile.indexOf('\t\tvgl_free(s->source);');
const cleanupEnd = compile.indexOf('\t\tsize_t sz =', cleanupStart);
const tailStart = compile.indexOf('\t\tunserialize_shader(buf, sz, s, GL_FALSE);');
const tailEnd = compile.indexOf('\t\treturn;\n', tailStart) + '\t\treturn;\n'.length;
if (cleanupStart < 0 || cleanupEnd < 0 || tailStart < 0 || tailEnd < tailStart) throw new Error('Original hit-tail structure changed');
const cleanup = compile.slice(cleanupStart, cleanupEnd);
const tail = compile.slice(tailStart, tailEnd);
const indent = text => text.replace(/^\t\t/gm, '\t\t\t');
const branch = `#ifdef GXV_SHADER_CACHE_READER
\tconst void *cached_bytes = NULL;
\tsize_t cached_size = 0;
\tif (glsl_sema_mode == VGL_MODE_GLOBAL && vgxvita_shaderCacheRead_v1(fname, &cached_bytes, &cached_size)) {
\t\tvoid *copy = vglMalloc(cached_size);
\t\tif (copy) {
\t\t\tvgl_fast_memcpy(copy, cached_bytes, cached_size);
${indent(cleanup)}${indent(tail.replace(/\bbuf\b/g, 'copy').replace(/\bsz\b/g, 'cached_size'))}\t\t}
\t}
#endif
`;
let patched = once(original, 'void glCompileShader(GLuint handle) {', declaration + resumeFunction + 'void glCompileShader(GLuint handle) {');
patched = once(patched, marker, '\n' + branch + resumeReader + marker.slice(1));
const passthrough = once(original, 'void glCompileShader(GLuint handle) {', declaration + resumeFunction + 'void glCompileShader(GLuint handle) {');
for (const [name, source] of [['control', original], ['passthrough', passthrough], ['candidate', patched]]) {
  fs.mkdirSync(path.join(output, name), {recursive: true});
  fs.writeFileSync(path.join(output, name, 'custom_shaders.c'), source);
}
fs.writeFileSync(path.join(output, 'inputs.json'), JSON.stringify({original_sha256: pinned, candidate_sha256: sha(patched),
  conditional: 'GXV_SHADER_CACHE_READER', scope: 'GLOBAL glCompileShader only; original file/compiler reader untouched',
  original_cleanup_sha256: sha(cleanup), original_tail_sha256: sha(tail),
  bytes: 'exact serialized file image, copied into original allocator before deserialization'}, null, 2) + '\n');
console.log(JSON.stringify({original_sha256: pinned, candidate_sha256: sha(patched), supplied_archive_modified: false}));
