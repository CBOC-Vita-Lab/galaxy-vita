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
option(GXV_SHARED_CLIP_PREP "Reuse common clipping planes on supported Vita ARM32 builds" ON)
set_property(SOURCE "${DECOMP}/src/Game/LiveActor/ClippingJudge.cpp" APPEND PROPERTY
  COMPILE_DEFINITIONS "GXV_SHARED_CLIP_PREP=$<BOOL:${GXV_SHARED_CLIP_PREP}>")
option(GXV_PROJMAP_RUN_REUSE "Reuse consecutive equal projection initializers within each update" ON)
set_property(SOURCE "${DECOMP}/src/Game/LiveActor/MaterialCtrl.cpp" APPEND PROPERTY
  COMPILE_DEFINITIONS "GXV_PROJMAP_RUN_REUSE=$<BOOL:${GXV_PROJMAP_RUN_REUSE}>")

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
# Back end: null (default; no GL library, draws nothing) or gl-stub (the real GL back end, vgx_backend_gl.cpp, against a GL
# that does nothing: armhf/glstub/GLES2/gl2.h).  gl-stub runs the back end's own code (state caching, uniform and attribute
# de-duplication, target pooling), which the null back end skips, so the render thread's work is the Vita's minus the driver.
set(ARMHF_VGX_BACKEND "null" CACHE STRING "vgx back end of the harness: null or gl-stub")
if(ARMHF_VGX_BACKEND STREQUAL "gl-stub")
  list(FILTER VGX_SRCS EXCLUDE REGEX "/vgx_backend_null\\.cpp$")
elseif(ARMHF_VGX_BACKEND STREQUAL "null")
  list(FILTER VGX_SRCS EXCLUDE REGEX "/vgx_backend_gl\\.cpp$")
else()
  message(FATAL_ERROR "ARMHF_VGX_BACKEND must be null or gl-stub")
