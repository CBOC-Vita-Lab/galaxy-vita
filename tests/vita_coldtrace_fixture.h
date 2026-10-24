#pragma once
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vitashark.h>
#include <cstdint>

namespace coldtrace_test {
enum Call : unsigned { Mkdir,Open,Seek,Read,Pread,Write,Close,Shark,SharkSimple,Register,Vertex,Fragment,Begin,End,Finish,Wait,TransferFinish,Count };
enum class Mode { Hit,Miss,Failure };
extern Mode mode;
extern unsigned calls[Count];
extern char path[],source[],buffer[];
extern uint32_t binarySize;
extern SceGxmShaderPatcherId programId;
extern SceGxmVertexProgram* vertexProgram;
extern SceGxmFragmentProgram* fragmentProgram;
template<class T> T* Ptr(uintptr_t n) {return reinterpret_cast<T*>(n);}
void Require(bool,const char*);
void Input(Call);
void Output(Call);
void Leaf(Call);
void RunDriver();
void WriteMiss();
void ReadWorker();
void WaitWorker();
}
