#include "vita_coldtrace_fixture.h"
#include <cerrno>

namespace coldtrace_test {
void Input(Call call) {errno=100+call;}
void Output(Call call) {Require(errno==200+int(call),"the wrapper preserves errno after its clock/thread/destructor callbacks");}
void WriteMiss() {
    Input(Write);Require(sceIoWrite(17,buffer,9)==3,"short write returned unchanged");Output(Write);
}
void ReadWorker() {
    Input(Read);Require(sceIoRead(17,buffer,9)==7,"worker return parity");Output(Read);
}
void WaitWorker() {
    Input(Wait);Require(sceGxmNotificationWait(Ptr<SceGxmNotification>(0xf000))==-61,"worker notification return parity");Output(Wait);
}
void RunDriver() {
    Input(Mkdir);Require(sceIoMkdir(path,0777)==-7,"mkdir result unchanged");Output(Mkdir);
    Input(Open);Require(sceIoOpen(path,0x723,0641)==17,"open result unchanged");Output(Open);
    Input(Seek);Require(sceIoLseek(17,0x100000007ll,2)==-0x100000002ll,"64-bit seek result unchanged");Output(Seek);
    Input(Read);Require(sceIoRead(17,buffer,9)==7,"short read returned unchanged");Output(Read);
    Input(Pread);Require(sceIoPread(17,buffer,11,0x200000009ll)==-19,"pread failure returned unchanged");Output(Pread);
    Input(Close);Require(sceIoClose(17)==-31,"close failure returned unchanged");Output(Close);
    if(mode!=Mode::Hit) {
        Input(Shark);
        auto* result=shark_compile_shader_extended(source,&binarySize,SHARK_FRAGMENT_SHADER,SHARK_OPT_UNSAFE,-7,5,-3);
        Require(result==(mode==Mode::Failure?nullptr:Ptr<SceGxmProgram>(0x1000)),"compiler pointer returned unchanged");Output(Shark);
        Input(SharkSimple);Require(shark_compile_shader(source,&binarySize,SHARK_VERTEX_SHADER)==Ptr<SceGxmProgram>(0x2000),"simple compiler pointer unchanged");Output(SharkSimple);
    }
    Input(Register);Require(sceGxmShaderPatcherRegisterProgram(Ptr<SceGxmShaderPatcher>(0x3000),Ptr<SceGxmProgram>(0x4000),&programId)==-41,"register result unchanged");Output(Register);
    Input(Vertex);Require(sceGxmShaderPatcherCreateVertexProgram(Ptr<SceGxmShaderPatcher>(0x3000),programId,Ptr<SceGxmVertexAttribute>(0x5000),3,Ptr<SceGxmVertexStream>(0x6000),5,&vertexProgram)==-43,"vertex result unchanged");Output(Vertex);
    Input(Fragment);Require(sceGxmShaderPatcherCreateFragmentProgram(Ptr<SceGxmShaderPatcher>(0x3000),programId,SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,SCE_GXM_MULTISAMPLE_4X,Ptr<SceGxmBlendInfo>(0x7000),Ptr<SceGxmProgram>(0x4000),&fragmentProgram)==-47,"fragment result unchanged");Output(Fragment);
    Input(Begin);Require(sceGxmBeginScene(Ptr<SceGxmContext>(0x8000),13,Ptr<SceGxmRenderTarget>(0x9000),Ptr<SceGxmValidRegion>(0xa000),Ptr<SceGxmSyncObject>(0xb000),Ptr<SceGxmSyncObject>(0xc000),Ptr<SceGxmColorSurface>(0xd000),Ptr<SceGxmDepthStencilSurface>(0xe000))==-53,"begin result unchanged");Output(Begin);
    Input(End);Require(sceGxmEndScene(Ptr<SceGxmContext>(0x8000),Ptr<SceGxmNotification>(0xf000),Ptr<SceGxmNotification>(0x10000))==-59,"end result unchanged");Output(End);
    Input(Finish);sceGxmFinish(Ptr<SceGxmContext>(0x8000));Output(Finish);
    Input(Wait);Require(sceGxmNotificationWait(Ptr<SceGxmNotification>(0xf000))==-61,"notification result unchanged");Output(Wait);
    Input(TransferFinish);Require(sceGxmTransferFinish()==-67,"transfer-finish result unchanged");Output(TransferFinish);
}
}
