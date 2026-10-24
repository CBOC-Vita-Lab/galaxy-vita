# armhf Linux (32-bit ARM, glibc, libstdc++) with the PS Vita build's code-generation flags.
# Compiler: clang (as in vita/clang-vita-toolchain.cmake); linker driver: arm-linux-gnueabihf-g++
# (clang's own link step needs the cross crt files; do NOT link -static, see strikers CONTEXT.md).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
find_program(CMAKE_C_COMPILER NAMES clang REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES clang++ REQUIRED)
find_program(CMAKE_AR NAMES llvm-ar REQUIRED)
find_program(CMAKE_RANLIB NAMES llvm-ranlib REQUIRED)
set(CMAKE_C_COMPILER_TARGET arm-linux-gnueabihf)
set(CMAKE_CXX_COMPILER_TARGET arm-linux-gnueabihf)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# Link with the GNU cross driver.
set(CMAKE_CXX_LINK_EXECUTABLE "arm-linux-gnueabihf-g++ <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
