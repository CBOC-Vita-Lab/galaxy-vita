// GXV_AUDIO_CPU1 (platform/src/os/os_thread.cpp, port_audio_window): the game's audio thread on a second virtual CPU, while the main
// thread is in a window (calcAnim(), draw()) that does not drive JAudio.
//  - port_audio_window(1 / 0): GameSystem::frameLoop, around calcAnim() and draw() (the main thread, interrupts enabled).
//  - PORT_AUDIO_TOUCH(): first thing in the JAI and JASTrack entry points the main thread drives JAudio through (the managers'
//    calc/mixOut, sound starts and stops, the track's mute/pause/tempo/ports).  Should the main thread reach one inside a window, the
//    window ends there, after a CPU1 run in progress, before anything is touched.  Elsewhere (the audio thread, the switch off, outside a
//    window) it is one load of a global.
//  - port_audio_thread_mark(): JASAudioThread::run, on the audio thread: the thread CPU1 may run.
// Weak: host tests that build a few JAudio files without the scheduler (tools/test_jai_stream_prepare.sh) have none of them.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
extern int port_audio_window_flag __attribute__((weak));
void port_audio_window(int open) __attribute__((weak));
void port_audio_touch(void) __attribute__((weak));
void port_audio_thread_mark(void) __attribute__((weak));
#ifdef __cplusplus
}
#endif

#define PORT_AUDIO_TOUCH()                                                                                         \
    do {                                                                                                           \
        if (&port_audio_window_flag != 0 && __atomic_load_n(&port_audio_window_flag, __ATOMIC_RELAXED) != 0) {     \
            port_audio_touch();                                                                                    \
        }                                                                                                          \
    } while (0)
#define PORT_AUDIO_WINDOW(open)                                                                                    \
    do {                                                                                                           \
        if (port_audio_window) port_audio_window(open);                                                            \
    } while (0)
