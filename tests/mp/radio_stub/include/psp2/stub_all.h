// Host stand-in for the PS Vita SDK declarations that net/src/vlink_vita_radio.cpp uses (tests/mp/radio_stub). Written fresh from the shape of the
// vitaSDK API (names, argument lists, documented constants); it contains no code from any other project. The implementation is radio_stub.cpp.
// Every psp2/... header the radio includes is one line that includes this file.
#ifndef RADIO_STUB_PSP2_ALL_H
#define RADIO_STUB_PSP2_ALL_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
typedef uint32_t SceUInt32;
typedef int32_t SceInt32;
typedef int8_t SceInt8;
typedef uint8_t SceUInt8;
typedef uint8_t SceUChar8;
typedef uint8_t SceChar8;
typedef uint16_t SceUShort16;
typedef uint64_t SceUInt64;
typedef int SceBool;
#define PSP2_SDK_VERSION 0x03570011

// ---- kernel ----
enum { SCE_KERNEL_ERROR_WAIT_TIMEOUT = (int)0x80028005 };
#define SCE_KERNEL_CPU_MASK_USER_2 0x00040000
typedef int (*SceKernelThreadEntry)(SceSize args, void *argp);
typedef struct SceKernelThreadInfo { SceSize size; char name[32]; } SceKernelThreadInfo;
uint64_t sceKernelGetProcessTimeWide(void);
int sceKernelDelayThread(SceUInt delay);
SceUID sceKernelCreateThread(const char *name, SceKernelThreadEntry entry, int initPriority, SceSize stackSize, SceUInt attr, int cpuAffinityMask, const void *option);
int sceKernelStartThread(SceUID thid, SceSize arglen, void *argp);
int sceKernelWaitThreadEnd(SceUID thid, int *stat, SceUInt *timeout);
int sceKernelDeleteThread(SceUID thid);
int sceKernelGetThreadInfo(SceUID thid, SceKernelThreadInfo *info);
int sceKernelGetThreadId(void);

// ---- sysmodule ----
enum { SCE_SYSMODULE_LOADED = 0 };
typedef enum SceSysmoduleModuleId { SCE_SYSMODULE_NET = 0x0001, SCE_SYSMODULE_PSPNET_ADHOC = 0x0043 } SceSysmoduleModuleId;
int sceSysmoduleLoadModule(SceSysmoduleModuleId id);
int sceSysmoduleUnloadModule(SceSysmoduleModuleId id);
int sceSysmoduleIsLoaded(SceSysmoduleModuleId id);

// ---- sceNet / netctl ----
typedef struct SceNetEtherAddr { uint8_t data[6]; } SceNetEtherAddr;
typedef struct SceNetInitParam { void *memory; int size; int flags; } SceNetInitParam;
enum { SCE_NET_ERROR_EBUSY = (int)0x80410110 };
int sceNetInit(SceNetInitParam *param);
int sceNetTerm(void);
typedef enum SceNetCtlState { SCE_NETCTL_STATE_DISCONNECTED, SCE_NETCTL_STATE_CONNECTING, SCE_NETCTL_STATE_FINALIZING, SCE_NETCTL_STATE_CONNECTED } SceNetCtlState;
int sceNetCtlInit(void);
void sceNetCtlTerm(void);
int sceNetCtlAdhocGetState(int *state);
int sceNetCtlAdhocDisconnect(void);

