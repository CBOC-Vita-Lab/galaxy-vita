// Actual native occupancy transitions plus full ARM reader/archive link guard.
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {spawnSync} from 'node:child_process';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'..');
const [driverArg,sdkArg,outArg]=process.argv.slice(2);
if(!driverArg||!sdkArg||!outArg)throw Error('usage: node test_vita_program_capacity.mjs DRIVER SDK NEW_OUTPUT');
const driver=path.resolve(driverArg),sdk=path.resolve(sdkArg),out=path.resolve(outArg);
if(fs.existsSync(out))throw Error('output already exists');
fs.mkdirSync(out,{recursive:true});
const generated=path.join(out,'driver');
const prepare=spawnSync(process.execPath,[path.join(root,'tools/prepare_vita_shader_reader.cjs'),driver,generated],{encoding:'utf8'});
if(prepare.status!==0)throw Error(prepare.stderr);
const src=fs.readFileSync(path.join(generated,'residency/custom_shaders.c'),'utf8');
function section(start,end){const a=src.indexOf(start),b=src.indexOf(end,a);if(a<0||b<a)throw Error('missing native source section');return src.slice(a,b);}
const fields=section('// Program struct holding vertex/fragment shader info','\n#if defined(HAVE_VERTEX_LAYOUT_CACHE) && !defined(STRICT_DRAW_COMPLIANCE) && !defined(DRAW_SPEEDHACK)\nenum');
const pre=`#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <assert.h>\n#define MAX_CUSTOM_PROGRAMS 1024\n#define MAX_CUSTOM_SHADERS 2048\n#define VERTEX_ATTRIBS_NUM 16\n#define TEXTURE_IMAGE_UNITS_NUM 16\n#define THREAD_SAFE()\n#define vgl_log(...) ((void)0)\n#define sceClibMemset memset\ntypedef unsigned GLuint; typedef unsigned char GLboolean;\ntypedef struct {int ref_counter,dirty;} shader; typedef struct {int unused;} uniform; typedef struct {void *chain;} ubo;\ntypedef struct {unsigned regIndex;} SceGxmVertexAttribute; typedef struct {int unused;} SceGxmVertexStream;\ntypedef void SceGxmVertexProgram; typedef void SceGxmFragmentProgram; typedef void attr_mapping; typedef unsigned blend_config;\nenum {PROG_INVALID,PROG_UNLINKED,PROG_LINKED};\nstatic unsigned finishes,frees,releases; static void *gxm_context;\nstatic void sceGxmFinish(void *p){(void)p;++finishes;}\nstatic void vgl_free(void *p){++frees;free(p);}\nstatic void release_shader(shader *p){(void)p;++releases;}\n`;
const counter=src.match(/^static unsigned program_slots_used;$/m)?.[0]||'';
const harness=pre+fields+'\nstatic program progs[MAX_CUSTOM_PROGRAMS];\n'+counter+'\nstatic shader *shaders[MAX_CUSTOM_SHADERS];\n'+section('void reset_custom_shaders(void)', '\nvoid _glMultiDrawArrays_CustomShadersIMPL')+section('unsigned vglGetFreeProgramSlots(void)', '\nGLboolean glIsProgram')+section('void glDeleteProgram(GLuint prog)', '\nvoid glGetProgramInfoLog')+`
static unsigned scan_oracle(void){unsigned free=0;for(unsigned i=0;i<MAX_CUSTOM_PROGRAMS;++i)free+=progs[i].status==PROG_INVALID;return free;}
static void agree(void){assert(vglGetFreeProgramSlots()==scan_oracle());}
int main(void){
 reset_custom_shaders(); agree();assert(vglGetFreeProgramSlots()==1024);
 for(unsigned i=1;i<=1024;i++){assert(glCreateProgram()==i);agree();assert(vglGetFreeProgramSlots()==1024-i);}
 assert(glCreateProgram()==0); agree();assert(vglGetFreeProgramSlots()==0);
 progs[0].status=PROG_LINKED;agree();assert(vglGetFreeProgramSlots()==0);
 for(unsigned i=1;i<=4;i++){glDeleteProgram(i);agree();}
 assert(finishes==4 && vglGetFreeProgramSlots()==4);
 glDeleteProgram(1);agree();assert(finishes==4 && vglGetFreeProgramSlots()==4);
 for(unsigned i=1;i<=4;i++){assert(glCreateProgram()==i);agree();}
 assert(vglGetFreeProgramSlots()==0);reset_custom_shaders();agree();assert(vglGetFreeProgramSlots()==1024);
 unsigned seed=1;for(unsigned op=0;op<10000;++op){seed=seed*1664525u+1013904223u;unsigned index=(seed>>8)%1024;if(op%97==0)reset_custom_shaders();else if(seed&1){glCreateProgram();}else if(progs[index].status){if(seed&2)progs[index].status=PROG_LINKED;else glDeleteProgram(index+1);}else glDeleteProgram(index+1);agree();}
 printf("PASS actual native allocator/query/delete/reset: 10000mixed+1024full oraclechecks linked+unlinked,4deletes=4finishcalls, noGPUtimingclaim\\n");
}
`;
fs.writeFileSync(path.join(out,'native-extracted.c'),harness);
let output='';
function run(cmd,args){const r=spawnSync(cmd,args,{encoding:'utf8',maxBuffer:16*1024*1024,timeout:180000});output+=(r.stdout||'')+(r.stderr||'');if(r.status){fs.writeFileSync(path.join(out,'native-proof.log'),output);throw Error(`${cmd} exit${r.status}`);}return r;}
run('clang',['-std=c11','-O1','-g','-fsanitize=address,undefined',path.join(out,'native-extracted.c'),'-o',path.join(out,'native-test')]);run(path.join(out,'native-test'),[]);

