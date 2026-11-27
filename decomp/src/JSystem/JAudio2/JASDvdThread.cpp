#include "JSystem/JAudio2/JASDvdThread.hpp"
#include "JSystem/JAudio2/JASTaskThread.hpp"
#include "JSystem/JKernel/JKRSolidHeap.hpp"

namespace JASDvd {
    static JASTaskThread* sThread;
};  // namespace JASDvd

JASTaskThread* JASDvd::getThreadPointer() {
    return JASDvd::sThread;
}

#ifdef TARGET_PC
extern "C" void port_os_guest_audio_disc(void* osThread) __attribute__((weak));
#endif
bool JASDvd::createThread(s32 priority, int msgCount, u32 stackSize) {
    if (sThread != NULL) {
        return false;
    }

    sThread = new (JASDram, 0) JASTaskThread(priority, msgCount, stackSize);

    JKRHeap* pCurrentHeap = JKRGetSystemHeap();

    sThread->setCurrentHeap(pCurrentHeap);
#ifdef TARGET_PC
    // (GXV_DRAWPIPE's "another guest thread ran" check, os_thread.cpp: this thread runs JAudio's loads only and is not counted)
    if (port_os_guest_audio_disc) port_os_guest_audio_disc(sThread->getThreadRecord());
#endif
    sThread->resume();
    return true;
}
