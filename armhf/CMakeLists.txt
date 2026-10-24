# Super Mario Galaxy port, armhf Linux harness: the game's translation units with the PS Vita build's
# code-generation flags (vita/clang-vita-toolchain.cmake + vita/CMakeLists.txt), the platform layer,
# the vgx renderer with its null back end, and platform/desktop/main.cpp as the runner.  The purpose is to
# run the game under qemu-arm with an instruction-counting plugin (scripts/arm-*.sh), not to play it.
cmake_minimum_required(VERSION 3.24)
project(GalaxyArmhf C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(DECOMP "${ROOT}/decomp")

# ---- the same source inventory as vita/CMakeLists.txt -------------------------------------------------
file(GLOB_RECURSE GAME_SRCS CONFIGURE_DEPENDS "${DECOMP}/src/Game/*.cpp" "${DECOMP}/src/Game/*.c")
file(GLOB_RECURSE JSYSTEM_SRCS CONFIGURE_DEPENDS "${DECOMP}/src/JSystem/*.cpp")
file(GLOB_RECURSE NW4R_SRCS CONFIGURE_DEPENDS "${DECOMP}/src/nw4r/*.cpp")
set(SDK_SRCS)
foreach(module IN ITEMS gx gd mem arc tpl aralt)
  file(GLOB_RECURSE s CONFIGURE_DEPENDS "${DECOMP}/src/RVL_SDK/${module}/*.c")
  list(APPEND SDK_SRCS ${s})
endforeach()
list(FILTER SDK_SRCS EXCLUDE REGEX "/gx/GXFifo\\.c$|/gx/GXPerf\\.c$")
list(APPEND SDK_SRCS "${DECOMP}/src/RVL_SDK/os/OSMutex.c" "${DECOMP}/src/RVL_SDK/os/OSMessage.c"
     "${DECOMP}/src/RVL_SDK/thp/THPAudio.c")

# ---- platform layer: platform/src minus the legacy GLES renderer, XR and Vita-only files -----------------
file(GLOB_RECURSE PLATFORM_SRCS CONFIGURE_DEPENDS "${ROOT}/platform/src/*.c" "${ROOT}/platform/src/*.cpp")
list(FILTER PLATFORM_SRCS EXCLUDE REGEX "/gx/vita_[a-z_]+\\.cpp$")
list(FILTER PLATFORM_SRCS EXCLUDE REGEX "/gx/(gx_recorder|gl_renderer|gl_shadergen|headless_render|gpu_null|gx_ram_texture|draw_spans)\\.cpp$")
list(FILTER PLATFORM_SRCS EXCLUDE REGEX "/platform/src/xr/")
file(GLOB VGX_SRCS CONFIGURE_DEPENDS "${ROOT}/platform/vgx/src/*.cpp")
list(FILTER VGX_SRCS EXCLUDE REGEX "/vgx_backend_gl\\.cpp$")        # null back end only (no GL library)
file(GLOB HARNESS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/glue/*.cpp")
# no headset on a handheld: the port_vr_* stubs the Vita build links (vita/runtime/flat_view.cpp)
list(APPEND HARNESS_SRCS "${ROOT}/vita/runtime/flat_view.cpp")

# ---- flags ------------------------------------------------------------------------------------------------
# Vita: clang-vita-toolchain.cmake vita_flags (minus -D__vita__/newlib/-nostdlibinc, see below) + vita/CMakeLists.txt.
set(ARCH_FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fno-short-enums -femulated-tls -fno-pic -munaligned-access)
set(GAME_FLAGS
  -w -O2 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=on -fno-fast-math -fms-extensions -ferror-limit=0
  "SHELL:-include ${ROOT}/vita/include/vita_source_prelude.h"
  "SHELL:-include ${ROOT}/platform/include/port/compat.h"
  -Wno-register -Wno-multichar -Wno-invalid-offsetof -Wno-c++11-narrowing
  -Wno-writable-strings -Wno-null-conversion -Wno-deprecated-declarations
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-invalid-source-encoding
  -Wno-unused-value -Wno-return-type -Wno-switch -Wno-parentheses
  -Wno-dangling-else -Wno-tautological-compare -Wno-tautological-undefined-compare
  -Wno-undefined-bool-conversion -Wno-constant-conversion -Wno-shift-negative-value
  -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types
  -Wno-incompatible-function-pointer-types -Wno-pointer-sign -Wno-unknown-pragmas
  -Wno-ignored-attributes -Wno-microsoft-template -Wno-void-pointer-to-int-cast
  -Wno-microsoft-cast -Wno-microsoft-anon-tag -Wno-microsoft-include -Wno-language-extension-token
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")
# glibc declares __fabsf/__fabs as extern; under -fms-extensions the PPC static-inline replacements would become global C symbols.
set(PORT_C_FLAGS "SHELL:-include ${ROOT}/platform/desktop/c_compat.h")
set(INCLUDES "${ROOT}/platform/include" "${DECOMP}/include" "${DECOMP}/libs/JSystem/include"
  "${DECOMP}/libs/RVL_SDK/include" "${DECOMP}/libs/nw4r/include" "${DECOMP}/libs/RVLFaceLib/include")

# The game's translation units: identical to the Vita build, including -D__vita__ (the decomp has Vita-only code
# paths: original-size heaps, no PC-only pointer ray cast, progress checkpoints, ...).  The platform layer is
# the Linux flavour (no PS Vita SDK), told apart by PORT_ARMHF_HARNESS (port.h gives it the Vita memory sizes).
add_library(decomp OBJECT ${GAME_SRCS} ${JSYSTEM_SRCS} ${NW4R_SRCS} ${SDK_SRCS})
target_include_directories(decomp PRIVATE ${INCLUDES})
target_compile_definitions(decomp PRIVATE TARGET_PC=1 NDEBUG=1 VERSION=0 __vita__=1 PORT_ARMHF_HARNESS=1)
target_compile_options(decomp PRIVATE ${ARCH_FLAGS} ${GAME_FLAGS} $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)

add_library(platform OBJECT ${PLATFORM_SRCS} ${HARNESS_SRCS} "${ROOT}/platform/desktop/main.cpp")
target_include_directories(platform PRIVATE ${INCLUDES} "${ROOT}/platform/vgx/src")
target_compile_definitions(platform PRIVATE TARGET_PC=1 NDEBUG=1 VERSION=0 PORT_ARMHF_HARNESS=1 PORT_PLATFORM_CODE=1
  PORT_GX_VGX=1 PORT_GX_VGX_ONLY=1 _GNU_SOURCE=1)
target_compile_options(platform PRIVATE ${ARCH_FLAGS} ${GAME_FLAGS} $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)

# vgx front end (parser, vertex/texture decode, command buffers): same code generation, but not the game's compat.h.
add_library(vgx OBJECT ${VGX_SRCS})
target_include_directories(vgx PRIVATE "${ROOT}/platform/vgx/src" "${ROOT}/platform/vgx/third_party")
target_compile_definitions(vgx PRIVATE NDEBUG=1 PORT_ARMHF_HARNESS=1)
target_compile_options(vgx PRIVATE ${ARCH_FLAGS} -w -O2 -fno-exceptions -fno-rtti)
# Analysis only: -DARMHF_VGX_G=ON adds line tables to the vgx objects (same code, so the profiled binary's addresses still apply: scripts/arm-lines.py, built by scripts/arm-build-g.sh).
option(ARMHF_VGX_G "line tables for the vgx objects" OFF)
if(ARMHF_VGX_G)
  target_compile_options(vgx PRIVATE -g1)
endif()

add_executable(game $<TARGET_OBJECTS:decomp> $<TARGET_OBJECTS:platform> $<TARGET_OBJECTS:vgx>)
set_target_properties(game PROPERTIES LINKER_LANGUAGE CXX)
# Non-PIE at 0x80000000, like the Vita's fixed image address range (>= 0x40000000: os_mem.cpp requires it).
target_link_options(game PRIVATE -static-libgcc -no-pie -Wl,-Ttext-segment=0x80000000 -Wl,--no-undefined
  -Wl,-Map,${CMAKE_BINARY_DIR}/game.map)
target_link_libraries(game PRIVATE pthread m)
