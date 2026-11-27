// setfix31: the pad on the Yes / No window (port/yesno_pad.h): the focus starts on No, Left / Right move it, Cross answers it, Circle is No,
// the pointer moves the focus when it comes onto a button. A run of random presses always ends with Circle answering No.
#include "port/yesno_pad.h"
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); std::exit(1); } } while (0)
using namespace port::yesno;
static int Press(PadState& s, bool l, bool r, bool d, bool b, int p = kNone) { PadIn in; in.left = l, in.right = r, in.decide = d, in.back = b, in.pointed = p; return Step(s, in); }
int main() {
    {   // nothing pressed: no answer, focus on No
        PadState s; for (int i = 0; i < 100; ++i) CHECK(Press(s, false, false, false, false) == kNone); CHECK(s.focus == kNo);
    }
    { PadState s; CHECK(Press(s, false, false, true, false) == kNo); }                     // Cross at once: the safe answer
    { PadState s; Press(s, false, true, false, false); CHECK(s.focus == kYes); CHECK(Press(s, false, false, true, false) == kYes); }   // Right, Cross: Yes
    { PadState s; Press(s, false, true, false, false); Press(s, true, false, false, false); CHECK(Press(s, false, false, true, false) == kNo); }   // Right, Left, Cross: No
    { PadState s; Press(s, false, true, false, false); CHECK(Press(s, false, false, false, true) == kNo); }   // Circle is No even with the focus on Yes
    {   // the pointer: coming onto Yes moves the focus there and the click answers it (as before), in the same frame too
        PadState s; CHECK(Press(s, false, false, false, false, kYes) == kNone && s.focus == kYes);
        CHECK(Press(s, false, false, true, false, kYes) == kYes);
        PadState t; CHECK(Press(t, false, false, true, false, kYes) == kYes);
        // resting on Yes does not undo a Left: the pad wins until the pointer moves onto a button again
        PadState u; Press(u, false, false, false, false, kYes); Press(u, true, false, false, false, kYes); CHECK(u.focus == kNo);
        CHECK(Press(u, false, false, false, false, kYes) == kNone && u.focus == kNo);
        Press(u, false, false, false, false, kNone); Press(u, false, false, false, false, kYes); CHECK(u.focus == kYes);
    }
    {   // monkey: random presses; whatever happened, Circle answers No (never stuck)
        unsigned seed = 12345;
        auto rnd = [&] { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
        for (int run = 0; run < 20000; ++run) {
            PadState s; int answer = kNone;
            for (int k = 0; k < 40 && answer == kNone; ++k) {
                const unsigned r = rnd() % 8;
                answer = Press(s, r == 0, r == 1, false, false, r == 2 ? kYes : r == 3 ? kNo : kNone);
                CHECK(s.focus == kNo || s.focus == kYes);
            }
            CHECK(Press(s, false, false, false, true) == kNo);
        }
    }
    std::puts("yesno_pad: all passed");
}
