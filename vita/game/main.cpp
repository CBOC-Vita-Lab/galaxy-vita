#include <psp2/kernel/processmgr.h>
extern "C" int port_vita_game_entry();
int main() {
    const int result=port_vita_game_entry();
    sceKernelExitProcess(result);
    return result;
}
