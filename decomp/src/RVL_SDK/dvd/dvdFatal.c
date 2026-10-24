#include "revolution/os.h"
#include "revolution/gx.h"
#include "revolution/vi.h"
#include "revolution/dvd.h"
#include "revolution/sc.h"

extern u16 OSSetFontEncode(u16);

static void (*FatalFunc)(void) = NULL;

const char* const __DVDErrorMessageDefault[] = {
    "\n"
    "\n"
    "\n"
    "\x83\x47\x83\x89\x81\x5b\x82\xaa\x94\xad\x90\xb6\x82\xb5\x82\xdc\x82\xb5\x82\xbd\x81\x42\n"
    "\n"
    "\x83\x43\x83\x57\x83\x46\x83\x4e\x83\x67\x83\x7b\x83\x5e\x83\x93\x82\xf0\x89\x9f\x82\xb5\x82\xc4\x83\x66\x83\x42\x83\x58\x83\x4e\x82\xf0\x8e\xe6\x82\xe8\x8f\x6f\x82\xb5\x82\xc4\x82\xa9\n"
    "\x82\xe7\x81\x41\x96\x7b\x91\xcc\x82\xcc\x93\x64\x8c\xb9\x82\xf0OFF\x82\xc9\x82\xb5\x82\xc4\x81\x41\x96\x7b\x91\xcc\x82\xcc\x8e\xe6\x88\xb5\x90\xe0\x96\xbe\x8f\x91\x82\xcc\n"
    "\x8e\x77\x8e\xa6\x82\xc9\x8f\x5d\x82\xc1\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",

    "\n"
    "\n"
    "\n"
    "An error has occurred.\n"
    "Press the Eject Button, remove the\n"
    "Game Disc, and turn the power off.\n"
    "Please read the Wii Operations Manual\n"
    "for more information.",

    "\n"
    "\n"
    "\n"
    "Ein Fehler ist aufgetreten.\n"
    "Dr\xFC""cke den Ausgabeknopf, entnimm die\n"
    "Disc und schalte die Wii-Konsole aus.\n"
    "Bitte lies die Wii-Bedienungsanleitung,\n"
    "um weitere Informationen zu erhalten.",

    "\n"
    "\n"
    "\n"
    "Une erreur est survenue.\n"
    "Appuyez sur le bouton EJECT, retirez\n"
    "le disque et \xE9""teignez la console.\n"
    "Veuillez vous r\xE9""f\xE9""rer au mode d'emploi\n"
    "Wii pour plus de d\xE9""tails.",

    "\n"
    "\n"
    "\n"
    "Se ha producido un error.\n"
    "Pulsa el Bot\xF3""n EJECT, extrae el disco y\n"
    "apaga la consola. Consulta el manual de\n"
    "instrucciones de la consola Wii para\n"
    "obtener m\xE1""s informaci\xF3""n.",

    "\n"
    "\n"
    "\n"
    "Si \xE8"" verificato un errore.\n"
    "Premi il pulsante EJECT, estrai il disco\n"
    "e spegni la console. Per maggiori\n"
    "informazioni, consulta il manuale di\n"
    "istruzioni della console Wii.",

    "\n"
    "\n"
    "\n"
    "Er is een fout opgetreden.\n"
    "Druk op de EJECT-knop, verwijder de\n"
    "disk en zet het Wii-systeem uit. Lees\n"
    "de handleiding voor meer informatie."
};

