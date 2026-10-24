set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED VITASDK)
  set(VITASDK "$ENV{VITASDK}" CACHE PATH "Read-only VitaSDK root")
endif()
if(NOT EXISTS "${VITASDK}/arm-vita-eabi/include/psp2/types.h")
  message(FATAL_ERROR "VITASDK must point to the existing read-only VitaSDK")
endif()
set(VITA_GXX_VERSION "15.2.0" CACHE STRING "VitaSDK libstdc++ header version")
set(vita_sys "${VITASDK}/arm-vita-eabi")
if(NOT EXISTS "${vita_sys}/include/c++/${VITA_GXX_VERSION}/arm-vita-eabi/bits/c++config.h")
  message(FATAL_ERROR "Missing VitaSDK libstdc++ headers ${VITA_GXX_VERSION}")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES VITASDK VITA_GXX_VERSION)

find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
set(CMAKE_C_COMPILER_TARGET arm-vita-eabi)
set(CMAKE_CXX_COMPILER_TARGET arm-vita-eabi)
set(CMAKE_SYSROOT "${vita_sys}")
set(CMAKE_FIND_ROOT_PATH "${vita_sys}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ABI details taken read-only from Strikers' vita/vita_clang.mk.
# Native game data contains 32-bit enum fields (for example TPL headers). Unlike
# translated Strikers code, these objects must not inherit GCC's short enums.
set(vita_flags "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard -mthumb -fsigned-char -fno-short-enums -femulated-tls -fno-pic -munaligned-access -D__vita__ -D__USES_INITFINI__ -U_GNU_SOURCE -Wno-builtin-macro-redefined -U__WINT_TYPE__ -D'__WINT_TYPE__=unsigned int' -U__INT_FAST8_TYPE__ -D__INT_FAST8_TYPE__=int -U__INT_FAST16_TYPE__ -D__INT_FAST16_TYPE__=int -U__UINT_FAST8_TYPE__ -D'__UINT_FAST8_TYPE__=unsigned int' -U__UINT_FAST16_TYPE__ -D'__UINT_FAST16_TYPE__=unsigned int' -nostdlibinc")
set(CMAKE_C_FLAGS_INIT "${vita_flags} -isystem ${vita_sys}/include")
# C headers must follow libstdc++ so its include_next finds newlib, not the host.
set(CMAKE_CXX_FLAGS_INIT "${vita_flags} -nostdinc++ -isystem ${vita_sys}/include/c++/${VITA_GXX_VERSION} -isystem ${vita_sys}/include/c++/${VITA_GXX_VERSION}/arm-vita-eabi -isystem ${vita_sys}/include/c++/${VITA_GXX_VERSION}/backward -isystem ${vita_sys}/include")
