// Galaxy 3.0 multiplayer: desktop-only harness switches. On the Vita the environment is filled from ux0:data/smg/env.txt, so a getenv of a test
// name there would be a switch a player can reach (and the Vita build defines TARGET_PC too). MP_TEST_GETENV("SMG_MP_TEST_X") is getenv on the
// desktop and a constant null on the console; the name is then not in the object at all (tests/mp/vita_hook_strings_check.sh).
#ifndef PORT_MP_TEST_ENV_H
#define PORT_MP_TEST_ENV_H
#include <stdlib.h>
#if defined(TARGET_PC) && !defined(__vita__)
#define MP_TEST_GETENV(name) getenv(name)
#else
#define MP_TEST_GETENV(name) ((const char*)0)
#endif
#endif
