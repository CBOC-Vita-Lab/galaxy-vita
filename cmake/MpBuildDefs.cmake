# Galaxy 3.0 multiplayer (core2): the two build-time choices of the multiplayer layer, one place for every build (the desktop
# CMakeLists.txt, the Vita source stage vita/CMakeLists.txt, the Vita link stage vita/vgx-game/CMakeLists.txt; tools/vgx_build/build.py
# passes --online-url / --build-id on to them).
#   NP_ONLINE_URL  the online service this build talks to when neither SMG_ONLINE_URL (env.txt) nor `server = ...` in mp.cfg names one.
#                  Default https://galaxy.psvita.dev (the Galaxy service's own hostname, 14 Nov 2026). There is NO fallback to the old
#                  workers.dev address: accounts belong to one server address, a silent fallback would create a second account.
#                  -DNP_ONLINE_URL=none (or empty) compiles no service in (host tests; online then needs SMG_ONLINE_URL or mp.cfg).
#                  https://host[:port] only (MP_ALLOW_HTTP_URL=ON accepts http:// for host tests against the local fake).
#   MP_BUILD_ID    goes into the multiplayer fingerprint: consoles with different build ids never play together. Default "dev"
#                  (the value platform/src/mp/mp_core.h assumes when nothing is passed: a default build is unchanged).
# Sets MP_BUILD_DEFS (a list for target_compile_definitions). Usable with cmake -P too (tests/mp/build_defs_check.cmake).
set(NP_ONLINE_URL "https://galaxy.psvita.dev" CACHE STRING "Online service compiled into the build (https://host[:port]); none or empty = no service")
set(MP_BUILD_ID "dev" CACHE STRING "Multiplayer build id (part of the version fingerprint)")
option(MP_ALLOW_HTTP_URL "Accept an http:// NP_ONLINE_URL (host tests against the local fake only)" OFF)

set(MP_BUILD_DEFS)
if(NOT MP_BUILD_ID MATCHES "^[A-Za-z0-9._+-]+$")
  message(FATAL_ERROR "MP_BUILD_ID '${MP_BUILD_ID}': letters, digits and . _ + - only")
endif()
string(LENGTH "${MP_BUILD_ID}" _mp_id_len)
if(_mp_id_len GREATER 40)
  message(FATAL_ERROR "MP_BUILD_ID: at most 40 characters")
endif()
list(APPEND MP_BUILD_DEFS "MP_BUILD_ID=\"${MP_BUILD_ID}\"")
if(NP_ONLINE_URL STREQUAL "none")
  set(NP_ONLINE_URL "")
endif()
if(NOT NP_ONLINE_URL STREQUAL "")
  set(_mp_url_re "^https://[A-Za-z0-9.-]+(:[0-9]+)?$")
  if(MP_ALLOW_HTTP_URL)
    set(_mp_url_re "^https?://[A-Za-z0-9.-]+(:[0-9]+)?$")
  endif()
  if(NOT NP_ONLINE_URL MATCHES "${_mp_url_re}")
    message(FATAL_ERROR "NP_ONLINE_URL '${NP_ONLINE_URL}': https://host[:port] only (no path, no quotes)")
  endif()
  list(APPEND MP_BUILD_DEFS "NP_ONLINE_URL=\"${NP_ONLINE_URL}\"")
  message(STATUS "multiplayer: online service compiled in: ${NP_ONLINE_URL}")
else()
  message(STATUS "multiplayer: no online service compiled in (NP_ONLINE_URL=none): SMG_ONLINE_URL or mp.cfg `server` must name one")
endif()
message(STATUS "multiplayer: build id ${MP_BUILD_ID}")
