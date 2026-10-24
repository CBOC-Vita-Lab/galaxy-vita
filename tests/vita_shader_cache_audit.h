#pragma once
#include "vita_shader_cache_fixture.h"
#include <array>
#include <atomic>
#include <mutex>
#include <string>

namespace audit {
extern std::atomic<unsigned> mkdirCalls, openCalls, readCalls, writeCalls, seekCalls, closeCalls, clockCalls;
extern std::atomic<unsigned> compilerCalls, nativeSources, hostAllocations, sdkErrors;
extern std::atomic<uint64_t> nowUs;
extern std::atomic<int> mkdirFailure;
extern std::atomic<int> openFailure, openErrno;
extern std::mutex sdkMutex;
extern std::string lastOpenPath, lastMkdirPath;
extern int lastFlags, lastMode, openIncomingErrno;
extern thread_local bool countHostAllocation;
extern std::array<uint8_t, 24> binary;
extern unsigned nextShader;
extern GLuint expectedShader;
extern GLsizei expectedCount;
extern const GLchar* const* expectedStrings;
extern const GLint* expectedLengths;
void Require(bool good, const char* why);
uint64_t Clock() noexcept;
int SdkError(int error);
}
