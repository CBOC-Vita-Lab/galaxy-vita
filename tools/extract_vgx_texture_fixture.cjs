const fs=require('fs'),path=require('path'),crypto=require('crypto');
const [driver,out,checkout]=process.argv.slice(2);
if(!driver||!out||!checkout)throw Error('driver, output and checkout required');
const inputs={};
function read(file,base=driver){const p=path.join(base,file),s=fs.readFileSync(p,'utf8');inputs[p]=crypto.createHash('sha256').update(s).digest('hex');return s;}
function body(source,start,end){const a=source.indexOf(start),b=source.indexOf(end,a+start.length);if(a<0||b<0||source.indexOf(start,a+1)>=0)throw Error('Frozen fixture marker changed: '+start);return source.slice(a,b);}
const gpu=read('src/utils/gpu_utils.c'),shared=read('src/shared.h');
const compile=read('compile.txt').split('\n').find(line=>line.includes(' -c utils/gpu_utils.c '));
if(!compile)throw Error('Missing frozen allocator compile record');
for(const flag of ['TEXTURE_UPLOADS_SPEEDHACK','HAVE_TEX_CACHE','TEXTURES_SPEEDHACK']) {
 if(new RegExp('(?:^|\\s)-D'+flag+'(?:\\s|=|$)').test(compile)||new RegExp('^\\s*#\\s*define\\s+'+flag+'\\b','m').test(gpu+'\n'+shared))
  throw Error('Allocator fixture requires build-flag parity audit: '+flag);
}
const helpers=body(gpu,'static inline __attribute__((always_inline)) int gpu_get_compressed_mip_size(', 'void gpu_alloc_compressed_cube_texture(');
const alloc=body(gpu,'void gpu_alloc_compressed_texture(', 'void gpu_alloc_mipmaps(');
const po2=body(shared,'static inline __attribute__((always_inline)) uint32_t nearest_po2(', 'static inline __attribute__((always_inline)) uint32_t get_color_from_texture(');
fs.writeFileSync(path.join(out,'frozen-texture.c'),'#include "vgx_frozen_texture.h"\n'+po2+helpers+alloc+`
int test_gpu_mip_size(int l,int w,int h){return gpu_get_compressed_mip_size(l,w,h,SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR);}
int test_gpu_mip_offset(int l,int w,int h){return gpu_get_compressed_mip_offset(l,w,h,SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR);}
int test_gpu_chain_size(int l,int w,int h){return gpu_get_compressed_mipchain_size(l,w,h,SCE_GXM_TEXTURE_FORMAT_UBC1_ABGR);}
`);
const host=read('platform/vgx/vita/vgx_host_galaxy_vita.cpp',checkout);
fs.writeFileSync(path.join(out,'frozen-upload-host.inc'),body(host,'static bool UploadSync() {','extern "C" void vgxvita_glGenerateMipmap('));
fs.writeFileSync(path.join(out,'inputs.json'),JSON.stringify({kind:'unmodified-function-extraction',inputs},null,2)+'\n');
console.log(JSON.stringify({driver_functions:5,host_functions:2,inputs}));
