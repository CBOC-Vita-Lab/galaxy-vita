cmake_minimum_required(VERSION 3.16)
project(galaxy_vgx_game LANGUAGES CXX)
# Link stage of the vgx build (scripts/build-vita-vgx.sh link): the runtime archive (platform + vgx +
# the Vita host), the game archive and vitaGL with its friends. Title SMGV00001, data in ux0:data/smg.
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
foreach(archive IN ITEMS RUNTIME_ARCHIVE GAME_ARCHIVE)
    if(NOT EXISTS "${${archive}}")
        message(FATAL_ERROR "${archive} is required")
    endif()
endforeach()
if(NOT DEFINED VGL_DEPS OR NOT EXISTS "${VGL_DEPS}/lib/libvitaGL.a")
    message(FATAL_ERROR "VGL_DEPS must name the folder with lib/libvitaGL.a (strikers performance-batch-deps-01)")
endif()
set(VGL_SHADER_READER_ARCHIVE "" CACHE FILEPATH "Opt-in locally rebuilt shader-read-callback archive")
set(vgl_archive "${VGL_DEPS}/lib/libvitaGL.a")
if(VGL_SHADER_READER_ARCHIVE)
    if(NOT EXISTS "${VGL_SHADER_READER_ARCHIVE}")
        message(FATAL_ERROR "VGL_SHADER_READER_ARCHIVE does not exist")
    endif()
    set(vgl_archive "${VGL_SHADER_READER_ARCHIVE}")
endif()
set(SMG_TITLE_ID "SMGV00001" CACHE STRING "Title ID")
add_executable(galaxy_native main.cpp entry.cpp settings_runtime.cpp fd_guard.cpp)
target_link_options(galaxy_native PRIVATE "-Wl,--wrap=_lseek_r")
if(VGL_SHADER_READER_ARCHIVE)
    # Both sides must be compiled in; an unchanged driver/runtime fails the link.
    target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=port_vgx_shader_cache_reader_driver_v1"
        "-Wl,--require-defined=vgxvita_shaderCacheRead_v1")
endif()
option(PORT_SILENT_STDIO "Discard library console diagnostics before formatting" OFF)
if(PORT_SILENT_STDIO)
    target_sources(galaxy_native PRIVATE silent_stdio.cpp)
    # (the wrappers below discard console output; the port's own log file is decided by PORT_LOGGING, which the player build
    # keeps on for its basic log)
    target_compile_definitions(galaxy_native PRIVATE PORT_SILENT_STDIO_WRAP=1)
    foreach(symbol IN ITEMS printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite)
        target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endif()
option(VGX_COLDTRACE_WRAPPERS "Observe cold shader/cache/GPU calls in RAM" OFF)
set(vgx_wrapper_trace 0)
if(VGX_COLDTRACE_WRAPPERS)
    set(vgx_wrapper_trace 1)
endif()
target_sources(galaxy_native PRIVATE coldtrace_wrap.cpp)
set_source_files_properties(coldtrace_wrap.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${VGL_DEPS}/include"
    COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti;-UVGX_COLDTRACE;-DVGX_COLDTRACE=${vgx_wrapper_trace}")
target_link_options(galaxy_native PRIVATE "-Wl,--wrap=sceIoOpen"
    "-Wl,--require-defined=port_vgx_shader_bucket_open_guard_v1"
    "-Wl,--require-defined=vgxvita_prepareShaderCacheWrite_v1"
    "-Wl,--require-defined=vgl_shader_cache_path")
if(VGX_COLDTRACE_WRAPPERS)
    target_sources(galaxy_native PRIVATE coldtrace_export.cpp)
    set_source_files_properties(coldtrace_export.cpp PROPERTIES
        COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti;-UVGX_COLDTRACE;-DVGX_COLDTRACE=1")
    # Reject a stale or probe-off runtime even when --gc-sections discards unused functions.
    target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=port_vgx_coldtrace_runtime_link_guard_v3"
        "-Wl,--require-defined=port_vgx_coldtrace_wrapper_runtime_link_guard_v3"
        "-Wl,--require-defined=port_vgx_coldtrace_export_runtime_link_guard_v3")
    target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=port_vgx_coldtrace_pause_runtime_link_guard_v1"
        "-Wl,--require-defined=port_vgx_coldtrace_boundary_runtime_link_guard_v3")
    foreach(symbol IN ITEMS sceIoMkdir sceIoLseek sceIoRead sceIoPread sceIoWrite sceIoClose
            shark_compile_shader_extended shark_compile_shader
            sceGxmShaderPatcherRegisterProgram sceGxmShaderPatcherCreateVertexProgram
            sceGxmShaderPatcherCreateFragmentProgram sceGxmBeginScene sceGxmEndScene sceGxmFinish sceGxmNotificationWait sceGxmTransferFinish)
        target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endif()
