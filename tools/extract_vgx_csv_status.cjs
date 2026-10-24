const fs=require('fs'),path=require('path'),crypto=require('crypto');
const [root,out]=process.argv.slice(2);
if(!root||!out)throw Error('checkout and output required');
const inputs={};
let source='';
for(const [file,start,end] of [
 ['platform/vgx/src/vgx_stats.cpp','int ExportFrameLog() {','void FrameEnd('],
 ['platform/vgx/src/vgx_backend_gl.cpp','int ExportCountLog() {','void Execute(']
]) {
 const data=fs.readFileSync(path.join(root,file),'utf8');
 inputs[file]=crypto.createHash('sha256').update(data).digest('hex');
 const a=data.indexOf(start),b=data.indexOf(end,a+start.length);
 if(a<0||b<0||data.indexOf(start,a+1)>=0)throw Error('CSV export markers changed');
 source+=data.slice(a,b)+'\n';
}
fs.writeFileSync(path.join(out,'csv-status-functions.inc'),source);
fs.writeFileSync(path.join(out,'inputs.json'),JSON.stringify({kind:'unmodified-function-extraction',inputs},null,2)+'\n');
console.log(JSON.stringify({functions:2,inputs}));
