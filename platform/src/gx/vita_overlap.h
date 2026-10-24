#pragma once
#include "vita_frame.h"
#include <span>

namespace gpu {
struct VitaOverlapGroup {
    uint32_t firstIndex=0,indexCount=0;
    std::array<int32_t,4> bounds{};
};

// Original index order, whole triangles, pairwise-disjoint conservative pixel
// bounds per group. Empty coverage is retained. Failed validation leaves out intact.
bool partitionVitaTriangles(const VitaPreparedDraw&,std::span<const uint16_t>,
    std::vector<VitaOverlapGroup>& out,std::string& error);
}