endif()
file(GLOB HARNESS_SRCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/glue/*.cpp")
list(FILTER HARNESS_SRCS EXCLUDE REGEX "/vgl_stub\\.cpp$")      # (only with the Vita-flavoured stub back end: below)
# no headset on a handheld: the port_vr_* stubs the Vita build links (vita/runtime/flat_view.cpp)
list(APPEND HARNESS_SRCS "${ROOT}/vita/runtime/flat_view.cpp")

# ---- flags ------------------------------------------------------------------------------------------------
# Vita: clang-vita-toolchain.cmake vita_flags (minus -D__vita__/newlib/-nostdlibinc, see below) + vita/CMakeLists.txt.
set(ARCH_FLAGS -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fno-short-enums -femulated-tls -fno-pic -munaligned-access)
set(GAME_FLAGS
  -w -O2 -fshort-wchar -fno-builtin-wcslen -fsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=on -fno-fast-math -fno-math-errno -fms-extensions -ferror-limit=0
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
  PORT_GX_VGX=1 PORT_GX_VGX_ONLY=1 PORT_WORKLOAD_GAME_BRIDGES=1 _GNU_SOURCE=1)
target_compile_options(platform PRIVATE ${ARCH_FLAGS} ${GAME_FLAGS} $<$<COMPILE_LANGUAGE:C>:${PORT_C_FLAGS}>)

# vgx front end (parser, vertex/texture decode, command buffers): same code generation, but not the game's compat.h.
add_library(vgx OBJECT ${VGX_SRCS})
target_include_directories(vgx PRIVATE "${ROOT}/platform/vgx/src" "${ROOT}/platform/vgx/third_party")
target_compile_definitions(vgx PRIVATE NDEBUG=1 PORT_ARMHF_HARNESS=1)
target_compile_options(vgx PRIVATE ${ARCH_FLAGS} -w -O2 -fno-exceptions -fno-rtti)
if(ARMHF_VGX_BACKEND STREQUAL "gl-stub")
  target_include_directories(vgx PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/glstub")
endif()
# The renderer as the Vita build's runtime stage compiles it (tools/vgx_build/vita_runtime_gen_vgx.py, build.py): -O3 for the
# vgx units, the player build's quiet flags and the draw profile of the console, the game units' code-generation flags.
# Off by default (the default harness is unchanged); the renderer's profile is recorded and measured with it on, so that
# the control flow the profile is keyed on is the Vita build's.
option(ARMHF_VITA_RUNTIME_FLAGS "compile the platform layer and the vgx units with the Vita runtime stage's flags" OFF)
if(ARMHF_VITA_RUNTIME_FLAGS)
  set(RUNTIME_DEFS PORT_RUNTIME_DIAGNOSTICS=0 PORT_LOGGING=1 GXV_ABSIDX_DEFAULT=0 GXV_STRICT_DRAW_PROFILE=1 NOMINMAX)
  target_compile_definitions(vgx PRIVATE ${RUNTIME_DEFS})
  target_compile_definitions(platform PRIVATE ${RUNTIME_DEFS})
  target_compile_options(vgx PRIVATE -O3 -fshort-wchar -fno-strict-aliasing -fwrapv -ffp-contract=on -fno-fast-math
    -fno-math-errno -fms-extensions -Wno-unused-function)
  # A profile is keyed on each function's control flow as Clang sees it before inlining, and the renderer's own functions inline
  # many std:: templates (vector, unordered_map, string): the Vita SDK's libstdc++ (GCC 15) and the cross GCC's (13) have
  # different ones, so about a fifth of the renderer's functions would not match.  With the Vita SDK's library headers (and the
  # cross GCC's target-specific bits/ files: newlib's ctype and gthr do not fit glibc) they match.  Only the renderer's units
  # are compiled this way; the library they link is still the cross GCC's (the templates are header code).
  set(ARMHF_VITA_CXXINC "" CACHE PATH "the Vita SDK's libstdc++ headers, .../arm-vita-eabi/include/c++/<version> (default: from VITASDK)")
  if(NOT ARMHF_VITA_CXXINC AND DEFINED ENV{VITASDK})
    file(GLOB _vita_cxx "$ENV{VITASDK}/arm-vita-eabi/include/c++/*")
    list(SORT _vita_cxx)
    if(_vita_cxx)
      list(GET _vita_cxx -1 _vita_cxx_dir)
      set(ARMHF_VITA_CXXINC "${_vita_cxx_dir}" CACHE PATH "" FORCE)
    endif()
  endif()
  file(GLOB _cross_bits "/usr/arm-linux-gnueabihf/include/c++/*/arm-linux-gnueabihf/bits")
  if(ARMHF_VGX_BACKEND STREQUAL "gl-stub")
    # The back end as the console build compiles it (its Vita-only branches are what a profile of the console must describe);
    # the same define for the front-end units that read it.  vgx_autoq.cpp has a gxm/GPU-fence part that needs the Vita SDK: not it.
    set_source_files_properties("${ROOT}/platform/vgx/src/vgx_backend_gl.cpp" "${ROOT}/platform/vgx/src/vgx_draw.cpp"
      "${ROOT}/platform/vgx/src/vgx_fesplit.cpp" "${ROOT}/platform/vgx/src/vgx_hdtex.cpp" PROPERTIES COMPILE_DEFINITIONS __VGX_VITA__=1)
    target_sources(platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/glue/vgl_stub.cpp")
  endif()
  if(ARMHF_VITA_CXXINC AND _cross_bits)
    list(GET _cross_bits -1 _cross_bits_dir)
    set(CXX_OVERLAY "${CMAKE_BINARY_DIR}/libstdcxx-overlay")
    file(MAKE_DIRECTORY "${CXX_OVERLAY}/bits")
    # (not the gthr*.h: the Vita's keep std::mutex behind the weak-pthread probe, which is control flow a profile is keyed on)
    foreach(f IN ITEMS ctype_base.h ctype_inline.h c++locale.h c++io.h messages_members.h time_members.h basic_file.h
        atomic_word.h cxxabi_tweaks.h error_constants.h os_defines.h cpu_defines.h opt_random.h c++allocator.h)
      file(COPY "${_cross_bits_dir}/${f}" DESTINATION "${CXX_OVERLAY}/bits")
    endforeach()
    # glibc 2.34+ pthreads need no probe, so the cross GCC's os_defines.h turns the weak-symbol probe of std::mutex off; the Vita
    # library keeps it (an extra branch in every lock and unlock): the harness keeps it too, the program links it as always true.
    file(READ "${CXX_OVERLAY}/bits/os_defines.h" _os_defines)
    string(REPLACE "define _GLIBCXX_GTHREAD_USE_WEAK 0" "define _GLIBCXX_GTHREAD_USE_WEAK_IS_THE_VITAS_DEFAULT 0" _os_defines "${_os_defines}")
    file(WRITE "${CXX_OVERLAY}/bits/os_defines.h" "${_os_defines}")
    set(VITA_CXX_FLAGS "SHELL:-nostdinc++" "SHELL:-isystem ${CXX_OVERLAY}" "SHELL:-isystem ${ARMHF_VITA_CXXINC}"
        "SHELL:-isystem ${ARMHF_VITA_CXXINC}/arm-vita-eabi" "SHELL:-isystem ${ARMHF_VITA_CXXINC}/backward")
    target_compile_options(vgx PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:${VITA_CXX_FLAGS}>")
    message(STATUS "renderer units use the Vita SDK's libstdc++ headers: ${ARMHF_VITA_CXXINC}")
  else()
    message(WARNING "ARMHF_VITA_RUNTIME_FLAGS: no Vita SDK libstdc++ headers (set VITASDK or ARMHF_VITA_CXXINC): "
                    "a profile of the renderer will match the Vita build less well")
  endif()
endif()
# Analysis only: -DARMHF_VGX_G=ON adds line tables to the vgx objects (same code, so the profiled binary's addresses still apply: scripts/arm-lines.py, built by scripts/arm-build-g.sh).
option(ARMHF_VGX_G "line tables for the vgx objects" OFF)
if(ARMHF_VGX_G)
  target_compile_options(vgx PRIVATE -g1)
endif()

# ---- profile-guided optimisation of the game units (tools/pgo/README.md) -----------------------------------
# ARMHF_PGO_GEN       instrument the game units (clang IR profile, -fprofile-generate); the run writes a .profraw at exit
#                     (armhf/pgo/pgo_exit.cpp flushes it from _exit, which the runner ends with).  Needs ARMHF_PROFILE_RT.
# ARMHF_PROFILE_RT    libclang_rt.profile for armhf (tools/pgo/build_profile_rt.sh builds it from the LLVM 18 sources)
# ARMHF_PGO_USE       compile the game units with this merged .profdata (-fprofile-use), as the Vita build does
# ARMHF_DECOMP_BITCODE  game units as ThinLTO bitcode, linked by `ld.lld -r --lto-O2` into one object exactly like
#                     tools/vgx_build/build.py (the Vita release form), then linked with the platform layer
# All four default off: the default harness build is unchanged.
option(ARMHF_PGO_GEN "instrument the game units for a profile run" OFF)
set(ARMHF_PROFILE_RT "" CACHE FILEPATH "libclang_rt.profile-armhf.a (ARMHF_PGO_GEN only)")
set(ARMHF_PGO_USE "" CACHE FILEPATH "merged .profdata the game units are compiled with")
option(ARMHF_DECOMP_BITCODE "game units as ThinLTO bitcode in one lld-linked object" OFF)
set(ARMHF_LTO_JOBS 2 CACHE STRING "ThinLTO backend jobs for ARMHF_DECOMP_BITCODE")
if(ARMHF_PGO_GEN AND ARMHF_PGO_USE)
  message(FATAL_ERROR "ARMHF_PGO_GEN and ARMHF_PGO_USE are exclusive")
endif()
if(ARMHF_PGO_GEN)
  if(NOT EXISTS "${ARMHF_PROFILE_RT}")
    message(FATAL_ERROR "ARMHF_PGO_GEN needs ARMHF_PROFILE_RT=<libclang_rt.profile-armhf.a> (tools/pgo/build_profile_rt.sh)")
  endif()
  target_compile_options(decomp PRIVATE -fprofile-generate)
  target_sources(platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pgo/pgo_exit.cpp")
endif()
if(ARMHF_PGO_USE)
  if(NOT EXISTS "${ARMHF_PGO_USE}")
    message(FATAL_ERROR "ARMHF_PGO_USE: no such file ${ARMHF_PGO_USE}")
  endif()
  target_compile_options(decomp PRIVATE "-fprofile-use=${ARMHF_PGO_USE}")
endif()
# The renderer: the vgx units and platform/src/gx (the GX front end's glue; the runtime stage of tools/vgx_build/build.py
# builds the same files), profiled with the same Clang IR instrumentation.
#   ARMHF_PGO_GEN_RENDERER   with ARMHF_PGO_GEN: instrument them too (the .profraw then holds both)
#   ARMHF_PGO_USE_RENDERER   compile them with this merged .profdata (-fprofile-use)
# ARMHF_PGO_GEN_RENDERER wants ARMHF_VITA_RUNTIME_FLAGS=ON and ARMHF_VGX_BACKEND=gl-stub for a profile that is meant for the console.
option(ARMHF_PGO_GEN_RENDERER "instrument the vgx and platform/src/gx units (with ARMHF_PGO_GEN)" OFF)
set(ARMHF_PGO_USE_RENDERER "" CACHE FILEPATH "merged .profdata the vgx and platform/src/gx units are compiled with")
set(PLATFORM_GX_SRCS ${PLATFORM_SRCS})
list(FILTER PLATFORM_GX_SRCS INCLUDE REGEX "/platform/src/gx/[A-Za-z0-9_]+\\.cpp$")
if(ARMHF_VITA_RUNTIME_FLAGS AND VITA_CXX_FLAGS)
  # (a source property takes no "SHELL:" groups: the joined spelling of the same options)
  set_property(SOURCE ${PLATFORM_GX_SRCS} APPEND PROPERTY COMPILE_OPTIONS -nostdinc++ "-isystem${CXX_OVERLAY}"
    "-isystem${ARMHF_VITA_CXXINC}" "-isystem${ARMHF_VITA_CXXINC}/arm-vita-eabi" "-isystem${ARMHF_VITA_CXXINC}/backward")
endif()
if(ARMHF_PGO_GEN_RENDERER)
  if(NOT ARMHF_PGO_GEN)
    message(FATAL_ERROR "ARMHF_PGO_GEN_RENDERER needs ARMHF_PGO_GEN")
  endif()
  target_compile_options(vgx PRIVATE -fprofile-generate)
  set_property(SOURCE ${PLATFORM_GX_SRCS} APPEND PROPERTY COMPILE_OPTIONS -fprofile-generate)
endif()
if(ARMHF_PGO_USE_RENDERER)
  if(NOT EXISTS "${ARMHF_PGO_USE_RENDERER}")
    message(FATAL_ERROR "ARMHF_PGO_USE_RENDERER: no such file ${ARMHF_PGO_USE_RENDERER}")
  endif()
  target_compile_options(vgx PRIVATE "-fprofile-use=${ARMHF_PGO_USE_RENDERER}")
  set_property(SOURCE ${PLATFORM_GX_SRCS} APPEND PROPERTY COMPILE_OPTIONS "-fprofile-use=${ARMHF_PGO_USE_RENDERER}")
endif()
set(GAME_UNITS $<TARGET_OBJECTS:decomp>)
if(ARMHF_DECOMP_BITCODE)
  target_compile_options(decomp PRIVATE -flto=thin)
  set(LTO_OBJ "${CMAKE_BINARY_DIR}/galaxy_lto.o")
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/decomp_objects.rsp" CONTENT "$<JOIN:$<TARGET_OBJECTS:decomp>,\n>\n")
  add_custom_command(OUTPUT "${LTO_OBJ}"
    COMMAND ld.lld -r -o "${LTO_OBJ}" --thinlto-jobs=${ARMHF_LTO_JOBS} --lto-O2 -mllvm -emulated-tls "@${CMAKE_BINARY_DIR}/decomp_objects.rsp"
    DEPENDS $<TARGET_OBJECTS:decomp> "${CMAKE_BINARY_DIR}/decomp_objects.rsp"
    COMMENT "ThinLTO link of the game units" VERBATIM)
  set_source_files_properties("${LTO_OBJ}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  set(GAME_UNITS "${LTO_OBJ}")
endif()

add_executable(game ${GAME_UNITS} $<TARGET_OBJECTS:platform> $<TARGET_OBJECTS:vgx>)
set_target_properties(game PROPERTIES LINKER_LANGUAGE CXX)
# Non-PIE at 0x80000000, like the Vita's fixed image address range (>= 0x40000000: os_mem.cpp requires it).
target_link_options(game PRIVATE -static-libgcc -no-pie -Wl,-Ttext-segment=0x80000000 -Wl,--no-undefined
  -Wl,-Map,${CMAKE_BINARY_DIR}/game.map)
target_link_libraries(game PRIVATE pthread m)
if(ARMHF_PGO_GEN)
  # (the clang driver adds this when it links; the GNU driver used here does not)
  target_link_options(game PRIVATE -Wl,-u,__llvm_profile_runtime)
  target_link_libraries(game PRIVATE "${ARMHF_PROFILE_RT}")
endif()
