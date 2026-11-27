// The Vita system language -> the disc language names (platform/include/port/system_language.h), against the ids of the SDK's enum when it is there.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "port/system_language.h"
#define CHECK(c, n) do { const char* got = port_system_language_name(c); if (!(n ? (got && !strcmp(got, n)) : !got)) { std::fprintf(stderr, "FAIL line %d: id %d -> %s\n", __LINE__, (int)(c), got ? got : "null"); std::exit(1); } } while (0)
int main() {
    CHECK(1, "english"); CHECK(18, "english"); CHECK(2, "french"); CHECK(3, "spanish"); CHECK(4, "german"); CHECK(5, "italian");
    CHECK(0, "japanese"); CHECK(9, "korean");
    CHECK(6, nullptr); CHECK(7, nullptr); CHECK(8, nullptr); CHECK(10, nullptr); CHECK(17, nullptr); CHECK(19, nullptr); CHECK(-1, nullptr); CHECK(99, nullptr);
    std::puts("PASS: system language mapping");
    return 0;
}
