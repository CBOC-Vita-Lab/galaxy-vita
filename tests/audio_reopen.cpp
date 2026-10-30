// The host side of the Vita audio output after the console woke up: the real platform/src/audio/aaudio_out.cpp (built as the
// Vita-like path) against a fake output port.  A failed output must be reopened; an open the device refuses for a while must be
// retried on a schedule instead of leaving the game silent for good; a resume report must reopen the port (GXV_RESUME_RESET=1).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../platform/src/audio/aaudio_out.cpp"

namespace {
int64_t nowNs = 1000000000;
unsigned opens, closes, refusals;   // refusals: opens still to be refused
bool portOpen, portFailed;
std::atomic<unsigned> generation{0};
PortVitaAudioDrain drain;

void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void submit() {
    static s16 block[64 * 2];
    port_audio_submit(block, 64, 48000);
}
}

extern "C" {
int64_t port_host_time_ns(void) { return nowNs; }
int port_vita_audio_open(PortVitaAudioDrain d) {
    ++opens;
    if (refusals) { --refusals; return 0; }
    drain = d; portOpen = true; portFailed = false; return 1;
}
void port_vita_audio_close(void) { ++closes; portOpen = false; portFailed = false; }
int port_vita_audio_failed(void) { return portFailed ? 1 : 0; }
unsigned port_resume_generation(void) { return generation.load(); }
int port_remote_speaker_enabled(void) { return 0; }
int port_skip_fast_forwarding(void) { return 0; }
void port_log(const char*, ...) {}
}

int main(int argc, char** argv) {
    require(argc == 2, "mode");
    const std::string mode = argv[1];
    if (mode == "single") setenv("GXV_AUDIO_RETRY", "0", 1);
    if (mode == "resume") setenv("GXV_RESUME_RESET", "1", 1);

    submit();
    require(opens == 1 && portOpen && port_audio_queued_ns() >= 0, "the first block opens the port");
    submit();
    require(opens == 1, "a working port is not reopened");

    if (mode == "resume") {
        generation = 1;                       // the console woke up
        submit();
        require(closes == 1 && opens == 2 && portOpen, "a resume report reopens the port once");
        submit(); submit();
        require(opens == 2, "the same resume is not acted on twice");
        generation = 3;
        submit();
        require(opens == 3 && closes == 2, "a later resume reopens again");
        std::puts("PASS: resume report reopens the output port once per resume");
        return 0;
    }

    // The output failed (what the console may do on waking): closed and opened again.
    portFailed = true;
    require(port_audio_queued_ns() == -1, "a failed port is paced by the timer meanwhile");
    submit();
    require(closes == 1 && opens == 2 && portOpen, "a failed output is reopened");

    // ... and the device refuses the next opens for a while.
    portFailed = true; refusals = 3;
    submit();
    require(opens == 3 && !portOpen && port_audio_queued_ns() == -1, "the reopen was refused");
    if (mode == "single") {
        for (int i = 0; i < 20; ++i) { nowNs += 3000000000ll; submit(); }
        require(opens == 3 && !portOpen, "GXV_AUDIO_RETRY=0: one attempt, as before");
        std::puts("PASS: GXV_AUDIO_RETRY=0 keeps the single attempt");
        return 0;
    }
    submit(); submit();
    require(opens == 3, "no new attempt before the delay");
    nowNs += 260000000;  submit();
    require(opens == 4 && !portOpen, "retry after 250 ms (refused again)");
    nowNs += 300000000;  submit();
    require(opens == 4, "the second delay is longer (500 ms)");
    nowNs += 260000000;  submit();
    require(opens == 5 && !portOpen, "retry after 500 ms (refused again)");
    nowNs += 1100000000; submit();
    require(opens == 6 && portOpen, "the device is back: the next retry opens it");
    require(port_audio_queued_ns() >= 0, "paced by the device again");
    submit(); submit();
    require(opens == 6, "no more attempts once it works");
    // The schedule is capped at 2 s however many refusals there were.
    portFailed = true; refusals = 1000;
    submit();
    const unsigned base = opens;
    for (int i = 0; i < 30; ++i) { nowNs += 2100000000ll; submit(); }
    require(opens == base + 30, "an attempt every 2 s at most, for as long as it takes");
    std::puts("PASS: output reopened after failure; refused opens retried on a schedule (250 ms .. 2 s)");
}
