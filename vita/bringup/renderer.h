#pragma once
#include "port/input.h"
#include <string>

// Native GXM/input diagnostic only. No game startup, game data or FPS claim.
struct VitaBringupRenderer;
namespace gpu { class VitaShaderLibrary; }
int createVitaBringupRenderer(const gpu::VitaShaderLibrary&,VitaBringupRenderer**,std::string& error);
int drawVitaBringup(VitaBringupRenderer*,const PortPadState&,std::string& error);
// Failed draws stop the runner; closing fences existing work, never redraws.
int releaseVitaBringupRenderer(VitaBringupRenderer**);
