cmake_minimum_required(VERSION 3.16)
project(galaxy_vgx_game LANGUAGES C CXX)   # (C: MP3 net/ monocypher.c)
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
add_executable(galaxy_native main.cpp entry.cpp settings_runtime.cpp title_art_runtime.cpp fd_guard.cpp)
target_link_options(galaxy_native PRIVATE "-Wl,--wrap=_lseek_r" "-Wl,--wrap=fopen" "-Wl,--wrap=freopen" "-Wl,--wrap=fclose")
# Profile recording on the console (environment VITA_PGO_GENERATE=1, as vita/CMakeLists.txt; tools/pgo/README.md): the instrumented game and
# renderer need clang's profile runtime, which has no Vita platform. vita/pgo/ has the upstream writer sources (vendored, Apache-2.0 with the LLVM
# exception, licenses/), the platform layer on the linker's __start_/__stop_ section symbols and the dump thread (vita_pgo.cpp); entry.cpp starts
# it (GXV_PGO_GENERATE) and dumps at exit.  Without the variable none of this is compiled and the link line is the one it was.
if("$ENV{VITA_PGO_GENERATE}" STREQUAL "1")
    enable_language(C)
    set(pgo_dir "${root}/vita/pgo")
    set(pgo_rt "${pgo_dir}/compiler-rt/profile")
    set(pgo_c_sources "${pgo_dir}/vita_pgo_core.c" "${pgo_dir}/vita_pgo_platform.c"
        "${pgo_rt}/InstrProfiling.c" "${pgo_rt}/InstrProfilingBuffer.c" "${pgo_rt}/InstrProfilingInternal.c"
        "${pgo_rt}/InstrProfilingValue.c" "${pgo_rt}/InstrProfilingWriter.c")
    # (not InstrProfilingVersionVar.c / NameVar.c: their weak __llvm_profile_raw_version would come first in the link and hide the
    # instrumented units' own, the one that marks the profile as an IR profile; every instrumented unit defines both variables)
    target_sources(galaxy_native PRIVATE ${pgo_c_sources} "${pgo_dir}/vita_pgo.cpp")
    # (the runtime's own files are C: no short enums or wide characters in them, hidden symbols, GCC's __sync atomics)
    set_source_files_properties(${pgo_c_sources} PROPERTIES COMPILE_OPTIONS "-fno-short-enums;-fvisibility=hidden;-DCOMPILER_RT_HAS_ATOMICS=1"
        INCLUDE_DIRECTORIES "${pgo_dir};${pgo_dir}/compiler-rt/include;${pgo_rt}")
    set_source_files_properties("${pgo_dir}/vita_pgo.cpp" PROPERTIES COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti"
        INCLUDE_DIRECTORIES "${pgo_dir};${root}/platform/include")
    target_compile_definitions(galaxy_native PRIVATE GXV_PGO_GENERATE=1)
    message(STATUS "VITA_PGO_GENERATE=1: the eboot writes the profile counters to ux0:data/smg/pgo.profraw")
endif()
# tls40: fast paths for __getreent and pte_osTlsGetValue (errno, _REENT, pthread_getspecific/pthread_self, thread_local's __emutls_get_address),
# vita/vgx-game/fast_tls.c. Build switch: FAST_TLS=0 in the environment of the link (or -DFAST_TLS=OFF) builds without it; at run time
# GXV_FAST_TLS=0 turns it off. The wraps are inert until port_fast_tls_init (main) has checked the TLS layout on the console.
if(NOT "$ENV{FAST_TLS}" STREQUAL "0" AND NOT DEFINED FAST_TLS_OFF)
    target_sources(galaxy_native PRIVATE fast_tls.c)
    target_link_options(galaxy_native PRIVATE "-Wl,--wrap=__getreent" "-Wl,--wrap=pte_osTlsGetValue")
