#pragma once
#include <stdint.h>
enum { SCE_AUDIO_OUT_PORT_TYPE_MAIN, SCE_AUDIO_OUT_SAMPLE_RATE_48000=48000, SCE_AUDIO_OUT_MODE_STEREO=2, SCE_AUDIO_VOLUME_0DB=32768, SCE_AUDIO_VOLUME_FLAG_L_CH=1, SCE_AUDIO_VOLUME_FLAG_R_CH=2 };
typedef int SceAudioOutChannelFlag;
int sceAudioOutOpenPort(int, int, int, int);
int sceAudioOutSetVolume(int, SceAudioOutChannelFlag, int*);
int sceAudioOutOutput(int, const void*);
int sceAudioOutReleasePort(int);
