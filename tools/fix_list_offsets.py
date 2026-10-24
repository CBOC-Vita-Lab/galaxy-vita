#!/usr/bin/env python3
"""Intrusive lists must know where their node lives inside the element.

The decomp hard-codes node offsets for non-CodeWarrior builds (0, or the
32-bit Wii layout's -0x248/-384).  On the 64-bit port they must be the real
offsetof() values:
  - nw4r ut::LinkList / JUTConsoleManager typedefs: use the offsetof() branch
    unconditionally (the element types are complete there);
  - JGadget::TLinkList for JASTrack / AudMeTrack (declared inside or before
    the class, where offsetof is not yet available): go through
    JGadget::TLinkListOffset<T, N>, specialised after the class definition.
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patch import read, write, sub, done  # noqa: E402

# 1. Remove the "#else ..., 0>" fallbacks.
BLOCK = re.compile(r'([ \t]*)#ifdef __MWERKS__\n(\s*typedef [^\n]*offsetof[^\n]*\n)[ \t]*#else\n\s*typedef [^\n]*\n[ \t]*#endif\n')
for path in ['libs/nw4r/include/nw4r/lyt/arcResourceAccessor.h', 'libs/nw4r/include/nw4r/lyt/group.h', 'libs/nw4r/include/nw4r/lyt/layout.h',
             'libs/nw4r/include/nw4r/lyt/pane.h', 'libs/nw4r/include/nw4r/lyt/types.h', 'libs/JSystem/include/JSystem/JUtility/JUTConsole.hpp']:
    text = read(path)
    new, n = BLOCK.subn(lambda m: m.group(2), text)
    if n == 0:
        print('no fallback block in', path)
    write(path, new)
    print('%s: %d typedef(s)' % (path, n))

# 2. Offset traits for JGadget::TLinkList.
L = 'libs/JSystem/include/JSystem/JGadget/linklist.hpp'
sub(L, '''    template < typename T, int NODE_OFFSET >''', '''    // Node offset used by TLinkList<T, NODE_OFFSET>.  Specialise it (after T is
    // complete) where the literal offset only matches the Wii's 32-bit layout.
    template < typename T, int NODE_OFFSET >
    struct TLinkListOffset {
        static int value() {
            return NODE_OFFSET;
        }
    };

    template < typename T, int NODE_OFFSET >''', count=1)
sub(L, '''            return (TLinkListNode*)((u8*)element - NODE_OFFSET);''', '''            return (TLinkListNode*)((u8*)element - TLinkListOffset< T, NODE_OFFSET >::value());''')
sub(L, '''            return (T*)((u8*)element + NODE_OFFSET);''', '''            return (T*)((u8*)element + TLinkListOffset< T, NODE_OFFSET >::value());''')
done()
