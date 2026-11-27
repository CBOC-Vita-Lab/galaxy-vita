// Test API of the host stand-in for the Vita ad hoc stack (see radio_stub.cpp). One "cell" lives in shared memory, created by the test before it
// forks one process per station (vlink_vita_radio.cpp keeps its state in globals: one station = one process). Datagrams travel over UDP loopback.
#ifndef RADIO_STUB_H
#define RADIO_STUB_H
#include <atomic>
#include <cstdint>
#include <string>

namespace radiostub {
constexpr int kMaxStations = 8;
// Fault switches, per station, live (the test may flip them while the station runs). Counters ("fail next n") count down; -1 = for ever.
struct Faults {
    std::atomic<int> broadcast_broken{0};     // a send to FF:..:FF reports success and reaches nobody
    std::atomic<int> swap_sport{0};           // PdpRecv returns the source port byte-swapped
    std::atomic<int> zero_mac{0};             // own MAC reads all zero, and so does the source MAC of everything this station sends
    std::atomic<int> firstbyte_mac{0};        // the source MAC of what this station sends has its first byte altered (peer list keeps the true MAC)
    std::atomic<int> recv_err_burst{0};       // the next n PdpRecv calls fail with recv_err_code (not a time-out)
    std::atomic<int> disconnect_fail{0};      // sceNetCtlAdhocDisconnect fails
    std::atomic<int> ctlterm_fail{0};         // sceNetAdhocctlTerm fails
    std::atomic<int> adhocterm_fail{0};       // sceNetAdhocTerm fails
    std::atomic<int> pdpdelete_fail{0};       // sceNetAdhocPdpDelete fails
    std::atomic<int> term_keeps_state{0};     // 1: a failed Term/Delete/Disconnect leaves the library initialised (strict firmware); 0: it still ends it
    std::atomic<int> dialog_result{0};        // 0 ok, 1 user cancelled, 2 failed (not connected)
    std::atomic<int> dialog_ms{100};          // how long the dialog stays RUNNING
    std::atomic<int> dialog_init_fail{0};     // the next n sceNetCheckDialogInit calls refuse
    std::atomic<int> dialog_refuse_group{0};  // refuse every Init that carries a group name
    std::atomic<int> vanish{0};               // this station is suspended: silent, gone from the peer lists, its own cell state DISCONNECTED
    std::atomic<int> ctl_refuse_smg{0};       // sceNetAdhocctlInit refuses id SMGV00001 with INVALID_ARG whatever the state (the title id then meets ALREADY_INITIALIZED)
    std::atomic<int> ctl_refuse_first{0};     // the first sceNetAdhocctlInit refuses the id (the title-id fallback)
    std::atomic<int> module_unload_fail{0};   // sceSysmoduleUnloadModule fails
    std::atomic<int> peerlist_oversize{0};    // GetPeerList reports this many bytes more than the buffer holds (0 = honest)
    std::atomic<int> recv_err_code{(int)0x80410001};
};
struct Station {
    Faults f;
    std::atomic<int> joined{0};               // in the cell (dialog finished OK, not disconnected)
    std::atomic<int> up{0}, got_all{0}, done{0};   // test barriers
    // what the stub saw (the settings of the last NetCheck dialog start, traffic counters)
    std::atomic<int> dlg_inits{0}, dlg_group{0}, dlg_mode{0}; std::atomic<uint32_t> dlg_timeout_us{0};
    char dlg_group_name[9]{}, dlg_id[10]{};
    std::atomic<int> tx_bcast{0}, tx_ucast{0}, rx_bcast{0}, rx_ucast{0}, rx_errs_served{0};
    std::atomic<int> adhoc_inits{0}, ctl_inits{0}, pdp_creates{0};
    std::atomic<uint32_t> log_len{0}; char log[16384]{};
    char fail[256]{};                          // first failed check in the station's process
    std::atomic<int> peers_seen_by_me{0};
};
struct Cell {
    int base_port = 0;                         // station i (1-based) listens on 127.0.0.1:base_port + i
    int stations = 0;
    Station st[kMaxStations + 1];
};
Cell *create_cell(int stations);               // before fork; anonymous shared mapping
void attach(Cell *cell, int station);          // in the station's process: which station this process is (1-based); also reads SMG_STUB_FAULTS
void mac_of(int station, uint8_t out[6]);      // 02:00:00:00:00:NN
std::string log_of(Cell *cell, int station);
bool log_has(Cell *cell, int station, const char *needle);
// SMG_STUB_FAULTS="bcast,swap,zero,fb,err=N,dis,ctlterm,adhocterm,pdpdel,keep,dlg=cancel|fail,vanish,ms=N,refgroup,oversize=N,unload": same switches by env.
}
#endif
