// The console's language (the Vita's SCE_SYSTEM_PARAM_LANG_* ids) as the converted disc names its languages ("english", "french" ...), which is what
// the "System Language" choice of Settings means (port_language_set_fallback, platform/src/dvd/dvd.cpp). Null where the disc has no such texts
// (Dutch, Portuguese, Russian, Chinese ...): the game then starts in the disc's first language, English. Pure; the host test is tests/system_language.cpp.
#pragma once

static inline const char* port_system_language_name(int id) {
    switch (id) {
        case 0: return "japanese";               // SCE_SYSTEM_PARAM_LANG_JAPANESE
        case 1: case 18: return "english";       // ENGLISH_US, ENGLISH_GB
        case 2: return "french";                 // FRENCH
        case 3: return "spanish";                // SPANISH
        case 4: return "german";                 // GERMAN
        case 5: return "italian";                // ITALIAN
        case 9: return "korean";                 // KOREAN
        default: return nullptr;
    }
}

#if defined(__vita__)
#include <psp2/system_param.h>
// The numbers above are the SDK's enum: the Vita build proves it.
static_assert(SCE_SYSTEM_PARAM_LANG_JAPANESE == 0 && SCE_SYSTEM_PARAM_LANG_ENGLISH_US == 1 && SCE_SYSTEM_PARAM_LANG_FRENCH == 2 &&
              SCE_SYSTEM_PARAM_LANG_SPANISH == 3 && SCE_SYSTEM_PARAM_LANG_GERMAN == 4 && SCE_SYSTEM_PARAM_LANG_ITALIAN == 5 &&
              SCE_SYSTEM_PARAM_LANG_KOREAN == 9 && SCE_SYSTEM_PARAM_LANG_ENGLISH_GB == 18, "system language ids");
#endif
