#include <psp2/kernel/processmgr.h>
extern "C" int port_vita_game_entry();
extern "C" void port_log_flush(void);
int main() {
    const int result=port_vita_game_entry();
    port_log_flush();  // (the log is written in batches: what still waits goes to the card first)
    sceKernelExitProcess(result);
    return result;
}
