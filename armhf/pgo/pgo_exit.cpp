// Profile run only (armhf/CMakeLists.txt ARMHF_PGO_GEN): the runner ends with _exit(), which skips the atexit hook that
// clang's profile runtime uses to write the .profraw.  This _exit writes it first (path: LLVM_PROFILE_FILE), then leaves.
#include <sys/syscall.h>
#include <unistd.h>

extern "C" int __llvm_profile_write_file(void);

extern "C" void _exit(int status) {
    static volatile int writing = 0;
    if (!__sync_lock_test_and_set(&writing, 1)) __llvm_profile_write_file();
    for (;;) syscall(SYS_exit_group, status);
}

extern "C" void _Exit(int status) { _exit(status); }
