#include "vgx_hrw_pair_depth.h"
#include "vgx_cmd.h"
#include <cassert>
#include <cstdio>

int main() {
    namespace pair = vgx::hrwpair;
    vgx::CmdBuf buf;
    for (unsigned id = 1; id <= 65535; ++id) {
        for (bool plain : {true, false}) {
            const auto marker = pair::Marker(id, plain);
            assert(marker <= 0xFFFFFF && pair::IsMarker(marker));
            const auto token = pair::Token(marker);
            buf.setHrwPair(token);
            assert(buf.hrwId == id && buf.hrwNoFresh == plain);
        }
    }
    buf.setHrwPair(pair::Token(pair::Marker(80, true)));
    vgx::CmdBuf next;
    std::swap(buf, next);
    assert(next.hrwNoFresh && !buf.hrwNoFresh);
    next.clear();
    assert(!next.hrwNoFresh && next.hrwId == 0);
    assert(!pair::IsMarker(0x123423) && !pair::IsMarker(7));
    for (int i = 0; i < 64; ++i) {
        bool armed = i & 1, snap = i & 2, fresh = i & 4, plain = i & 8, plane = i & 16, mask = i & 32;
        bool baseline = armed && (snap || fresh || plane || mask);
        bool optimized = pair::NeedsDepth(armed, snap, fresh, plain, plane, mask);
        assert((baseline != optimized) == (armed && !snap && fresh && plain && !plane && !mask));
    }
    puts("C182 depth: 131070 ordered markers; pooled jobs and all 64 consumer combinations passed");
}
