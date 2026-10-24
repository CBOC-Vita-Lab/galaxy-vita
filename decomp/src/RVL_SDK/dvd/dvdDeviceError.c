#include "revolution/os.h"
#include "revolution/dvd.h"
#include "revolution/sc.h"

vu8 __OSDeviceCheckCode : (OS_BASE_CACHED + 0x319C);
extern u16 OSSetFontEncode(u16);
extern void __DVDShowFatalMessage(void);
static u8 CheckBuffer[32] __attribute__ ((aligned(32)));

static volatile BOOL lowDone = TRUE;
static volatile u32 lowIntType = 0;

static void lowCallback(u32 intType) {
    lowIntType = intType;
    lowDone = TRUE;
}

void __DVDShowDeviceErrorMessage(void);

BOOL __DVDCheckDevice(void) {
    u32 checkCode = 0x460A0000;
    u32 outOfRangeError = 0xFFFFFFFF;
    u32 reportKeyError = 0xFFFFFFFF;
    OSIOSRev iosRev;

    if (OSGetPhysicalMem2Size() == 0x08000000) {
        return TRUE;
    }

    __OSGetIOSRev(&iosRev);
    
    if (iosRev.major < 30 || iosRev.major >= 254) {
        return TRUE;
    }

    if (__OSDeviceCheckCode == 129) {
        checkCode = 0x7ED40000;
    }

    lowDone = FALSE;
    DVDLowUnencryptedRead((void*)CheckBuffer, 32, checkCode, lowCallback);\

    while (!lowDone) {

    }

    switch (lowIntType) {
        case 2:
            break;
        case 1:
            goto invalid;
            break;
        default:
            goto fatal;
            break;
    }

    lowDone = FALSE;
    DVDLowRequestError(lowCallback);

    while (!lowDone) {

    }

    outOfRangeError = DVDLowGetImmBufferReg();

    switch (lowIntType) {
        case 1:
        {
            if ((DVDLowGetImmBufferReg() & 0xFF000000) != 0) {
                goto recover;
                break;
            }

            switch (DVDLowGetImmBufferReg() & 0xFFFFFF) {
                case 0x52100:
                    break;
                default:
                    goto invalid;
                    break;
            }

            break;
        }

        default:
            goto fatal;
            break;
    }

    lowDone = FALSE;
    DVDLowReportKey((DVDVideoReportKey*)CheckBuffer, 0x40000, 0, lowCallback);

    while (!lowDone) {

    }

    switch (lowIntType) {
        case 2:
            break;
        case 1:
            goto invalid;
            break;
        default:
            goto fatal;
            break;
    }

    lowDone = FALSE;
    DVDLowRequestError(lowCallback);

    while (!lowDone) {

    }

    reportKeyError = DVDLowGetImmBufferReg();
    switch (lowIntType) {
        case 1:
            if ((DVDLowGetImmBufferReg() & 0xFF000000) != 0) {
                goto recover;
                break;
            }

            switch (DVDLowGetImmBufferReg() & 0xFFFFFF) {
                case 0x53100:
                case 0x52000:
                    break;
                default:
                    goto invalid;
                    break;
            }

            break;

        default:
            goto fatal;
            break;
    }

valid:
    return TRUE;

invalid:
    __DVDShowDeviceErrorMessage();
    return FALSE;

recover:
    return FALSE;

fatal:
    __DVDShowFatalMessage();
    return FALSE;
}

const char* const __DVDDeviceErrorMessage[] = {
    "\n\n\n\x83\x47\x83\x89\x81\x5b\x83\x52\x81\x5b\x83\x68\x82\x4f\x82\x4f\x82\x50\x81\x42\n"
    "\x95\x73\x96\xbe\x82\xc8\x83\x66\x83\x6f\x83\x43\x83\x58\x82\xaa\x8c\xa9\x82\xc2\x82\xa9\x82\xe8\x82\xdc\x82\xb5\x82\xbd\x81\x42",

    "\n\n\nError #001,\n"
    "unauthorized device has been detected.",

    "\n\n\nFehler #001:\n"
    "Es wurde eine unzul\xe4\x73sige Komponente\n"
    "entdeckt.",

    "\n\n\nErreur 001:\n"
    "un dispositif non autoris\xe9 a \xe9t\xe9 d\xe9tect\xe9.",

    "\n\n\nError 001:\n"
    "Se ha detectado un dispositivo no\n"
    "autorizado.",

    "\n\n\nErrore #001:\n"
    "rilevato un dispositivo non autorizzato.",

    "\n\n\nFout #001:\n"
    "ongeoorloofd onderdeel gevonden."
};

void __DVDShowDeviceErrorMessage(void) {
    const char* message;
    const char* const* messageList;
    GXColor bg = { 0, 0, 0, 0 };
    GXColor fg = { 255, 255, 255, 0 };

    if (SCGetLanguage() == 0) {
        OSSetFontEncode(1);
    }
    else {
        OSSetFontEncode(0);
    }

    messageList = __DVDDeviceErrorMessage;

    if (SCGetLanguage() > 6) {
        message = messageList[1];
    }
    else {
        message = messageList[SCGetLanguage()];
    }

    OSFatal(fg, bg, message);
}