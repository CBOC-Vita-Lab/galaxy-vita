// tests31: -include this BEFORE platform/include/port/compat.h. compat.h #defines swprintf / vswprintf / wcs* to port_* names; libstdc++'s <string> (and others)
// use std::vswprintf, so a forced -include of compat.h ahead of them breaks the standard headers ("no member named 'port_vswprintf' in namespace 'std'").
#pragma once
#include <string>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>