// ---- ad hoc (PDP) ----
enum {
    SCE_ERROR_NET_ADHOC_INVALID_SOCKET_ID = (int)0x80410701, SCE_ERROR_NET_ADHOC_INVALID_ADDR = (int)0x80410702,
    SCE_ERROR_NET_ADHOC_INVALID_PORT = (int)0x80410703, SCE_ERROR_NET_ADHOC_INVALID_DATALEN = (int)0x80410705,
    SCE_ERROR_NET_ADHOC_WOULD_BLOCK = (int)0x80410709, SCE_ERROR_NET_ADHOC_PORT_IN_USE = (int)0x8041070A,
    SCE_ERROR_NET_ADHOC_NOT_CONNECTED = (int)0x8041070B, SCE_ERROR_NET_ADHOC_INVALID_ARG = (int)0x80410711,
    SCE_ERROR_NET_ADHOC_NOT_INITIALIZED = (int)0x80410712, SCE_ERROR_NET_ADHOC_ALREADY_INITIALIZED = (int)0x80410713,
    SCE_ERROR_NET_ADHOC_TIMEOUT = (int)0x80410715
};
enum { SCE_NET_ADHOC_F_NONBLOCK = 0x0001 };
#define SCE_NET_ADHOC_PDP_MFS 1444
int sceNetAdhocInit(void);
int sceNetAdhocTerm(void);
int sceNetAdhocPdpCreate(const SceNetEtherAddr *saddr, SceUShort16 sport, unsigned int bufsize, int flag);
int sceNetAdhocPdpSend(int id, const SceNetEtherAddr *daddr, SceUShort16 dport, const void *data, int len, unsigned int timeout, int flag);
int sceNetAdhocPdpRecv(int id, SceNetEtherAddr *saddr, SceUShort16 *sport, void *buf, int *len, unsigned int timeout, int flag);
int sceNetAdhocPdpDelete(int id, int flag);

// ---- adhocctl ----
enum { SCE_ERROR_NET_ADHOCCTL_INVALID_ARG = (int)0x80410B04, SCE_ERROR_NET_ADHOCCTL_ALREADY_INITIALIZED = (int)0x80410B07,
       SCE_ERROR_NET_ADHOCCTL_NOT_INITIALIZED = (int)0x80410B08 };
enum { SCE_NET_ADHOCCTL_ADHOCTYPE_PRODUCT_ID = 0, SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED = 1, SCE_NET_ADHOCCTL_ADHOCTYPE_SYSTEM = 2 };
typedef struct SceNetAdhocctlAdhocId { int type; SceChar8 data[9]; SceUChar8 padding[3]; } SceNetAdhocctlAdhocId;
typedef struct SceNetAdhocctlGroupName { SceChar8 data[8]; } SceNetAdhocctlGroupName;
typedef struct SceNetAdhocctlNickname { SceChar8 data[128]; } SceNetAdhocctlNickname;
typedef struct SceNetAdhocctlPeerInfo {
    struct SceNetAdhocctlPeerInfo *next; struct SceNetAdhocctlNickname nickname; struct SceNetEtherAddr macAddr; SceUChar8 padding[6]; SceUInt64 lastRecv;
} SceNetAdhocctlPeerInfo;
int sceNetAdhocctlInit(const struct SceNetAdhocctlAdhocId *adhocId);
int sceNetAdhocctlTerm(void);
int sceNetAdhocctlGetPeerList(int *buflen, void *buf);
int sceNetAdhocctlGetEtherAddr(struct SceNetEtherAddr *addr);

// ---- common dialog, NetCheck dialog, app util, app manager ----
enum { SCE_COMMON_DIALOG_ERROR_NOT_CONFIGURED = (int)0x80020407, SCE_COMMON_DIALOG_ERROR_NOT_FINISHED = (int)0x80020410,
       SCE_COMMON_DIALOG_ERROR_NOT_IN_USE = (int)0x80020411, SCE_COMMON_DIALOG_ERROR_BUSY = (int)0x80020401 };
