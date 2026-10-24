const fs=require('fs'),path=require('path'),crypto=require('crypto');
const [driver,out]=process.argv.slice(2);
if(!driver||!out)throw Error('frozen driver and output required');
const inputs={};
function read(file){const p=path.join(driver,file),s=fs.readFileSync(p,'utf8');inputs[p]=crypto.createHash('sha256').update(s).digest('hex');return s;}
function body(source,start,end){const a=source.indexOf(start),b=source.indexOf(end,a+start.length);if(a<0||b<0||source.indexOf(start,a+1)>=0)throw Error('Frozen linear fixture marker changed: '+start);return source.slice(a,b);}
const gpu=read('src/utils/gpu_utils.c'),header=read('src/utils/gpu_utils.h'),callbacks=read('src/texture_callbacks.c'),shared=read('src/shared.h');
const compile=read('compile.txt').split('\n').find(line=>line.includes(' -c utils/gpu_utils.c '));
if(!compile)throw Error('Missing allocator compile record');
for(const flag of ['TEXTURES_SPEEDHACK','HAVE_TEX_CACHE'])
 if(new RegExp('(?:^|\\s)-D'+flag+'(?:\\s|=|$)').test(compile)||new RegExp('^\\s*#\\s*define\\s+'+flag+'\\b','m').test(gpu+'\n'+shared))throw Error('Linear fixture build-flag parity changed: '+flag);
const source=body(header,'static inline __attribute__((always_inline)) int tex_format_to_bytespp(', 'static inline __attribute__((always_inline)) SceGxmTransferFormat tex_format_to_transfer(')+
 body(gpu,'void gpu_alloc_texture(', 'void gpu_alloc_paletted_texture(')+
 body(callbacks,'uint32_t read_la88(', '// Write callback for 32bpp unsigned RGBA format')+
 body(callbacks,'void write_ra88(', '// Write callback for 8bpp unsigned R format');
fs.writeFileSync(path.join(out,'frozen-linear.c'),'#include "vgx_frozen_linear.h"\n'+source);
fs.writeFileSync(path.join(out,'inputs.json'),JSON.stringify({kind:'unmodified-function-extraction',inputs},null,2)+'\n');
console.log(JSON.stringify({driver_functions:4,inputs}));