endif()
# (2.3: abort() logs a FATAL line and stops with a core dump instead of a silent exit; entry.cpp)
target_link_options(galaxy_native PRIVATE "-Wl,--wrap=abort" "-Wl,--wrap=sceKernelExitProcess")
if(VGL_SHADER_READER_ARCHIVE)
    # Both sides must be compiled in; an unchanged driver/runtime fails the link.
    target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=port_vgx_shader_cache_reader_driver_v1"
        "-Wl,--require-defined=vgxvita_shaderCacheRead_v1")
endif()
# GXV_FE_TWOCORE: the front end's second instance (platform/vgx/src/vgx_twocore_b*.cpp) is only referenced weakly (vgx_twocore_impl.h),
# and a weak reference does not pull a member out of the runtime archive: builds up to c41 linked without it and logged "fetwo: this
# build has no second instance". Requiring its entry pulls vgx_twocore_b.cpp, whose strong references pull the rest.
target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=vgx2_b_thread_main" "-Wl,--require-defined=vgx2_b_picture_state"
    "-Wl,--require-defined=vgx2_b_state_objs")
# MP3 (vitalink): objects of the runtime archive that are only REFERENCED WEAKLY by the rest of the port (host tests link without them), so a
# weak reference never pulls the archive member: the Vita build linked without them and the multiplayer silently did not exist (no boot hook, no
# menu data store, no connection test, no update notice; found by tools/vita_compile_check.sh --link). One symbol per object is required here;
# the member then comes in with everything it defines. tools/vita_must_define.txt lists the same symbols (checked after the link, see below).
target_link_options(galaxy_native PRIVATE
    "-Wl,--require-defined=mp_port_boot"          # platform/src/mp/mp_boot.cpp (boot.cpp, vgx_backend_gl.cpp, settings_runtime.cpp call it weakly)
    "-Wl,--require-defined=mp_ui_net_info"        # platform/src/mp/mp_ui_flow.cpp (Game/MP/MpMenu*.cpp call it weakly)
    "-Wl,--require-defined=mp_conntest_start"     # platform/src/mp/mp_conntest_sys.cpp (MpMenuConnTest.cpp)
    "-Wl,--require-defined=mp_aids_boot"          # platform/src/mp/mp_hwaids.cpp (mp_boot.cpp, mp_log.cpp, mp_core.cpp, MpPuppets.cpp, os_alarm.cpp call it weakly)
    "-Wl,--require-defined=mp_update_take_notice" # platform/src/mp/mp_update.cpp (MpMenuSequence.cpp, mp_service.cpp, np_signal_http.cpp)
    "-Wl,--require-defined=mp_diag_report_begin"  # platform/src/mp/mp_diag.cpp: beta diagnostics (MpMenuReport.cpp, mp_core.cpp, MpPuppets.cpp call it weakly)
    "-Wl,--require-defined=mp_diag_check_begin")  # platform/src/mp/mp_netcheck.cpp: Check Connection (MpMenuReport.cpp)
# linkfix31: the native Settings' live setters and the pause panel's "Settings can open here" answer (platform/src/port/settings_live.cpp)
# are referenced only weakly (perf.cpp's port_settings_hint_visible, settings_native.cpp's NativeLiveOps), so integ-3.1's first Vita build
# linked without the object: no "Triangle: Settings" hint, Triangle opened nothing, and Less Motion / Tilt / Tilt Strength rows had no setter.
# Every symbol is required (not only one), so a setter that moves to another file still fails the link instead of silently going weak.
target_link_options(galaxy_native PRIVATE
    "-Wl,--require-defined=port_settings_native_available"
    "-Wl,--require-defined=port_less_motion_enabled" "-Wl,--require-defined=port_less_motion_set"
    "-Wl,--require-defined=port_tilt_stick_enabled" "-Wl,--require-defined=port_tilt_stick_set"
    "-Wl,--require-defined=port_tilt_level" "-Wl,--require-defined=port_tilt_level_set"
    # (c186merge) C186.1's Settings link also required these two at the link itself (both are in tools/vita_must_define.txt as well):
    "-Wl,--require-defined=port_settings_hint_visible"      # platform/src/port/perf.cpp: the pause panel's hint
    "-Wl,--require-defined=port_settings_native_request"    # decomp/src/Game/MP/MpUiSystem.cpp: Triangle opens the native Settings
    # freecam31: keep weak-referenced Free Camera hooks & ThinLTO-inlined reset bridges alive across --gc-sections
    "-Wl,--require-defined=port_free_camera_enabled"
    "-Wl,--require-defined=port_right_stick_mode" "-Wl,--require-defined=port_right_stick_mode_set"
    "-Wl,--require-defined=port_camera_speed" "-Wl,--require-defined=port_camera_speed_set"
    "-Wl,--require-defined=port_input_set_camera_invert_mask"
    "-Wl,--require-defined=port_free_camera_owns_reset"
    "-Wl,--require-defined=port_free_camera_take_parallel_reset"
    "-Wl,--require-defined=port_free_camera_active"
    "-Wl,--require-defined=port_free_camera_turn_blend"
    "-Wl,--require-defined=port_free_camera_clip_pad")
