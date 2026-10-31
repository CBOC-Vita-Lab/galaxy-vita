// GXV_DOF: unset, empty and unrecognised values leave the depth-of-field blur on; 0 turns it off. (DepthOfFieldBlur::draw returns before
// issuing any command when it is off; the early return is the one the effect already has when the game switches it off itself.)
#include "port/depth_of_field.h"
#include <cassert>
#include <cstdlib>
#include <iostream>
int main() {
    using port::dof::ParseSwitch;
    assert(ParseSwitch(nullptr)&&ParseSwitch("")&&ParseSwitch("1")&&ParseSwitch("2")&&ParseSwitch("on")&&ParseSwitch("0x1")&&ParseSwitch("1x"));
    assert(!ParseSwitch("0")&&!ParseSwitch("00")&&!ParseSwitch("0x0")&&!ParseSwitch("-0"));
    unsetenv("GXV_DOF");assert(port::dof::Enabled());   // read once: the first read decides for the process
    std::cout<<"depth_of_field: switch values passed\n";
}
