// Generate an opt-in local driver variant. Never edits the supplied driver/archive.
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const [driver, output] = process.argv.slice(2);
if (!driver || !output) throw new Error('driver and new output directory required');
const original = fs.readFileSync(path.join(driver, 'src/custom_shaders.c'), 'utf8');
const sha = text => crypto.createHash('sha256').update(text).digest('hex');
const pinned = '95219c9b84fe31f342db0a7d63759895a821842de79e1743f7b4da5547bfe48d';
const required = {
  'src/custom_shaders.c': pinned,
  'src/shared.h': '493991e4ff6c4ca0fe34dc0387f11b1505b57719de443350a9d37c1490b070be',
  'src/utils/shader_candidate.h': 'bb1b0f4bdb78350418f9ee908b340ace999f699fa5e9752a1acd50e9fde97163',
  'src/utils/shader_binary.h': '6adb86c41e9f4a2391c405bd85601f8055f9996d4e3a536720501eec5d7b8ec5',
  'src/utils/shader_cache_io.h': '0ab26f84aea858e49fc4c73eb7bf952b92542a112d381b5b0c26e84e111e9196',
  'include/vitaGL.h': 'a8f335530d7e25e1279780ab3a7f807919a4dc64f5a57370886cff47ebb68d8a',
  'compile.txt': '416f1175e87d9d46ecf9ab022fa989142fa2e05a8714d60473e5cd434316d446',
  'lib/libvitaGL.a': '16cd3e3282ad4aaad1eba266535f407333bc3f95a29411f90b7ed83e784471b5',
};
for (const [file, expected] of Object.entries(required))
  if (sha(fs.readFileSync(path.join(driver, file))) !== expected) throw new Error('Frozen driver input changed; review ' + file);
const compileRecord = fs.readFileSync(path.join(driver, 'compile.txt'), 'utf8').split('\n').find(line => line.includes(' -c custom_shaders.c '));
if (!compileRecord || !compileRecord.includes('-DHAVE_SHADER_CACHE') || /-DHAVE_GLSL_TEXTURE_SIZE(?:\s|=|$)/.test(compileRecord))
  throw new Error('Frozen shader serialization flags changed');
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
const marker = '\n\tif (load_shader_cache(sceIoOpen(fname, SCE_O_RDONLY, 0777), s, GL_FALSE)) {';
const compileStart = original.indexOf('void glCompileShader(GLuint handle) {');
const compileEnd = original.indexOf('void glDeleteShader(', compileStart);
const compile = original.slice(compileStart, compileEnd);
if (compileStart < 0 || compileEnd < 0 || !compile.includes(marker)) throw new Error('Reader marker is outside glCompileShader');
const lineAt = offset => original.slice(0, offset).split('\n').length;
const resumeFunction = `#line ${lineAt(compileStart)} "custom_shaders.c"\n`;
const resumeReader = `#line ${lineAt(original.indexOf(marker) + 1)} "custom_shaders.c"\n`;
const branch = `#ifdef GXV_SHADER_CACHE_READER
\tconst void *cached_bytes = NULL;
\tsize_t cached_size = 0;
\tif (glsl_sema_mode == VGL_MODE_GLOBAL && vgxvita_shaderCacheRead_v1(fname, &cached_bytes, &cached_size)) {
\t\tvoid *copy = vglMalloc(cached_size);
\t\tif (copy) {
\t\t\tvgl_fast_memcpy(copy, cached_bytes, cached_size);
\t\t\tGLboolean restored = unserialize_shader(copy, cached_size, s, GL_FALSE);
\t\t\tvgl_free(copy);
\t\t\tif (restored) {
\t\t\t\tvgl_free(s->source);
\t\t\t\ts->source = NULL;
\t\t\t\treturn;
\t\t\t}
\t\t}
\t}
#endif
`;
let patched = once(original, 'void glCompileShader(GLuint handle) {', declaration + resumeFunction + 'void glCompileShader(GLuint handle) {');
patched = once(patched, marker, '\n' + branch + resumeReader + marker.slice(1));
const passthrough = once(original, 'void glCompileShader(GLuint handle) {', declaration + resumeFunction + 'void glCompileShader(GLuint handle) {');
// The original reader experiment remains unchanged. Residency owns an additional
// native contract: all live names, including unlinked/utility names, consume slots.
let residency = once(patched, 'static program progs[MAX_CUSTOM_PROGRAMS];',
  'static program progs[MAX_CUSTOM_PROGRAMS];\nstatic unsigned program_slots_used;');
residency = once(residency, '\t\tprogs[i].status = PROG_INVALID;\n\t}\n}',
  '\t\tprogs[i].status = PROG_INVALID;\n\t}\n\tprogram_slots_used = 0;\n}');
residency = once(residency, 'GLuint glCreateProgram(void) {',
  'unsigned vglGetFreeProgramSlots(void) {\n    return MAX_CUSTOM_PROGRAMS - program_slots_used;\n}\n\nGLuint glCreateProgram(void) {');
residency = once(residency, '\t\t\tprogs[i].status = PROG_UNLINKED;',
  '\t\t\tprogs[i].status = PROG_UNLINKED;\n\t\t\t++program_slots_used;');
residency = once(residency, '\tp->status = PROG_INVALID;',
  '\tif (p->status) --program_slots_used;\n\tp->status = PROG_INVALID;');
for (const [name, source] of [['control', original], ['passthrough', passthrough], ['candidate', patched], ['residency', residency]]) {
  fs.mkdirSync(path.join(output, name), {recursive: true});
  fs.writeFileSync(path.join(output, name, 'custom_shaders.c'), source);
}
fs.writeFileSync(path.join(output, 'inputs.json'), JSON.stringify({original_sha256: pinned, candidate_sha256: sha(patched),
  residency_sha256: sha(residency), residency_required_symbol: 'vglGetFreeProgramSlots',
  residency_scope: 'RAM reader plus serialized native program occupancy at reset/create/delete; link preserves occupancy',
  conditional: 'GXV_SHADER_CACHE_READER', scope: 'GLOBAL glCompileShader only; hardened deserializer and original file/compiler fallback untouched',
  required_inputs: required, compile_record: compileRecord,
  bytes: 'exact serialized file image, copied into original allocator before deserialization'}, null, 2) + '\n');
console.log(JSON.stringify({original_sha256: pinned, candidate_sha256: sha(patched), supplied_archive_modified: false}));