# Every symbol above, and the multiplayer / SceNet ones, are also listed in tools/vita_must_define.txt: tools/vgx_build/build.py's link stage
# runs tools/check_weak_links.py on the ELF after the link (each must be T, and no weak-only reference may name code a link input has).
# The Wi-Fi watch of mp_core_vita.cpp references four SceNet / SceNetCtl calls weakly (null-checked); their stubs are on the link line, but a weak
# reference does not pull a stub member out of the archive, so the callbacks and the free-memory reading were silently off. Require them: the
# members are then linked and the console imports them from the system modules (all four exist in SceNet / SceNetCtl).
target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=sceNetCtlInetRegisterCallback" "-Wl,--require-defined=sceNetCtlInetUnregisterCallback"
    "-Wl,--require-defined=sceNetCtlCheckCallback" "-Wl,--require-defined=sceNetGetStatisticsInfo")
option(PORT_SILENT_STDIO "Discard library console diagnostics before formatting" OFF)
if(PORT_SILENT_STDIO)
    target_sources(galaxy_native PRIVATE silent_stdio.cpp)
    # (the wrappers below discard console output; the port's own log file is decided by PORT_LOGGING, which the player build
    # keeps on for its basic log)
    target_compile_definitions(galaxy_native PRIVATE PORT_SILENT_STDIO_WRAP=1)
    foreach(symbol IN ITEMS printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite fflush)
        target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
