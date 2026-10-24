cmake_minimum_required(VERSION 3.16)
project(galaxy_native_game LANGUAGES CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
get_filename_component(root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
foreach(archive IN ITEMS RUNTIME_ARCHIVE GAME_ARCHIVE)
    if(NOT EXISTS "${${archive}}")
        message(FATAL_ERROR "Current verified ${archive} is required")
    endif()
endforeach()
add_executable(galaxy_native main.cpp entry.cpp)
target_include_directories(galaxy_native PRIVATE "${root}/platform/include" "${root}/platform/src/gx")
target_compile_features(galaxy_native PRIVATE cxx_std_20)
target_compile_options(galaxy_native PRIVATE -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb
    -fsigned-char -fshort-wchar -fno-fast-math -ffunction-sections -fdata-sections)
set_source_files_properties(entry.cpp PROPERTIES COMPILE_OPTIONS "-fno-short-enums")
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS "-fshort-enums")
# The SDK converter appends SCE metadata to RX before the next load segment.
target_link_options(galaxy_native PRIVATE -Wl,--gc-sections -Wl,--no-undefined -Wl,-Map,galaxy_native.map
    -Wl,--defsym=__sce_headroom=0x1000)
target_link_libraries(galaxy_native PRIVATE -Wl,--start-group "${RUNTIME_ARCHIVE}"
    -Wl,--whole-archive "${GAME_ARCHIVE}" pthread -Wl,--no-whole-archive -Wl,--end-group
    SceGxm_stub SceDisplay_stub SceCtrl_stub SceMotion_stub SceTouch_stub SceAudio_stub
    SceKernelThreadMgr_stub SceSysmem_stub SceIofilemgr_stub SceProcessmgr_stub m)
vita_create_self(eboot.bin galaxy_native)
vita_create_vpk(galaxy-native-development.vpk CDXG00002 eboot.bin VERSION 00.01 NAME "Galaxy Native Development")
