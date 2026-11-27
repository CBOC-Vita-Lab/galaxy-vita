// host stand-in for newlib reent.h (tools/test_fclose_lock.sh)
#pragma once
struct _reent { int _errno; };