const record=fs.readFileSync(path.join(driver,'compile.txt'),'utf8').split('\n').find(s=>s.includes(' -c custom_shaders.c '));
const args=record.trim().split(/\s+/).map(s=>s==='custom_shaders.c'?'/proof/driver/residency/custom_shaders.c':s==='-I/vlibs/include'?'-I/vdeps/include':s==='/build/vitagl/nhustdx/obj/custom_shaders.o'?'/proof/residency-no-reader.o':s);
const docker=['run','--rm','--pull=never','--platform','linux/amd64','--network','none',
 '--mount',`type=bind,source=${driver},target=/vdeps,readonly`,
 '--mount',`type=bind,source=${sdk},target=/opt/vitasdk,readonly`,
 '--mount',`type=bind,source=${root},target=/source,readonly`,
 '--mount',`type=bind,source=${out},target=/proof`,
 '-w','/vdeps/src','sha256:f5d787d2029cc064b711ac253f3183a04aae121bce9dbd8fd3c708ae2b60d6f2'];
run('docker',[...docker,...args]);
const readerArgs=args.map(s=>s==='/proof/residency-no-reader.o'?'/proof/custom_shaders.o':s);
readerArgs.push('-DGXV_SHADER_CACHE_READER=1','-I/source/platform/vgx/vita');
run('docker',[...docker,...readerArgs]);
const controlArgs=readerArgs.map(s=>s==='/proof/driver/residency/custom_shaders.c'?'/proof/driver/candidate/custom_shaders.c':s==='/proof/custom_shaders.o'?'/proof/control/custom_shaders.o':s);
fs.mkdirSync(path.join(out,'control'));
run('docker',[...docker,...controlArgs]);
const bin='/opt/vitasdk/bin/arm-vita-eabi-';
const native=(tool,a)=>run('docker',[...docker,bin+tool,...a]);
const query=native('objdump',['-d','--disassemble=vglGetFreeProgramSlots','/proof/custom_shaders.o']);
fs.writeFileSync(path.join(out,'native-query-arm.txt'),query.stdout);
for(const name of ['residency','control'])fs.copyFileSync(path.join(driver,'lib/libvitaGL.a'),path.join(out,name+'.a'));
native('ar',['r','/proof/residency.a','/proof/custom_shaders.o']);
native('ar',['r','/proof/control.a','/proof/control/custom_shaders.o']);
const members=native('ar',['t','/proof/residency.a']).stdout.trim().split('\n');
if(members.filter(s=>s==='custom_shaders.o').length!==1)throw Error('shader archive member not unique');
fs.writeFileSync(path.join(out,'query-consumer.c'),'extern unsigned vglGetFreeProgramSlots(void);\nextern void port_vgx_shader_cache_reader_driver_v1(void);\nvolatile unsigned proof_free;\nvoid proof(void){proof_free=vglGetFreeProgramSlots();port_vgx_shader_cache_reader_driver_v1();}\n');
native('gcc',['-mcpu=cortex-a9','-mthumb','-c','/proof/query-consumer.c','-o','/proof/query-consumer.o']);
const required=['-r','--require-defined=vglGetFreeProgramSlots','--require-defined=port_vgx_shader_cache_reader_driver_v1','/proof/query-consumer.o'];
const negative=spawnSync('docker',[...docker,bin+'ld',...required,'/proof/control.a','-o','/proof/negative.o'],{encoding:'utf8',timeout:120000});
output+=(negative.stdout||'')+(negative.stderr||'');
if(negative.status!==1||!negative.stderr.includes('vglGetFreeProgramSlots'))throw Error('old reader must fail missing strong query');
native('ld',[...required,'/proof/residency.a','-o','/proof/query-reader-link.o']);
const nm=native('nm',['/proof/query-reader-link.o']).stdout;
for(const symbol of ['vglGetFreeProgramSlots','port_vgx_shader_cache_reader_driver_v1'])
 if(!new RegExp(' T '+symbol+'$','m').test(nm))throw Error('missing required symbol '+symbol);
if(!/ U vgxvita_shaderCacheRead_v1$/m.test(nm))throw Error('RAM reader callback lost');
fs.writeFileSync(path.join(out,'native-proof.log'),output);
console.log(output.split('\n').filter(s=>s.includes('PASS')).join('\n'));
console.log('PASS full ARM residency TU with/without reader; old reader missing-query link RED; copied residency archive query+reader guard GREEN. Not a full SELF or GPU timing proof.');
