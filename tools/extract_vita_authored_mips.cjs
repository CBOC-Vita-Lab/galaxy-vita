// Extract pinned driver functions without editing their bodies. No game data.
const fs = require('fs'), path = require('path'), crypto = require('crypto');
const [src, out, root] = process.argv.slice(2);
if (!src || !out || !root) throw Error('usage: driver-source output checkout');
const inputs = {};
function read(file) {
  const text = fs.readFileSync(path.join(src, file), 'utf8');
  inputs[file] = crypto.createHash('sha256').update(text).digest('hex');
  return text;
}
function section(s, a, b) {
  const start = s.indexOf(a), end = s.indexOf(b, start + a.length);
  if (start < 0 || end < 0 || s.indexOf(a, start + 1) >= 0) throw Error('driver marker changed: ' + a);
  return s.slice(start, end);
}
const shared = read('shared.h'), gpu = read('utils/gpu_utils.c'), gpuHeader = read('utils/gpu_utils.h');
const textures = read('textures.c'), gxm = read('utils/gxm_utils.h'), callbacks = read('texture_callbacks.c');
for (const flag of ['HAVE_TEX_CACHE', 'TEXTURES_SPEEDHACK', 'HAVE_UNPURE_TEXTURES']) {
  if (new RegExp('^\\s*#\\s*define\\s+' + flag + '\\b', 'm').test(shared + textures + gpu))
    throw Error('unsupported driver variant ' + flag);
}
fs.writeFileSync(path.join(out, 'authored-driver-types.h'),
  section(shared, '// Texture object struct', '// Memory file cache settings'));
const parts = [
  '#include "vita_authored_mips_fixture.h"\n',
  section(gxm, 'typedef struct {\n\tuint32_t control_words[4];', 'static inline __attribute__((always_inline)) uint32_t *vglProgramGetParameterBase('),
  section(shared, 'static inline __attribute__((always_inline)) uint32_t nearest_po2(', 'static inline __attribute__((always_inline)) uint32_t get_color_from_texture('),
  section(gpuHeader, 'static inline __attribute__((always_inline)) int tex_format_to_bytespp(', '// Alloc a texture'),
  callbacks.slice(callbacks.indexOf('#define convert_u16_to_u32_cspace')),
  section(gpu, 'void gpu_alloc_texture(', 'void gpu_alloc_paletted_texture('),
  section(gpu, 'void gpu_alloc_mipmaps(', 'void gpu_free_palette('),
  section(textures, 'static inline __attribute__((always_inline)) void _glTexImage2D_FlatIMPL(', 'void _glCompressedTexImage2D('),
  '\nvoid fixture_image(int l,int w,int h,const void* p) { _glTexImage2D_FlatIMPL(&fixture_texture,l,GL_RGBA,w,h,GL_RGBA,GL_UNSIGNED_BYTE,p); }\n',
  'void fixture_subimage(int l,int w,int h,const void* p) { _glTexSubImage2D(&fixture_texture,GL_TEXTURE_2D,l,0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,p); }\n',
  'void fixture_generate(void) { gpu_alloc_mipmaps(-1,&fixture_texture); }\n',
];
fs.writeFileSync(path.join(out, 'authored-driver.c'), parts.join('\n'));
const host = fs.readFileSync(path.join(root, 'platform/vgx/vita/vgx_host_galaxy_vita.cpp'), 'utf8');
const marker = 'extern "C" void vgxvita_uploadAuthoredI4Mips(';
fs.writeFileSync(path.join(out, 'authored-host.inc'), host.includes(marker)
  ? section(host, marker, '// ---- vitaGL\'s cache folders') : '');
fs.writeFileSync(path.join(out, 'authored-inputs.json'), JSON.stringify({kind: 'unmodified-driver-function-extraction', inputs}, null, 2) + '\n');
console.log(JSON.stringify({kind: 'unmodified-driver-function-extraction', inputs}));
