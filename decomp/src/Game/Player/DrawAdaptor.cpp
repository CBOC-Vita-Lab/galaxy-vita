#include "Game/Player/DrawAdaptor.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/Util/ObjUtil.hpp"

DrawAdaptor::DrawAdaptor(const MR::FunctorBase& rFunc, int drawType) : NameObj("\x83\x68\x83\x8d\x81\x5b""2D"), mFunc(rFunc.clone(nullptr)) {
    MR::connectToScene(this, MR::MovementType_None, MR::CalcAnimType_None, MR::DrawBufferType_None, drawType);
}

void DrawAdaptor::draw() const {
    (*mFunc)();
}
