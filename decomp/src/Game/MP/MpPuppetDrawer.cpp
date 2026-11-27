// Galaxy 3.0 multiplayer: draws every puppet from one NameObj in DrawType_Player (owner: puppet). One entry in that category
// (capacity 4: the local Mario, GhostPlayer, us) whatever the number of puppets.
#include "Game/MP/MpDemo.hpp"   // MP3 demo
#include "Game/MP/MpPuppetsInternal.hpp"
#include "Game/MP/MpRemotePlayer.hpp"
#include "Game/MP/MpWarm.hpp"
#include "Game/NameObj/NameObj.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/LightUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include "port/mp_prof.h"

namespace {
    class MpPuppetDrawer : public NameObj {
    public:
        MpPuppetDrawer() : NameObj("MpPuppetDrawer") {}

        virtual void init(const JMapInfoIter&) {
            MR::connectToScene(this, -1, -1, -1, MR::DrawType_Player);
        }

        virtual void draw() const {
            if (!mp_active() && !MpDemo::live()) {   // MP3 demo: the puppet demo draws them too
                return;
            }
            MP_PROF_SCOPE(MP_PROF_TOTAL);
            MP_PROF_SCOPE(MP_PROF_PUPPET_DRAW);
            MpWarm::onDraw();   // MP3 puppetprep: the first pictures draw the scene's puppets / bubble / tag / mark once, out of sight
            MpPuppetsInt::notePicture();
            bool any = false;
            for (int slot = 0; slot < MP_MAX_PLAYERS; slot++) {
                MpPuppetBase* p = MpPuppetsInt::puppet(slot);
                // (perf) off screen: left out (its joints were not computed this turn either)
                if (p == nullptr || !p->usesDrawer() || !p->isDrawable() || p->isCulled()) {
                    continue;
                }
                if (!any) {
                    any = true;
#ifdef TARGET_PC
                    port_gx_marker(PORT_GX_MARK_PLAYER_BEGIN);   // moves on its own, like Mario (VR motion vectors)
#endif
                }
                MR::loadActorLight(p);
                p->drawPuppet();
            }
            if (any) {
#ifdef TARGET_PC
                port_gx_marker(PORT_GX_MARK_PLAYER_END);
#endif
                MR::loadLightPlayer();   // SceneFunction::executeDrawListOpa loads it once before the DrawType_Player list
            }
        }
    };
}  // namespace

namespace MpPuppetDrawerFn {
    void create() {
        MpPuppetDrawer* d = new MpPuppetDrawer();
        d->initWithoutIter();
    }
}  // namespace MpPuppetDrawerFn
