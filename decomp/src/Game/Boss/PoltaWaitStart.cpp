#include "Game/Boss/PoltaWaitStart.hpp"
#include "Game/Boss/Polta.hpp"
#include "Game/Boss/PoltaActionBase.hpp"
#include "Game/Boss/PoltaFunction.hpp"
#include "Game/Util/LiveActorUtil.hpp"

PoltaWaitStart::PoltaWaitStart(Polta* pPolta) : PoltaActionBase("\x83\x7c\x83\x8b\x83\x5e\x8a\x4a\x8e\x6e\x91\xd2\x82\xbf", pPolta) {
}

void PoltaWaitStart::appear() {
    mIsDead = false;
    PoltaFunction::killLeftArm(getHost());
    PoltaFunction::killRightArm(getHost());
    MR::hideModel(getHost());
}

PoltaWaitStart::~PoltaWaitStart() {
}