target_include_directories(galaxy_native PRIVATE "${root}/platform/include" "${root}/platform/src/gx")
target_include_directories(galaxy_native PRIVATE "${root}/platform/vgx/src" "${root}/vita/runtime")
target_compile_features(galaxy_native PRIVATE cxx_std_20)
target_compile_options(galaxy_native PRIVATE -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
    -fsigned-char -fshort-wchar -fno-fast-math -ffunction-sections -fdata-sections)
set_source_files_properties(entry.cpp settings_runtime.cpp PROPERTIES COMPILE_OPTIONS "-fno-short-enums")
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-fshort-enums")
# The SDK converter appends SCE metadata to RX before the next load segment.
target_link_options(galaxy_native PRIVATE -Wl,--gc-sections -Wl,--no-undefined -Wl,-Map,galaxy_native.map
    -Wl,--defsym=__sce_headroom=0x1000
    # (main.cpp: the pthread library's polling wait replaced by a blocking one)
    -Wl,--wrap=pte_osSemaphoreCancellablePend)
# -pthread, NOT -lpthread: VitaSDK's spec turns it into --whole-archive -lpthread (libstdc++ finds its
# thread functions through weak references; see the Strikers Makefile.vita).
target_link_libraries(galaxy_native PRIVATE -Wl,--start-group "${RUNTIME_ARCHIVE}"
    -Wl,--whole-archive "${GAME_ARCHIVE}" -Wl,--no-whole-archive -Wl,--end-group
    "${vgl_archive}" "${VGL_DEPS}/lib/libvitashark.a" "${VGL_DEPS}/lib/libSceShaccCgExt.a"
    "${VGL_DEPS}/lib/libmathneon.a" "${VGL_DEPS}/lib/libtaihen_stub.a"
    -pthread
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceMotion_stub SceTouch_stub SceAudio_stub
    SceKernelThreadMgr_stub SceSysmem_stub SceIofilemgr_stub SceProcessmgr_stub
    SceShaccCg_stub SceCommonDialog_stub SceKernelDmacMgr_stub SceAppUtil_stub SceAppMgr_stub SceLibKernel_stub
    SceRtc_stub ScePower_stub m)
vita_create_self(eboot.bin galaxy_native)
# ATTRIBUTE2=12: the extended (about 365 MiB) memory budget, as the Strikers package sets it.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
# Bubble and LiveArea are original drawings; the preserved title background is documented in docs/ASSETS.md.
set(sys "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys")
include("${CMAKE_CURRENT_LIST_DIR}/loading_artwork.cmake")
vita_create_vpk(smg-vgx.vpk ${SMG_TITLE_ID} eboot.bin VERSION 01.00 NAME "Super Mario Galaxy"
    ${smg_loading_picture}
    FILE "${sys}/icon0.png" sce_sys/icon0.png
    FILE "${sys}/pic0.png" sce_sys/pic0.png
    FILE "${sys}/livearea/contents/bg.png" sce_sys/livearea/contents/bg.png
    FILE "${sys}/livearea/contents/startup.png" sce_sys/livearea/contents/startup.png
    FILE "${sys}/livearea/contents/settings.png" sce_sys/livearea/contents/settings.png
    FILE "${sys}/livearea/contents/template.xml" sce_sys/livearea/contents/template.xml)
