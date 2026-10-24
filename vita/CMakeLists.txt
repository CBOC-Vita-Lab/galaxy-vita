cmake_minimum_required(VERSION 3.24)
project(GalaxyVitaSource C CXX)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_C_COMPILER_TARGET STREQUAL "arm-vita-eabi")
  message(FATAL_ERROR "Use vita/clang-vita-toolchain.cmake; this is not a host build")
endif()

get_filename_component(GALAXY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(DECOMP "${GALAXY_ROOT}/decomp")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Match the root upstream inventory, including its hardware-SDK exclusions.
file(GLOB_RECURSE GAME_SRCS CONFIGURE_DEPENDS "${DECOMP}/src/Game/*.cpp" "${DECOMP}/src/Game/*.c")
file(GLOB_RECURSE JSYSTEM_SRCS CONFIGURE_DEPENDS "${DECOMP}/src/JSystem/*.cpp")
file(GLOB_RECURSE NW4R_SRCS CONFIGURE_DEPENDS "${DECOMP}/src/nw4r/*.cpp")
set(SDK_MODULES gx gd mem arc tpl aralt)
set(SDK_SRCS)
foreach(module IN LISTS SDK_MODULES)
  file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${DECOMP}/src/RVL_SDK/${module}/*.c")
  list(APPEND SDK_SRCS ${sources})
endforeach()
list(FILTER SDK_SRCS EXCLUDE REGEX "/gx/GXFifo\\.c$|/gx/GXPerf\\.c$")
list(APPEND SDK_SRCS "${DECOMP}/src/RVL_SDK/os/OSMutex.c"
  "${DECOMP}/src/RVL_SDK/os/OSMessage.c" "${DECOMP}/src/RVL_SDK/thp/THPAudio.c")

# Evaluate only upstream's inventory with CMake, never its changing host runtime.
# Fail closed if these source-section boundaries change upstream.
foreach(group IN ITEMS GAME JSYSTEM NW4R SDK)
  set(sidecar_${group} "${${group}_SRCS}")
endforeach()
file(READ "${GALAXY_ROOT}/CMakeLists.txt" upstream_cmake)
string(FIND "${upstream_cmake}" "file(GLOB_RECURSE GAME_SRCS" inventory_begin)
string(FIND "${upstream_cmake}" "set(DECOMP_INCLUDES" inventory_end)
if(inventory_begin LESS 0 OR inventory_end LESS inventory_begin)
  message(FATAL_ERROR "Upstream source inventory boundaries changed; review parity")
endif()
math(EXPR inventory_length "${inventory_end} - ${inventory_begin}")
string(SUBSTRING "${upstream_cmake}" ${inventory_begin} ${inventory_length} upstream_inventory)
file(WRITE "${CMAKE_BINARY_DIR}/upstream-inventory.cmake" "${upstream_inventory}")
include("${CMAKE_BINARY_DIR}/upstream-inventory.cmake")
set(upstream_manifest "group\tsource\n")
foreach(group IN ITEMS GAME JSYSTEM NW4R SDK)
  list(SORT ${group}_SRCS)
  list(SORT sidecar_${group})
  if(NOT "${${group}_SRCS}" STREQUAL "${sidecar_${group}}")
    message(FATAL_ERROR "Vita ${group} source set differs from upstream")
  endif()
  foreach(source IN LISTS ${group}_SRCS)
    file(RELATIVE_PATH relative "${GALAXY_ROOT}" "${source}")
    string(APPEND upstream_manifest "${group}\t${relative}\n")
  endforeach()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/upstream-source-manifest.tsv" "${upstream_manifest}")

add_library(vita_source_options INTERFACE)
target_include_directories(vita_source_options INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${GALAXY_ROOT}/platform/include"
  "${DECOMP}/include"
  "${DECOMP}/libs/JSystem/include"
  "${DECOMP}/libs/RVL_SDK/include"
  "${DECOMP}/libs/nw4r/include"
  "${DECOMP}/libs/RVLFaceLib/include")
target_compile_definitions(vita_source_options INTERFACE TARGET_PC=1 NDEBUG=1 VERSION=0)
# Keep upstream behavior flags, not Strikers' translated-code optimizations.
target_compile_options(vita_source_options INTERFACE
  -O2 -fshort-wchar -fsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=on
  -fno-fast-math -fms-extensions -ferror-limit=0
  "-ffile-prefix-map=${GALAXY_ROOT}/=" "-ffile-prefix-map=${GALAXY_ROOT}=."
  "-ffile-prefix-map=${VITASDK}=vitasdk"
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/vita_source_prelude.h"
  "SHELL:-include ${GALAXY_ROOT}/platform/include/port/compat.h"
  -Wno-register -Wno-multichar -Wno-invalid-offsetof -Wno-c++11-narrowing
  -Wno-writable-strings -Wno-null-conversion -Wno-deprecated-declarations
  -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-invalid-source-encoding
  -Wno-unused-value -Wno-return-type -Wno-switch -Wno-parentheses
  -Wno-dangling-else -Wno-tautological-compare -Wno-tautological-undefined-compare
  -Wno-undefined-bool-conversion -Wno-constant-conversion -Wno-shift-negative-value
  -Wno-implicit-function-declaration -Wno-int-conversion -Wno-incompatible-pointer-types
  -Wno-incompatible-function-pointer-types -Wno-pointer-sign -Wno-unknown-pragmas
  -Wno-ignored-attributes -Wno-microsoft-template -Wno-void-pointer-to-int-cast
  -Wno-microsoft-cast -Wno-microsoft-anon-tag -Wno-microsoft-include
  -Wno-language-extension-token
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(CMAKE_C_COMPILER_LAUNCHER "${Python3_EXECUTABLE};${CMAKE_CURRENT_SOURCE_DIR}/record_compile.py")
set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_C_COMPILER_LAUNCHER}")

set(manifest "group\tsource\n")
foreach(group IN ITEMS GAME JSYSTEM NW4R SDK)
  string(TOLOWER "${group}" lower)
  add_library(vita_${lower}_objects OBJECT ${${group}_SRCS})
  target_link_libraries(vita_${lower}_objects PRIVATE vita_source_options)
  list(LENGTH ${group}_SRCS count)
  message(STATUS "Vita ${group}: ${count} source objects")
  foreach(source IN LISTS ${group}_SRCS)
    file(RELATIVE_PATH relative "${GALAXY_ROOT}" "${source}")
    string(APPEND manifest "${group}\t${relative}\n")
  endforeach()
endforeach()
file(WRITE "${CMAKE_BINARY_DIR}/source-manifest.tsv" "${manifest}")

add_library(galaxy_vita_sources STATIC
  $<TARGET_OBJECTS:vita_game_objects> $<TARGET_OBJECTS:vita_jsystem_objects>
  $<TARGET_OBJECTS:vita_nw4r_objects> $<TARGET_OBJECTS:vita_sdk_objects>)
# This archive deliberately leaves platform/runtime symbols unresolved.
set_target_properties(galaxy_vita_sources PROPERTIES LINKER_LANGUAGE CXX)