const char* const __DVDErrorMessageEurope[] = {
    "\n"
    "\n"
    "\n"
    "\x83\x47\x83\x89\x81\x5b\x82\xaa\x94\xad\x90\xb6\x82\xb5\x82\xdc\x82\xb5\x82\xbd\x81\x42\n"
    "\n"
    "\x83\x43\x83\x57\x83\x46\x83\x4e\x83\x67\x83\x7b\x83\x5e\x83\x93\x82\xf0\x89\x9f\x82\xb5\x82\xc4\x83\x66\x83\x42\x83\x58\x83\x4e\x82\xf0\x8e\xe6\x82\xe8\x8f\x6f\x82\xb5\x82\xc4\x82\xa9\n"
    "\x82\xe7\x81\x41\x96\x7b\x91\xcc\x82\xcc\x93\x64\x8c\xb9\x82\xf0OFF\x82\xc9\x82\xb5\x82\xc4\x81\x41\x96\x7b\x91\xcc\x82\xcc\x8e\xe6\x88\xb5\x90\xe0\x96\xbe\x8f\x91\x82\xcc\n"
    "\x8e\x77\x8e\xa6\x82\xc9\x8f\x5d\x82\xc1\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",

    "\n"
    "\n"
    "\n"
    "An error has occurred.\n"
    "Press the EJECT Button, remove the\n"
    "Game Disc, and turn the power off.\n"
    "Please read the Wii Operations Manual\n"
    "for more information.",

    "\n"
    "\n"
    "\n"
    "Ein Fehler ist aufgetreten.\n"
    "Dr\xFC""cke den Ausgabeknopf, entnimm die\n"
    "Disc und schalte die Wii-Konsole aus.\n"
    "Bitte lies die Wii-Bedienungsanleitung,\n"
    "um weitere Informationen zu erhalten.",

    "\n"
    "\n"
    "\n"
    "Une erreur est survenue.\n"
    "Appuyez sur le bouton EJECT, retirez\n"
    "le disque et \xE9""teignez la console.\n"
    "Veuillez vous r\xE9""f\xE9""rer au mode d'emploi\n"
    "Wii pour plus de d\xE9""tails.",

    "\n"
    "\n"
    "\n"
    "Se ha producido un error.\n"
    "Pulsa el Bot\xF3""n EJECT, extrae el disco y\n"
    "apaga la consola. Consulta el manual de\n"
    "instrucciones de la consola Wii para\n"
    "obtener m\xE1""s informaci\xF3""n.",

    "\n"
    "\n"
    "\n"
    "Si \xE8"" verificato un errore.\n"
    "Premi il pulsante EJECT, estrai il disco\n"
    "e spegni la console. Per maggiori\n"
    "informazioni, consulta il manuale di\n"
    "istruzioni della console Wii.",

    "\n"
    "\n"
    "\n"
    "Er is een fout opgetreden.\n"
    "Druk op de EJECT-knop, verwijder de\n"
    "disk en zet het Wii-systeem uit. Lees\n"
    "de handleiding voor meer informatie."
};

const char* __DVDErrorMessage104 [] = {
    "\n"
    "\n"
    "\x83\x47\x83\x89\x81\x5b\x83\x52\x81\x5b\x83\x68\x82\x50\x82\x4f\x82\x53\x81\x42\n"
    "\x83\x47\x83\x89\x81\x5b\x82\xaa\x94\xad\x90\xb6\x82\xb5\x82\xdc\x82\xb5\x82\xbd\x81\x42\n"
    "\n"
    "\x83\x43\x83\x57\x83\x46\x83\x4e\x83\x67\x83\x7b\x83\x5e\x83\x93\x82\xf0\x89\x9f\x82\xb5\x82\xc4\x83\x66\x83\x42\x83\x58\x83\x4e\x82\xf0\x8e\xe6\x82\xe8\x8f\x6f\x82\xb5\x82\xc4\x82\xa9\n"
    "\x82\xe7\x81\x41\x96\x7b\x91\xcc\x82\xcc\x93\x64\x8c\xb9\x82\xf0OFF\x82\xc9\x82\xb5\x82\xc4\x81\x41\x96\x7b\x91\xcc\x82\xcc\x8e\xe6\x88\xb5\x90\xe0\x96\xbe\x8f\x91\x82\xcc\n"
    "\x8e\x77\x8e\xa6\x82\xc9\x8f\x5d\x82\xc1\x82\xc4\x82\xad\x82\xbe\x82\xb3\x82\xa2\x81\x42",

    "\n"
    "\n"
    "Error #104,\n"
    "An error has occurred.\n"
    "Press the EJECT Button, remove the\n"
    "Game Disc, and turn the power off.\n"
    "Please read the Wii Operations Manual\n"
    "for more information."
};

void __DVDShowFatalMessage(void) {
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

    switch (SCGetProductGameRegion()) {
        default:
            messageList = __DVDErrorMessageDefault;
            break;
        case 2:
            messageList = __DVDErrorMessageEurope;
            break;
        case 4:
        case 5:
            messageList = __DVDErrorMessage104;
            break;
    }

    if (SCGetLanguage() > 6) {
        message = messageList[1];
    }
    else {
        message = messageList[SCGetLanguage()];
    }

    OSFatal(fg, bg, message);
}

BOOL DVDSetAutoFatalMessaging(BOOL enable) {
    BOOL enabled, prev;
    enabled = OSDisableInterrupts();
    prev = FatalFunc ? TRUE : FALSE;
    FatalFunc = enable ? __DVDShowFatalMessage : NULL;
    OSRestoreInterrupts(enabled);
    return prev;
}

BOOL __DVDGetAutoFatalMessaging(void) {
    return FatalFunc ? TRUE : FALSE;
}

void __DVDPrintFatalMessage(void) {
    if (FatalFunc) {
        FatalFunc();
    }
}
