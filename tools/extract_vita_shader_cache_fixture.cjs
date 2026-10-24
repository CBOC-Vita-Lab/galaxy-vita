const fs = require('fs'), path = require('path'), crypto = require('crypto');
const [driver, out, readerVariant] = process.argv.slice(2);
if (!driver || !out) throw new Error('driver and output required');
const inputs = {};
function read(file) {
  const full = path.join(driver, file), source = fs.readFileSync(full, 'utf8');
  inputs[file] = crypto.createHash('sha256').update(source).digest('hex');
  return source;
}
function body(source, start, end) {
  const a = source.indexOf(start), b = source.indexOf(end, a + start.length);
  if (a < 0 || b < 0 || source.indexOf(start, a + 1) >= 0) throw new Error('Frozen fixture marker changed: ' + start);
  return source.slice(a, b);
}
const source = read('src/custom_shaders.c'), shared = read('src/shared.h');
const record = read('compile.txt').split('\n').find(line => line.includes(' -c custom_shaders.c '));
if (!record || !record.includes('-DHAVE_SHADER_CACHE') || !record.includes('-DHAVE_SHARK_LOG') || !record.includes('-DLOG_ERRORS'))
  throw new Error('Frozen shader-cache build flags need review');
if (/-DHAVE_GLSL_TEXTURE_SIZE(?:\s|=|$)/.test(record)) throw new Error('Fixture does not cover sized-sampler serialization');
const types = body(shared, '// Matrix uniform struct', '#ifdef HAVE_FFP_SHADER_SUPPORT');
fs.writeFileSync(path.join(out, 'frozen-shader-types.h'), types);
const serial = body(source, 'static inline __attribute__((always_inline)) size_t serialized_shader_size(', '#ifdef HAVE_SHADER_CACHE\n#define vgl_compile_shader');
const compile = body(source, '#ifdef HAVE_SHADER_CACHE\n#define vgl_compile_shader', 'void reset_custom_shaders(');
const shaderSource = body(source, 'void glShaderSource(', 'void glShaderBinary(');
const variant = readerVariant ? fs.readFileSync(readerVariant, 'utf8') : source;
const shaderCompile = (readerVariant ? body(variant, '#ifdef GXV_SHADER_CACHE_READER\n#include "vgx_shader_reader.h"', 'void glCompileShader(') : '') +
  body(variant, 'void glCompileShader(', 'void glDeleteShader(');
if (readerVariant) inputs.reader_variant = crypto.createHash('sha256').update(variant).digest('hex');
const prefix = `// Mechanically extracted unmodified vitaGL functions; generated locally, never a driver replacement.
#include "vita_shader_cache_fixture.h"
#define XXH_STATIC_LINKING_ONLY
#define XXH_IMPLEMENTATION
#define XXH_NAMESPACE VITAGL_
#include "utils/xxhash_utils.h"
`;
fs.writeFileSync(path.join(out, 'frozen-shader-cache.c'), prefix + serial + compile + shaderSource + shaderCompile);
const opens = [...source.matchAll(/^.*sceIoOpen\(.*$/gm)].map(m => m[0].trim());
const inventory = {root_references: [], writes: []};
function scan(directory) {
  for (const entry of fs.readdirSync(path.join(driver, directory), {withFileTypes: true})) {
    const file = path.posix.join(directory, entry.name);
    if (entry.isDirectory()) { scan(file); continue; }
    if (!/\.[ch]$/.test(file)) continue;
    const text = fs.readFileSync(path.join(driver, file), 'utf8');
    text.split('\n').forEach((line, i) => {
      if (line.includes('vgl_shader_cache_path')) inventory.root_references.push({file, line: i + 1, text: line.trim()});
      if (line.includes('sceIoOpen(') && line.includes('SCE_O_CREAT')) inventory.writes.push({file, line: i + 1, text: line.trim()});
    });
  }
}
scan('src');
function requireSource(good, why) { if (!good) throw new Error('Frozen cache inventory needs review: ' + why); }
requireSource(JSON.stringify([...new Set(inventory.root_references.map(site => site.file))].sort()) ===
  JSON.stringify(['src/custom_shaders.c', 'src/shared.h', 'src/vgl.c']), 'new root consumer');
const expectedWrites = [
  ['src/custom_shaders.c', 'SceUID f = sceIoOpen(cache_fname, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);'],
  ['src/ffp.c', 'f = sceIoOpen(fname, SCE_O_WRONLY | SCE_O_TRUNC | SCE_O_CREAT, 0777);'],
  ['src/ffp.c', 'f = sceIoOpen(fname, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);'],
  ['src/utils/gpu_utils.c', 'SceUID f = sceIoOpen(fname, SCE_O_CREAT | SCE_O_TRUNC | SCE_O_WRONLY, 0777);'],
];
requireSource(JSON.stringify(inventory.writes.map(site => [site.file, site.text]).sort()) === JSON.stringify(expectedWrites.sort()), 'changed write sites or SDK flags/modes');
const formats = inventory.root_references.filter(site => site.file === 'src/custom_shaders.c' && site.text.includes('sprintf('));
requireSource(formats.length === 3 && formats[0].text.includes('"%s/%s/%02X/%llX.gxp"') &&
  formats[1].text.includes('"%s/v/%02X/%llX.gxp"') && formats[2].text.includes('"%s/f/%02X/%llX.gxp"'), 'changed GLOBAL/POSTPONED private path grammar');
requireSource(source.includes('fname = vert_fname;') && source.includes('fname = frag_fname;') &&
  source.match(/vgl_compile_shader\(p->[vf]shader, GL_TRUE\);/g)?.length === 2, 'postponed compiler must reuse the inventoried private writer');
const ffp = read('src/ffp.c'), gpu = read('src/utils/gpu_utils.c'), init = read('src/vgl.c');
requireSource(ffp.includes('"ux0:data/shader_cache/v%d/v/') && ffp.includes('"ux0:data/shader_cache/v%d/f/') &&
  !ffp.includes('vgl_shader_cache_path'), 'FFP writes must remain in separate fixed-root non-bucket cache');
requireSource(init.includes('"ux0:data/shader_cache/v%d/v"') && init.includes('"ux0:data/shader_cache/v%d/f"'), 'FFP parent initialization');
requireSource(gpu.includes('"%s/%c%c/%s.raw", vgl_file_cache_path') && !record.includes('-DHAVE_TEX_CACHE'), 'separate texture cache is disabled in pinned build');
const host = fs.readFileSync(path.join(__dirname, '../platform/vgx/vita/vgx_host_galaxy_vita.cpp'), 'utf8');
requireSource(host.match(/vglSetSemanticBindingMode\(/g)?.length === 1 && host.includes('vglSetSemanticBindingMode(VGL_MODE_GLOBAL);'), 'Galaxy actual semantic mode');
inventory.product_semantic_mode = 'VGL_MODE_GLOBAL';
inventory.postponed_mode = 'same bucket grammar and sole compile_shader write; not selected by Galaxy';
inventory.other_writers = 'FFP uses separate fixed-root non-bucket paths with parents made by vitaGL; HAVE_TEX_CACHE is disabled';
fs.writeFileSync(path.join(out, 'shader-inputs.json'), JSON.stringify({kind: 'unmodified-function-extraction', functions: 6, inputs, build_record: record, custom_shader_open_inventory: opens, cache_inventory: inventory}, null, 2) + '\n');
console.log(JSON.stringify({functions: 6, inputs, shader_open_sites: opens.length, inventoried_write_sites: inventory.writes.length, product_semantic_mode: inventory.product_semantic_mode}));
