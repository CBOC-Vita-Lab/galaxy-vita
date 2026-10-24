#include "Game/Demo/AstroDemoFunction.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/JMapUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/StringUtil.hpp"

namespace {
    const char* const cGrandStarReturnDemoTable[] = {
        "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x50\x8b\x41\x8a\xd2", "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x51\x8b\x41\x8a\xd2", "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x52\x8b\x41\x8a\xd2",
        "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x53\x8b\x41\x8a\xd2", "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x54\x8b\x41\x8a\xd2", "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x82\x55\x8b\x41\x8a\xd2",
    };
};  // namespace

namespace AstroDemoFunction {
    int getOpenedAstroDomeNum() {
        return MR::calcOpenedAstroDomeNum();
    }

    const char* getGrandStarReturnDemoName(int index) {
        return ::cGrandStarReturnDemoTable[index];
    }

    int getActiveGrandStarReturnDemoIndex() {
        s32 result;
        for (u32 index = 0; index < ARRAY_SIZE(::cGrandStarReturnDemoTable); index++) {
            if (MR::isDemoActive(::cGrandStarReturnDemoTable[index])) {
                result = index;
                // only way I could get this to match
                // TODO -- fix me
                goto found;
            }
        }

        result = -1;
    found:
        return result;
    }

    void tryRegisterDemo(LiveActor* pParam1, const char* pParam2, const JMapInfoIter& rIter) {
        if (MR::isDemoExist(pParam2) && !MR::isDemoCast(pParam1, pParam2)) {
            MR::tryRegisterDemoCast(pParam1, pParam2, rIter);
        }
    }

    void tryRegisterAstroDemoAll(LiveActor* pParam1, const JMapInfoIter& rIter) {
        AstroDemoFunction::tryRegisterGrandStarReturn(pParam1, rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x4f\x83\x89\x83\x93\x83\x68\x83\x58\x83\x5e\x81\x5b\x8b\x41\x8a\xd2[\x82\x51\x89\xf1\x96\xda\x88\xc8\x8d\x7e]", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x70\x83\x8f\x81\x5b\x83\x58\x83\x5e\x81\x5b\x8b\x41\x8a\xd2", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8f\xf3\x8b\xb5\x90\xe0\x96\xbe\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8d\xc5\x8f\x49\x8c\x88\x90\xed\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x52\x83\x81\x83\x62\x83\x67\x90\xe0\x96\xbe\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x67\x81\x5b\x83\x60\x82\xcc\x89\x8a\x90\xe0\x96\xbe\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x67\x81\x5b\x83\x60\x82\xcc\x89\x8a\x90\x69\x92\xbb\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x93\x56\x95\xb6\x91\xe4\x8b\x40\x94\x5c\x89\xf1\x95\x9c\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x4c\x83\x6d\x83\x73\x83\x49\x92\x54\x8c\x9f\x91\xe0\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8b\x83\x43\x81\x5b\x83\x57\x8e\xb8\xe7\x48\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x4a\x83\x45\x83\x93\x83\x67\x83\x5f\x83\x45\x83\x93\x8a\x4a\x8e\x6e\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x6d\x81\x5b\x83\x7d\x83\x8b\x83\x47\x83\x93\x83\x66\x83\x42\x83\x93\x83\x4f\x8c\xe3\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8b\xe2\x89\xcd\x82\xcc\x92\x86\x90\x53\x90\xe0\x96\xbe\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x8b\x83\x43\x81\x5b\x83\x57\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x6f\x83\x67\x83\x89\x81\x5b\x83\x4f\x83\x8a\x81\x5b\x83\x93\x83\x68\x83\x89\x83\x43\x83\x6f\x90\xe0\x96\xbe", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x6f\x83\x67\x83\x89\x81\x5b\x83\x7d\x83\x62\x83\x76\x83\x8c\x83\x4e\x83\x60\x83\x83\x81\x5b", rIter);
    }

    void tryRegisterGrandStarReturn(LiveActor* pParam1, const JMapInfoIter& rIter) {
        for (u32 i = 0; i < ARRAY_SIZE(::cGrandStarReturnDemoTable); i++) {
            AstroDemoFunction::tryRegisterDemo(pParam1, ::cGrandStarReturnDemoTable[i], rIter);
        }
    }

    void tryRegisterGrandStarReturnWithFunction(LiveActor* pParam1, const JMapInfoIter& rIter, const MR::FunctorBase& rFunctor) {
        const char* pDemoName;

        for (u32 i = 0; i < ARRAY_SIZE(::cGrandStarReturnDemoTable); i++) {
            pDemoName = ::cGrandStarReturnDemoTable[i];

            if (MR::isDemoExist(pDemoName) && MR::tryRegisterDemoCast(pParam1, pDemoName, rIter)) {
                MR::tryRegisterDemoActionFunctorDirect(pParam1, rFunctor, pDemoName, nullptr);
            }
        }
    }

    void tryRegisterGrandStarReturnAndSimpleCast(LiveActor* pParam1, const JMapInfoIter& rIter) {
        AstroDemoFunction::tryRegisterGrandStarReturn(pParam1, rIter);
        AstroDemoFunction::tryRegisterSimpleCastIfAstroGalaxy(pParam1);
    }

    void tryRegisterGrandStarReturnWithFunctionAndSimpleCast(LiveActor* pParam1, const JMapInfoIter& rIter, const MR::FunctorBase& rFunctor) {
        AstroDemoFunction::tryRegisterGrandStarReturnWithFunction(pParam1, rIter, rFunctor);
        AstroDemoFunction::tryRegisterSimpleCastIfAstroGalaxy(pParam1);
    }

    bool tryRegisterSimpleCastIfAstroGalaxy(LiveActor* pParam1) {
        if (MR::isEqualStageName("AstroGalaxy")) {
            MR::registerDemoSimpleCastAll(pParam1);

            return true;
        }

        return false;
    }

    void tryRegisterDemoForTico(LiveActor* pParam1, const JMapInfoIter& rIter) {
        if (tryRegisterSimpleCastIfAstroGalaxy(pParam1)) {
            s32 demoCastID = MR::getDemoCastID(rIter);
            const char* pDemoName = nullptr;

            switch (demoCastID) {
            case 0:
                pDemoName = ::cGrandStarReturnDemoTable[0];
                break;
            case 1:
                pDemoName = ::cGrandStarReturnDemoTable[2];
                break;
            case 2:
                pDemoName = ::cGrandStarReturnDemoTable[4];
                break;
            }

            if (pDemoName != nullptr) {
                AstroDemoFunction::tryRegisterDemo(pParam1, pDemoName, rIter);
            }
        }
    }

    void tryRegisterDemoForLuigiAndKinopio(LiveActor* pParam1, const JMapInfoIter& rIter) {
        AstroDemoFunction::tryRegisterGrandStarReturn(pParam1, rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x83\x8b\x83\x43\x81\x5b\x83\x57\x83\x66\x83\x82", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x6f\x83\x67\x83\x89\x81\x5b\x83\x4f\x83\x8a\x81\x5b\x83\x93\x83\x68\x83\x89\x83\x43\x83\x6f\x90\xe0\x96\xbe", rIter);
        AstroDemoFunction::tryRegisterDemo(pParam1, "\x83\x8d\x83\x5b\x83\x62\x83\x5e\x8d\xc5\x8f\x49\x8c\x88\x90\xed\x83\x66\x83\x82", rIter);
    }
};  // namespace AstroDemoFunction
