#pragma once
// bowl31 (GXV_BOWL_POSE, port/bowl_pose.h): the logic is handed a model's joint matrices (JointUtil.cpp, ModelUtil.cpp). The model is marked for good (the
// rule never leaves its pose out); if its pose is owed right now (left out on a skip turn and not made again yet), the read is counted (mode 1: LATE) or
// compared (mode 2) by LiveActor.cpp. One byte store and one byte test, in every mode (the mark must exist before the switch is read in a stage).
#ifdef TARGET_PC
#include <JSystem/J3DGraphAnimator/J3DModel.hpp>
extern "C" void port_bowl_pose_read(const J3DModel* model, int what);  // (LiveActor.cpp; what: 1 joints, 2 base matrix)
inline void PortBowlJointRead(const J3DModel* pModel) {
    if (pModel == nullptr) {
        return;
    }
    J3DModel* m = const_cast< J3DModel* >(pModel);
    if (__builtin_expect((m->mPortBowl & 2) != 0, 0)) {
        port_bowl_pose_read(m, 1);
    }
    m->mPortBowl |= 1;
}
#endif
