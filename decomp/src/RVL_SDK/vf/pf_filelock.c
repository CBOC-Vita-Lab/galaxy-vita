#include "revolution/vf/pf_filelock.h"

void VFiPF_InitLockFile() {
    VFipf_sys_set.flock_count = 0;
}

int VFiPF_UnLockFile(PF_FILE* p_file) {
    return -1;
}
