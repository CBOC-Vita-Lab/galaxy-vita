// Galaxy 3.0 multiplayer: beta diagnostics, the door for other menus (Settings > More later). Screens, rows and flow live in
// src/Game/MP/MpMenuReport.cpp (a family of MpMenu::Sequence). Both calls need the Multiplayer menu's Sequence (MpMenu::sequence()).
#pragma once

namespace MpReport {

// "Send a Problem Report": the online account first (the existing sign-up flow when there is none), what is sent and what is not, then
// Send / Add a Note / Cancel. where: 0 the Multiplayer menu, 1 Settings, 2 after a session.
void open(int where);

// "Check Connection": the NAT / relay check, then its result in a window.
void openCheck();

}  // namespace MpReport
