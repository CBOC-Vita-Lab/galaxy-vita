#pragma once

// Parse runtime declarations before the shared header's UTF-16 name remapping.
#ifdef __cplusplus
#include <algorithm>
#include <functional>
#include <iterator>
#include <locale>
#include <cwchar>
#include <cctype>

// Newlib's short C classification-mask macros collide with decomp field names.
// C++ uses real ctype functions; C macro implementations still need the masks.
#undef _U
#undef _L
#undef _N
#undef _S
#undef _P
#undef _C
#undef _X
#undef _B
#else
#include <wchar.h>
#endif
