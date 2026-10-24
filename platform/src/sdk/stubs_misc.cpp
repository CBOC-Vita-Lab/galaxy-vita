// Wii services that have no meaningful equivalent on Quest: system config
// (answered with fixed settings), WiiConnect24, Mii (RFL), dynamic modules,
// the Wii remote speaker encoder, VF.
#include <string.h>

#include "port/port.h"
#include "revolution.h"
#include "revolution/nwc24.h"
#include "revolution/rso.h"
#include "revolution/sc.h"
#include "revolution/vf.h"
#include "revolution/wenc.h"
#include <RVLFaceLib.h>

extern "C" {

// ---------------------------------------------------------------------------
// SC (system configuration)
// ---------------------------------------------------------------------------
u8 SCGetAspectRatio(void) { return 1; }     // 16:9
u8 SCGetEuRgb60Mode(void) { return 1; }     // 60 Hz
// A language the disc has texts in: the one of the `language` setting, or
// the disc's own first one (dvd.cpp).
u8 SCGetLanguage(void) { return (u8)port_language_start(); }
u8 SCGetProgressiveMode(void) { return 1; }
u8 SCGetSoundMode(void) { return 1; }       // stereo

// ---------------------------------------------------------------------------
// WiiConnect24: report the service as unavailable.
// ---------------------------------------------------------------------------
static const NWC24Err kNwcUnavailable = (NWC24Err)-1;
NWC24Err NWC24OpenLib(void*) { return kNwcUnavailable; }
NWC24Err NWC24CloseLib(void) { return kNwcUnavailable; }
NWC24Err NWC24CommitMsg(NWC24MsgObj*) { return kNwcUnavailable; }
s32 NWC24GetErrorCode(void) { return 0; }
NWC24Err NWC24GetMsgSize(const NWC24MsgObj*, u32* size) {
    if (size) *size = 0;
    return kNwcUnavailable;
}
NWC24Err NWC24GetMyUserId(NWC24UserId* id) {
    if (id) memset(id, 0, sizeof(*id));
    return kNwcUnavailable;
}
NWC24Err NWC24InitMsgObj(NWC24MsgObj*, NWC24MsgType) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgAltName(NWC24MsgObj*, const u16*, u32) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgAttached(NWC24MsgObj*, const char*, u32, NWC24MIMEType) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgLedPattern(NWC24MsgObj*, u16) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgMBDelay(NWC24MsgObj*, u8) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgMBNoReply(NWC24MsgObj*, BOOL) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgTag(NWC24MsgObj*, u16) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgText(NWC24MsgObj*, const char*, u32, NWC24Charset, NWC24Encoding) { return kNwcUnavailable; }
NWC24Err NWC24SetMsgToId(NWC24MsgObj*, NWC24UserId) { return kNwcUnavailable; }

// ---------------------------------------------------------------------------
// RVL Face Library: behaves like a console whose Mii Channel has no Miis:
// initialization succeeds and every database slot is empty.
// ---------------------------------------------------------------------------
static const RFLErrcode kRflNoData = RFLErrcode_Broken;
void RFLDrawOpa(const RFLCharModel*) {}
void RFLDrawOpaCore(const RFLCharModel*, const RFLDrawCoreSetting*) {}
void RFLDrawXlu(const RFLCharModel*) {}
void RFLDrawXluCore(const RFLCharModel*, const RFLDrawCoreSetting*) {}
void RFLExit(void) {}
RFLErrcode RFLGetAdditionalInfo(RFLAdditionalInfo*, RFLDataSource, RFLMiddleDB*, u16) { return kRflNoData; }
RFLErrcode RFLGetAsyncStatus(void) { return RFLErrcode_Success; }
s32 RFLGetLastReason(void) { return 0; }
u32 RFLGetModelBufferSize(RFLResolution, u32) { return 32; }
u32 RFLGetWorkSize(BOOL) { return 32; }
RFLErrcode RFLInitCharModel(RFLCharModel*, RFLDataSource, RFLMiddleDB*, u16, void*, RFLResolution, u32) { return kRflNoData; }
RFLErrcode RFLInitResAsync(void*, void*, u32, BOOL) { return RFLErrcode_Success; }
BOOL RFLIsAvailableOfficialData(u16) { return FALSE; }
void RFLLoadMaterialSetting(const RFLDrawCoreSetting*) {}
void RFLLoadVertexSetting(const RFLDrawCoreSetting*) {}
RFLErrcode RFLMakeIcon(void*, RFLDataSource, RFLMiddleDB*, u16, RFLExpression, const RFLIconSetting*) { return kRflNoData; }
BOOL RFLSearchOfficialData(const RFLCreateID*, u16*) { return FALSE; }
void RFLSetExpression(RFLCharModel*, RFLExpression) {}
void RFLSetMtx(RFLCharModel*, const Mtx) {}

// ---------------------------------------------------------------------------
// RSO dynamic modules: only used by the Home Button menu, which the port
// replaces with the Quest system menu.
// ---------------------------------------------------------------------------
void* RSOFindExportSymbolAddr(const RSOObjectHeader*, const char*) { return nullptr; }
int RSOGetJumpCodeSize(const RSOObjectHeader*) { return 0; }
BOOL RSOIsImportSymbolResolvedAll(const RSOObjectHeader*) { return FALSE; }
int RSOLinkJump(RSOObjectHeader*, const RSOObjectHeader*, void*) { return 0; }
BOOL RSOLinkList(void*, void*) { return FALSE; }
BOOL RSOListInit(void*) { return FALSE; }
void RSOMakeJumpCode(const RSOObjectHeader*, void*) {}

// Wii remote speaker ADPCM encoder: the PCM (6 kHz mono) is handed to the
// host audio output instead of being encoded for the remote.
void port_speaker_push(const s16* samples, int count);
s32 WENCGetEncodeData(WENCInfo*, u32, const s16* pcm, s32 samples, u8* out) {
    (void)out;  // nothing reads the encoded data
    if (pcm && samples > 0) port_speaker_push(pcm, samples);
    return samples > 0 ? (samples + 1) / 2 : 0;
}

void VFInitEx(void*, u32) {}

}  // extern "C"
