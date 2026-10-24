#include "Game/Screen/GalaxyMapBackground.hpp"
#include "Game/Util/LayoutUtil.hpp"

GalaxyMapBackground::GalaxyMapBackground() : LayoutActor("\x94\x77\x8c\x69", true) {
    initLayoutManager("MapGalaxyBg", 1);
}

void GalaxyMapBackground::appear() {
    LayoutActor::appear();
    MR::startAnim(this, "Wait", 0);
}