typedef enum SceCommonDialogStatus { SCE_COMMON_DIALOG_STATUS_NONE = 0, SCE_COMMON_DIALOG_STATUS_RUNNING = 1, SCE_COMMON_DIALOG_STATUS_FINISHED = 2 } SceCommonDialogStatus;
typedef enum SceCommonDialogResult { SCE_COMMON_DIALOG_RESULT_OK, SCE_COMMON_DIALOG_RESULT_USER_CANCELED, SCE_COMMON_DIALOG_RESULT_ABORTED } SceCommonDialogResult;
typedef enum SceSystemParamLang { SCE_SYSTEM_PARAM_LANG_JAPANESE = 0, SCE_SYSTEM_PARAM_LANG_ENGLISH_US = 1, SCE_SYSTEM_PARAM_LANG_MAX_VALUE = 18 } SceSystemParamLang;
typedef enum SceSystemParamEnterButtonAssign { SCE_SYSTEM_PARAM_ENTER_BUTTON_CIRCLE = 0, SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS = 1, SCE_SYSTEM_PARAM_ENTER_BUTTON_MAX_VALUE = 2 } SceSystemParamEnterButtonAssign;
enum { SCE_SYSTEM_PARAM_ID_LANG = 1, SCE_SYSTEM_PARAM_ID_ENTER_BUTTON = 2 };
typedef struct SceCommonDialogParam { uint32_t magic; } SceCommonDialogParam;
typedef struct SceCommonDialogConfigParam { SceUInt32 sdkVersion; SceSystemParamLang language; SceSystemParamEnterButtonAssign enterButtonAssign; SceUInt8 reserved[32]; } SceCommonDialogConfigParam;
static inline void sceCommonDialogConfigParamInit(SceCommonDialogConfigParam *param) {
    for (unsigned i = 0; i < sizeof *param; ++i) ((unsigned char *)param)[i] = 0;
    param->language = SCE_SYSTEM_PARAM_LANG_MAX_VALUE; param->enterButtonAssign = SCE_SYSTEM_PARAM_ENTER_BUTTON_MAX_VALUE; param->sdkVersion = PSP2_SDK_VERSION;
}
int sceCommonDialogSetConfigParam(const SceCommonDialogConfigParam *configParam);

typedef struct SceNpCommunicationId { char data[9]; char term; SceUChar8 num; char dummy; } SceNpCommunicationId;
typedef enum SceNetCheckDialogMode { SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN = 5, SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CREATE = 6, SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_JOIN = 7 } SceNetCheckDialogMode;
typedef struct SceNetCheckDialogParam {
    SceUInt32 sdkVersion; SceCommonDialogParam commonParam; SceInt32 mode; SceNpCommunicationId npCommunicationId; void *ps3ConnectParam;
    SceNetAdhocctlGroupName *groupName; SceUInt32 timeoutUs; SceUInt8 reserved[104];
} SceNetCheckDialogParam;
typedef struct SceNetCheckDialogResult { SceInt32 result; SceBool psnModeSucceeded; SceUInt8 reserved[124]; } SceNetCheckDialogResult;
static inline void sceNetCheckDialogParamInit(SceNetCheckDialogParam *param) {
    for (unsigned i = 0; i < sizeof *param; ++i) ((unsigned char *)param)[i] = 0;
    param->commonParam.magic = 0xC0DE; param->sdkVersion = PSP2_SDK_VERSION;
}
SceInt32 sceNetCheckDialogInit(SceNetCheckDialogParam *param);
SceCommonDialogStatus sceNetCheckDialogGetStatus(void);
SceInt32 sceNetCheckDialogAbort(void);
SceInt32 sceNetCheckDialogGetResult(SceNetCheckDialogResult *result);
SceInt32 sceNetCheckDialogTerm(void);

enum { SCE_APPUTIL_ERROR_BUSY = (int)0x80100603 };
typedef struct SceAppUtilInitParam { SceSize workBufSize; uint8_t reserved[60]; } SceAppUtilInitParam;
typedef struct SceAppUtilBootParam { int attr; unsigned int appVersion; uint8_t reserved[32]; } SceAppUtilBootParam;
int sceAppUtilInit(SceAppUtilInitParam *initParam, SceAppUtilBootParam *bootParam);
int sceAppUtilSystemParamGetInt(unsigned int paramId, int *value);
int sceAppMgrAppParamGetString(int pid, int param, char *string, SceSize length);

#ifdef __cplusplus
}
#endif
#endif