else()
    # stdio31: the diagnostic build writes stdout / stderr from many threads, and the SDK's C library has no FILE locks (two threads printing
    # at once corrupted the heap on the Strikers console). stdio_safe.cpp sends every console call of the link through one lock and the log
    # writer thread (platform/include/port/console_stdio.h; GXV_STDIO_SAFE=0 in env.txt restores the unlocked calls). Buffering is unchanged.
    target_sources(galaxy_native PRIVATE stdio_safe.cpp)
    foreach(symbol IN ITEMS printf vprintf fprintf vfprintf puts putchar fputs fputc putc fwrite fflush)
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
# (stall23, GXV_STUTTER_WAITS) the stutter lines' shader compiler runs and the render thread's GPU waits: wait_wrap.cpp wraps the four calls
# below. The cold-trace build wraps the same calls in coldtrace_wrap.cpp, so there the counts are left out (the back end then prints none).
if(NOT VGX_COLDTRACE_WRAPPERS)
    target_sources(galaxy_native PRIVATE wait_wrap.cpp)
    set_source_files_properties(wait_wrap.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${VGL_DEPS}/include"
        COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti")
    foreach(symbol IN ITEMS shark_compile_shader_extended sceGxmFinish sceGxmBeginScene sceGxmNotificationWait
        sceGxmEndScene sceGxmDisplayQueueAddEntry sceGxmReserveVertexDefaultUniformBuffer sceGxmReserveFragmentDefaultUniformBuffer
        sceGxmTransferFinish sceGxmDraw sceKernelWaitSema sceKernelDelayThread)
        target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
    endforeach()
endif()
# (asyncsh31) the shader worker (GXV_ASYNC_SHADERS) allocates from newlib's heap only, never from vitaGL's unlocked pools: alloc_wrap.cpp wraps vitaGL's four
# allocation entry points; on every thread but the worker's they are the originals (platform/vgx/src/vgx_worker_alloc.h). In every build flavour.
target_sources(galaxy_native PRIVATE alloc_wrap.cpp)
set_source_files_properties(alloc_wrap.cpp PROPERTIES COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti")
foreach(symbol IN ITEMS vglMalloc vglCalloc vglMemalign vglRealloc)
    target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
endforeach()
# (dma31, GXV_CLOCK_CENSUS, default 0) every clock read of the program, by thread and call site: clock_census.cpp wraps the system calls
# every clock goes through (the port's, newlib's, pthreads', vitaGL's). Off, each wrapper is one load and a branch in front of the call.
target_sources(galaxy_native PRIVATE clock_census.cpp)
set_source_files_properties(clock_census.cpp PROPERTIES COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti")
foreach(symbol IN ITEMS sceKernelGetProcessTimeWide sceKernelGetProcessTimeLow sceKernelGetThreadInfo sceKernelLibcGettimeofday sceRtcGetCurrentTick)
    target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
endforeach()
# (copy49, GXV_SMALLCOPY / GXV_COPY_CENSUS, default 0) newlib's memcpy is a tail jump to sceClibMemcpy (~190 ns a call), which vitaGL calls directly too:
# smallcopy_wrap.cpp wraps sceClibMemcpy (so every memcpy of the link), memmove and memset. Off, each wrapper is a load and a compare in front of the
# real function. The file is compiled -fno-builtin: its loops must never become a call to memcpy (the wrapper would call itself).
target_sources(galaxy_native PRIVATE smallcopy_wrap.cpp)
set_source_files_properties(smallcopy_wrap.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${root}/platform/include"
    COMPILE_OPTIONS "-fno-short-enums;-fno-exceptions;-fno-rtti;-fno-builtin;-fno-tree-loop-distribute-patterns")
foreach(symbol IN ITEMS sceClibMemcpy memmove memset sceKernelFreeMemBlock sceKernelAllocMemBlock)
    target_link_options(galaxy_native PRIVATE "-Wl,--wrap=${symbol}")
endforeach()
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
# MP3: net/ = the vendored Strikers Charged online stack (VLink: ad hoc radio, Same Wi-Fi UDP, online; NpNet: STUN, punching, TURN, session
# crypto, HTTPS signaling), game-agnostic, built here with the SDK's GCC and the Strikers flags (net/GALAXY-CHANGES.txt). SMG_MBEDTLS=<prefix of
# tools/mp/build-mbedtls.py --target vita> links the HTTPS client's TLS (only the np_https objects see it); empty = np_https without TLS (no online
# service over https). The library sits inside the --start-group below (the runtime calls into it, it calls the runtime's strong glue).
set(SMG_MBEDTLS "" CACHE PATH "mbedTLS prefix for net/ (include/, lib/libmbedtls.a ...); empty = no TLS")
set(net "${root}/net/src")
set(smg_net_sources
    "${net}/vlink.cpp" "${net}/vlink_udp.cpp" "${net}/vlink_vita.cpp" "${net}/vlink_vita_radio.cpp" "${net}/vlink_online.cpp"
    "${net}/np_glue.cpp" "${net}/np_keyboard_vita.cpp" "${net}/np_online_ime_vita.cpp"
    "${net}/np_https.cpp" "${net}/np_https_sys.cpp" "${net}/np_https_tls_mbedtls.cpp" "${net}/np_https_selftest.cpp"
    "${net}/online/np_net.cpp" "${net}/online/np_crypto.cpp" "${net}/online/np_hash.cpp" "${net}/online/np_turn.cpp"
    "${net}/online/np_stun.cpp" "${net}/online/np_peer.cpp" "${net}/online/np_online.cpp" "${net}/online/np_fault.cpp"
    "${net}/online/np_signal_fake.cpp" "${net}/online/np_signal_http.cpp" "${net}/online/np_http.cpp" "${net}/online/np_udp_vita.cpp"
    "${net}/online/np_upnp.cpp" "${net}/online/np_natpmp.cpp"
    "${net}/online/third_party/monocypher/monocypher.c")
add_library(smg_net STATIC ${smg_net_sources})
target_compile_definitions(smg_net PRIVATE NP_TARGET_VITA=1)
include("${root}/cmake/MpBuildDefs.cmake")   # core2: the same NP_ONLINE_URL / MP_BUILD_ID as the source stage (build.py passes both)
target_compile_definitions(smg_net PRIVATE ${MP_BUILD_DEFS})
target_compile_definitions(galaxy_native PRIVATE ${MP_BUILD_DEFS})
# The settings version the stale-settings guard stamps (platform/include/port/settings_guard.h) has no default of its own: it is the game's one app-version source, SMG_APP_VERSION in
# platform/src/mp/mp_service.cpp (the version the multiplayer service sees), read here. The Vita bubble version (VERSION in vita_create_vpk below) is a different thing and is not touched.
file(STRINGS "${root}/platform/src/mp/mp_service.cpp" _smg_ver_line REGEX "^#define SMG_APP_VERSION \"[0-9]+\\.[0-9]+\\.[0-9]+\"" LIMIT_COUNT 1)
if(NOT _smg_ver_line MATCHES "\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
    message(FATAL_ERROR "settings guard: cannot read SMG_APP_VERSION (x.y.z) from platform/src/mp/mp_service.cpp")
endif()
set(GALAXY_SETTINGS_VERSION "${CMAKE_MATCH_1}")
message(STATUS "settings guard: stamps version ${GALAXY_SETTINGS_VERSION} (from SMG_APP_VERSION in platform/src/mp/mp_service.cpp)")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/platform/src/mp/mp_service.cpp")
set_source_files_properties(settings_runtime.cpp PROPERTIES COMPILE_DEFINITIONS "GALAXY_SETTINGS_VERSION=\"${GALAXY_SETTINGS_VERSION}\"")
target_include_directories(smg_net PUBLIC "${root}/net/include")
target_compile_options(smg_net PRIVATE -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fno-short-enums
    -fno-strict-aliasing -fwrapv -ffunction-sections -fdata-sections $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20 -fno-exceptions -fno-rtti>
    $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -w>)
set(smg_mbedtls_libs)
if(SMG_MBEDTLS)
    foreach(lib IN ITEMS libmbedtls.a libmbedx509.a libmbedcrypto.a)
        if(NOT EXISTS "${SMG_MBEDTLS}/lib/${lib}")
            message(FATAL_ERROR "SMG_MBEDTLS=${SMG_MBEDTLS} has no lib/${lib} (python3 tools/mp/build-mbedtls.py --target vita --work /tls)")
        endif()
        list(APPEND smg_mbedtls_libs "${SMG_MBEDTLS}/lib/${lib}")   # (this order: tls, x509, crypto)
    endforeach()
    set_source_files_properties("${net}/np_https.cpp" "${net}/np_https_sys.cpp" "${net}/np_https_tls_mbedtls.cpp" "${net}/np_https_selftest.cpp"
        PROPERTIES COMPILE_DEFINITIONS "NP_HAVE_MBEDTLS=1" INCLUDE_DIRECTORIES "${SMG_MBEDTLS}/include")
    message(STATUS "net/: HTTPS with mbedTLS from ${SMG_MBEDTLS}")
else()
    message(STATUS "net/: no SMG_MBEDTLS: np_https without TLS (https:// requests answer UNAVAILABLE)")
endif()
target_include_directories(galaxy_native PRIVATE "${root}/platform/include" "${root}/platform/src/gx")
target_include_directories(galaxy_native PRIVATE "${root}/platform/vgx/src" "${root}/vita/runtime")
target_compile_features(galaxy_native PRIVATE cxx_std_20)
target_compile_options(galaxy_native PRIVATE -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
    -fsigned-char -fshort-wchar -fno-builtin-wcslen -fno-fast-math -ffunction-sections -fdata-sections)
set_source_files_properties(entry.cpp settings_runtime.cpp title_art_runtime.cpp PROPERTIES COMPILE_OPTIONS "-fno-short-enums")
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-fshort-enums")
# The SDK converter appends SCE metadata to RX before the next load segment.
target_link_options(galaxy_native PRIVATE -Wl,--gc-sections -Wl,--no-undefined -Wl,-Map,galaxy_native.map
    -Wl,--defsym=__sce_headroom=0x1000
    # (main.cpp: the pthread library's polling wait replaced by a blocking one)
    -Wl,--wrap=pte_osSemaphoreCancellablePend)
# -pthread, NOT -lpthread: VitaSDK's spec turns it into --whole-archive -lpthread (libstdc++ finds its
# thread functions through weak references; see the Strikers Makefile.vita).
# libstdc++'s __gthread_active_p() is the test "is pthread_cancel linked" (gthr-posix.h): without it every std::mutex and
# std::condition_variable silently does nothing (Strikers hit this). The link fails here rather than producing such a program.
target_link_options(galaxy_native PRIVATE "-Wl,--require-defined=pthread_cancel")
target_link_libraries(galaxy_native PRIVATE -Wl,--start-group "${RUNTIME_ARCHIVE}" smg_net
    -Wl,--whole-archive "${GAME_ARCHIVE}" -Wl,--no-whole-archive -Wl,--end-group ${smg_mbedtls_libs}
    "${vgl_archive}" "${VGL_DEPS}/lib/libvitashark.a" "${VGL_DEPS}/lib/libSceShaccCgExt.a"
    "${VGL_DEPS}/lib/libmathneon.a" "${VGL_DEPS}/lib/libtaihen_stub.a"
    -pthread
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceMotion_stub SceTouch_stub SceAudio_stub
    SceKernelThreadMgr_stub SceSysmem_stub SceIofilemgr_stub SceProcessmgr_stub
    SceShaccCg_stub SceCommonDialog_stub SceKernelDmacMgr_stub SceAppUtil_stub SceAppMgr_stub SceLibKernel_stub
    SceRtc_stub ScePower_stub SceJpeg_stub
    SceNet_stub SceNetCtl_stub SceSysmodule_stub ScePspnetAdhoc_stub_weak   # MP3: net/ (sceNet, Wi-Fi state, modules, ad hoc PDP)
    m)
vita_create_self(eboot.bin galaxy_native)
# ATTRIBUTE2=12: the extended (about 365 MiB) memory budget, as the Strikers package sets it.
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
# There is no LiveArea Settings tile (Settings live in the game); settings.png stays in the tree but is not packaged.
# The bubble and LiveArea pictures here are the five contributor PNGs (sce_sys/ARTWORK.md): contributor artwork that includes the game's
# key art and logo, shipped as supplied (owner, 17 Nov; never regenerated). The loading-screen pictures are made from the player's own
# title screen on the first start (title_art_runtime.cpp, docs/ASSETS.md).
set(sys "${CMAKE_CURRENT_SOURCE_DIR}/sce_sys")
vita_create_vpk(smg-vgx.vpk ${SMG_TITLE_ID} eboot.bin VERSION 03.01 NAME "Super Mario Galaxy"
    FILE "${sys}/icon0.png" sce_sys/icon0.png
    FILE "${sys}/pic0.png" sce_sys/pic0.png
    FILE "${sys}/livearea/contents/bg.png" sce_sys/livearea/contents/bg.png
    FILE "${sys}/livearea/contents/startup.png" sce_sys/livearea/contents/startup.png
    FILE "${sys}/livearea/contents/template.xml" sce_sys/livearea/contents/template.xml)
