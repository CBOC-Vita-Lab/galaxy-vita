cmake_minimum_required(VERSION 3.24)
project(GalaxyQuest C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DECOMP ${CMAKE_SOURCE_DIR}/decomp)

if(ANDROID)
  set(PORT_PLATFORM android)
else()
  set(PORT_PLATFORM desktop)
endif()

# ---------------------------------------------------------------------------
# Decompiled game sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE GAME_SRCS CONFIGURE_DEPENDS ${DECOMP}/src/Game/*.cpp ${DECOMP}/src/Game/*.c)
file(GLOB_RECURSE JSYSTEM_SRCS CONFIGURE_DEPENDS ${DECOMP}/src/JSystem/*.cpp)
file(GLOB_RECURSE NW4R_SRCS CONFIGURE_DEPENDS ${DECOMP}/src/nw4r/*.cpp)

# SDK libraries that are pure software and are compiled from the decomp as-is.
set(SDK_MODULES gx gd mem arc tpl aralt)
set(SDK_SRCS)
foreach(m ${SDK_MODULES})
  file(GLOB_RECURSE _s CONFIGURE_DEPENDS ${DECOMP}/src/RVL_SDK/${m}/*.c)
  list(APPEND SDK_SRCS ${_s})
endforeach()

list(FILTER SDK_SRCS EXCLUDE REGEX "/gx/GXFifo\\.c$|/gx/GXPerf\\.c$")
# OS pieces that are pure software on top of the thread API.
list(APPEND SDK_SRCS ${DECOMP}/src/RVL_SDK/os/OSMutex.c ${DECOMP}/src/RVL_SDK/os/OSMessage.c)
# THP movie audio is plain C (the video decoder is replaced by platform/src/sdk/thp_video.cpp).
list(APPEND SDK_SRCS ${DECOMP}/src/RVL_SDK/thp/THPAudio.c)

set(DECOMP_INCLUDES
  ${CMAKE_SOURCE_DIR}/platform/include
  ${DECOMP}/include
  ${DECOMP}/libs/JSystem/include
  ${DECOMP}/libs/RVL_SDK/include
  ${DECOMP}/libs/nw4r/include
  ${DECOMP}/libs/RVLFaceLib/include
)

set(PORT_DEFS TARGET_PC=1 NDEBUG=1 VERSION=0)
include(${CMAKE_SOURCE_DIR}/cmake/MpBuildDefs.cmake)   # core2: NP_ONLINE_URL (default https://galaxy.psvita.dev), MP_BUILD_ID (default dev): the
                                                       # port's own units only (target game below; the decomp objects stay as they were)
# MP3 perf: the multiplayer cost counter (port/mp_prof.h): "[mp] prof ..." lines every SMG_MP_PROF=<turns> (default 600). Off in releases.
option(SMG_MP_PROF "Multiplayer cost counter (ms per turn in the MP hooks, puppets, net)" OFF)

# Paths in the binaries (__FILE__, debug info) relative to the repository
# and the NDK, not the machine that built them.
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}/= -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
if(ANDROID)
  add_compile_options(-ffile-prefix-map=${ANDROID_NDK}=ndk)
endif()

set(PORT_COMMON_FLAGS
  -fshort-wchar
  -fno-builtin-wcslen  # (Producdevity's fork: Clang 22, not our 18, turns 16-bit length loops into newlib's 32-bit wcslen)
  -fsigned-char
  -fno-strict-aliasing
  -fwrapv
  -ffp-contract=on
  -Wno-register
  -Wno-multichar
  -Wno-invalid-offsetof
  -Wno-c++11-narrowing
  -Wno-writable-strings
  -Wno-null-conversion
  -Wno-deprecated-declarations
  -Wno-int-to-pointer-cast
  -Wno-pointer-to-int-cast
  -Wno-invalid-source-encoding
  -Wno-unused-value
  -Wno-return-type
  -Wno-switch
  -Wno-parentheses
  -Wno-dangling-else
  -Wno-tautological-compare
  -Wno-tautological-undefined-compare
  -Wno-undefined-bool-conversion
  -Wno-constant-conversion
  -Wno-shift-negative-value
  -Wno-implicit-function-declaration
  -Wno-int-conversion
  -Wno-incompatible-pointer-types
  -Wno-incompatible-function-pointer-types
  -Wno-pointer-sign
  -Wno-unknown-pragmas
  -Wno-ignored-attributes
  -Wno-microsoft-template
  -fms-extensions
  -Wno-void-pointer-to-int-cast
  -Wno-microsoft-cast
  -Wno-microsoft-anon-tag
  -Wno-microsoft-include
  -Wno-language-extension-token
  -ferror-limit=0
  -include ${CMAKE_SOURCE_DIR}/platform/include/port/compat.h
)
set(PORT_CXX_FLAGS -fno-exceptions -fno-rtti)
set(PORT_C_FLAGS)
if(NOT ANDROID AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND PORT_C_FLAGS "SHELL:-include ${CMAKE_SOURCE_DIR}/platform/desktop/c_compat.h")
endif()

add_library(decomp OBJECT ${GAME_SRCS} ${JSYSTEM_SRCS} ${NW4R_SRCS} ${SDK_SRCS})
if(SMG_MP_PROF)   # (only the multiplayer files: every other object stays the same)
  file(GLOB _mp_prof_srcs ${DECOMP}/src/Game/MP/*.cpp ${CMAKE_SOURCE_DIR}/platform/src/mp/*.cpp)
  set_source_files_properties(${_mp_prof_srcs} PROPERTIES COMPILE_DEFINITIONS SMG_MP_PROF=1)
endif()
target_include_directories(decomp PRIVATE ${DECOMP_INCLUDES})
target_compile_definitions(decomp PRIVATE ${PORT_DEFS})
target_compile_options(decomp PRIVATE ${PORT_COMMON_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:${PORT_CXX_FLAGS}> $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)

# ---------------------------------------------------------------------------
# Port platform layer + final game library
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PLATFORM_SRCS CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/platform/src/*.c
  ${CMAKE_SOURCE_DIR}/platform/src/*.cpp)
# Native replay/cache link scalar GXM adapters, not the GLES diagnostics.
# vita_xfb.cpp is Vita/GXM-only too (only vita_game_worker.cpp, also excluded, uses it); without it in this list the
# desktop link fails with undefined port_vita_gxm_* / genVita* symbols.  Added by the x86-64 reproduction (2026-10-09).
list(FILTER PLATFORM_SRCS EXCLUDE REGEX "/gx/vita_(frame|texture_cache|material_cache|shader_bundle|efb|game_worker|xfb)\\.cpp$")
list(FILTER PLATFORM_SRCS EXCLUDE REGEX "/gx/gx_ram_texture\\.cpp$")

if(ANDROID)
  add_library(game SHARED $<TARGET_OBJECTS:decomp> ${PLATFORM_SRCS})
else()
  # The game embeds 32-bit pointers. A non-PIE image in the Wii window keeps
  # both native code/data and emulated arenas representable without dlopen.
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|aarch64|arm64")
    message(FATAL_ERROR "Desktop diagnostic requires 64-bit Linux; use tools/Dockerfile.native")
  endif()
  list(FILTER PLATFORM_SRCS EXCLUDE REGEX "/xr/xr_app\\.cpp$")
  find_package(SDL2 REQUIRED)
  add_executable(game $<TARGET_OBJECTS:decomp> ${PLATFORM_SRCS} platform/desktop/main.cpp)
  # MP3 (harness): the game's operator new (JKRHeap gameAlloc) gives 4-byte aligned blocks, as on the Wii and the Vita, but
  # x86-64 clang assumes 16 (__STDCPP_DEFAULT_NEW_ALIGNMENT__) and zeroes new objects with movaps: #GP (SIGSEGV, address 0)
  # in the first constructor that gets a block not 16-aligned (StarPointerOnOffController, at boot). 8: no aligned SSE
  # stores to new'd memory (x86 allows unaligned scalar access); -fno-aligned-allocation keeps over-aligned types on the
  # game's operator new instead of libc++'s aligned one (host memory). Every C++ target here (decomp, game, vgx, net).
  string(APPEND CMAKE_CXX_FLAGS " -fnew-alignment=8 -fno-aligned-allocation")
  target_compile_options(decomp PRIVATE -mcmodel=large -fno-pie $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
  target_compile_options(game PRIVATE -mcmodel=large -fno-pie $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
  set(GALAXY_NATIVE_IMAGE_BASE "0x98000000" CACHE STRING "32-bit diagnostic game image base")
  target_link_options(game PRIVATE -stdlib=libc++ -fuse-ld=lld -no-pie -Wl,--image-base=${GALAXY_NATIVE_IMAGE_BASE})
  # MP3 integ (8 Nov): this toolchain's crtbegin.o (GCC 13) addresses .tm_clone_table with 32-bit sign-extended relocations, which
  # cannot reach an image at 0x98000000 (lld: "R_X86_64_32S out of range"). The position-independent start files
  # (crtbeginS.o / crtendS.o) have no such relocation: the executable stays non-PIE at the documented base.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    foreach(_f crt1.o crti.o crtbeginS.o crtendS.o crtn.o)
      execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=${_f} OUTPUT_VARIABLE _p OUTPUT_STRIP_TRAILING_WHITESPACE)
      string(REPLACE "." "_" _v ${_f})
      set(GALAXY_START_${_v} ${_p})
    endforeach()
    if(EXISTS "${GALAXY_START_crtbeginS_o}" AND EXISTS "${GALAXY_START_crt1_o}")
      target_link_options(game PRIVATE -nostartfiles ${GALAXY_START_crt1_o} ${GALAXY_START_crti_o} ${GALAXY_START_crtbeginS_o})
      target_link_libraries(game PRIVATE ${GALAXY_START_crtendS_o} ${GALAXY_START_crtn_o})
    endif()
  endif()
  target_link_libraries(game PRIVATE SDL2::SDL2 EGL GLESv2 pthread m dl)
endif()
target_include_directories(game PRIVATE ${DECOMP_INCLUDES})
target_compile_definitions(game PRIVATE ${PORT_DEFS} PORT_PLATFORM_CODE=1 ${MP_BUILD_DEFS})

# ---------------------------------------------------------------------------
# vgx: the GLES2-class GX renderer (platform/vgx), the Vita's renderer. On the
# desktop it runs on Mesa beside the legacy GLES 3.2 renderer, chosen per run
# with PETARI_BACKEND=vgx, so its pictures can be compared with the legacy ones.
# A library of its own: it must not get the game's forced compat.h.
# ---------------------------------------------------------------------------
option(PORT_GX_VGX "Build the vgx GX renderer" ON)
if(PORT_GX_VGX AND NOT ANDROID)
  file(GLOB VGX_SRCS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/platform/vgx/src/*.cpp)
  list(FILTER VGX_SRCS EXCLUDE REGEX "/vgx_backend_null\\.cpp$")
  add_library(vgx STATIC ${VGX_SRCS} platform/vgx/host/vgx_host_egl.cpp)
  target_include_directories(vgx PRIVATE platform/vgx/src platform/vgx/third_party)
  # (merge with the performance line: vgx sources include "port/hitch24.h"; searched after the system directories, so the game's own libc
  # shim headers in platform/include never shadow the host's)
  target_compile_options(vgx PRIVATE -idirafter ${CMAKE_SOURCE_DIR}/platform/include)
  target_compile_features(vgx PRIVATE cxx_std_20)
  target_compile_options(vgx PRIVATE -O2 -fno-exceptions -fno-rtti -mcmodel=large -fno-pie -stdlib=libc++ -Wall -Wno-unused-function)
  target_compile_definitions(game PRIVATE PORT_GX_VGX=1)
  target_include_directories(game PRIVATE platform/vgx/src)
  target_link_libraries(game PRIVATE vgx)
endif()
target_compile_options(game PRIVATE ${PORT_COMMON_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:${PORT_CXX_FLAGS}> $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)
target_link_options(game PRIVATE -Wl,--no-undefined -Wl,--error-limit=0)

# ---------------------------------------------------------------------------
# MP3: net/ = the vendored Strikers Charged online stack (VLink over POSIX UDP and the online transport; NpNet: STUN, punching, TURN,
# session crypto, signaling over plain http to a local fake service). Game-agnostic, its own flags (not the game's forced compat.h);
# no TLS on the desktop (np_https answers UNAVAILABLE for https://). net/GALAXY-CHANGES.txt lists what differs from Strikers.
# ---------------------------------------------------------------------------
set(SMG_NET_SRC ${CMAKE_SOURCE_DIR}/net/src)
if(TRUE)   # (every build that has the game: platform/src/mp/mp_keyboard.cpp calls into it)
  add_library(smg_net STATIC
    ${SMG_NET_SRC}/vlink.cpp ${SMG_NET_SRC}/vlink_udp.cpp ${SMG_NET_SRC}/vlink_online.cpp ${SMG_NET_SRC}/np_glue.cpp
    ${SMG_NET_SRC}/np_keyboard_host.cpp ${SMG_NET_SRC}/np_https.cpp ${SMG_NET_SRC}/np_https_sys.cpp
    ${SMG_NET_SRC}/online/np_net.cpp ${SMG_NET_SRC}/online/np_crypto.cpp ${SMG_NET_SRC}/online/np_hash.cpp ${SMG_NET_SRC}/online/np_turn.cpp
    ${SMG_NET_SRC}/online/np_stun.cpp ${SMG_NET_SRC}/online/np_peer.cpp ${SMG_NET_SRC}/online/np_online.cpp ${SMG_NET_SRC}/online/np_fault.cpp
    ${SMG_NET_SRC}/online/np_signal_fake.cpp ${SMG_NET_SRC}/online/np_signal_http.cpp ${SMG_NET_SRC}/online/np_http.cpp
    ${SMG_NET_SRC}/online/np_http_plain.cpp ${SMG_NET_SRC}/online/np_udp_posix.cpp ${SMG_NET_SRC}/online/np_upnp.cpp ${SMG_NET_SRC}/online/np_natpmp.cpp
    ${SMG_NET_SRC}/online/third_party/monocypher/monocypher.c)
  target_include_directories(smg_net PUBLIC ${CMAKE_SOURCE_DIR}/net/include)
  target_compile_features(smg_net PRIVATE cxx_std_20)
  target_compile_options(smg_net PRIVATE -O2 -fsigned-char -fno-short-enums -fno-strict-aliasing -fwrapv
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -Wno-unused-function> $<$<COMPILE_LANGUAGE:C>:-w>)
  if(NOT ANDROID)   # (the desktop game's code model and C++ library)
    target_compile_options(smg_net PRIVATE -mcmodel=large -fno-pie $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
    target_link_libraries(smg_net PUBLIC pthread)
  endif()
  target_link_libraries(game PRIVATE smg_net)
endif()
if(ANDROID)
  # -Bsymbolic: game code must bind to the port's operator new/delete, not the
  # system C++ runtime's.
  target_link_options(game PRIVATE -Wl,-Bsymbolic -Wl,--gc-sections)
  target_link_libraries(game PRIVATE log android)
endif()

if(ANDROID)
  # The Khronos OpenXR loader (tools/fetch_openxr.sh puts it there).
  set(OPENXR_DIR ${CMAKE_SOURCE_DIR}/third_party/openxr/prefab/modules)
  set(GLUE_DIR ${ANDROID_NDK}/sources/android/native_app_glue)

  add_library(openxr_loader SHARED IMPORTED)
  set_target_properties(openxr_loader PROPERTIES IMPORTED_LOCATION ${OPENXR_DIR}/openxr_loader/libs/android.arm64-v8a/libopenxr_loader.so)

  target_include_directories(game PRIVATE ${OPENXR_DIR}/headers/include ${GLUE_DIR})
  target_link_libraries(game PRIVATE openxr_loader EGL GLESv3 aaudio)

  # Launcher: reserves the 32-bit game window and loads libgame.so into it.
  add_library(main SHARED platform/android/launcher.c ${GLUE_DIR}/android_native_app_glue.c)
  target_include_directories(main PRIVATE ${GLUE_DIR})
  target_link_libraries(main PRIVATE android log dl)
  target_link_options(main PRIVATE -u ANativeActivity_onCreate)

  # Preloaded into the app process by wrap.sh: reserves the game window before
  # ART lays out its heap.
  add_library(reserve SHARED platform/android/reserve.c)
  target_link_libraries(reserve PRIVATE log)

  # Bring-up tool: boots libgame.so from an adb shell without VR.
  add_executable(petari_headless platform/android/headless.c)
  target_link_libraries(petari_headless PRIVATE log dl m)
  target_include_directories(petari_headless PRIVATE platform/include)
endif()
