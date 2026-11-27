// np_json.h (the reader for the online service's answers): depth, size and node limits, and a mutation fuzz that must never crash or hang.
// Build/run: bash tests/mp/json_limits_build.sh
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "online/np_json.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

static bool parse(const std::string& s) { NpNet::Json j; return j.Parse(s.data(), s.size()); }

int main() {
    // ordinary documents
    CHECK(parse("{\"a\":[1,2,{\"b\":\"c\"}],\"d\":true,\"e\":null}"));
    CHECK(!parse(""));
    CHECK(!parse("{"));
    CHECK(!parse("{\"a\":1} x"));
    CHECK(!parse("{\"a\":\"\x01\"}"));            // a control character inside a string
    CHECK(parse("[\"\\ud800\"]"));            // a lone surrogate becomes a question mark, not an error
    NpNet::Json none;
    CHECK(!none.Parse(nullptr, 5));

    // depth: 12 levels of nesting are fine, a deeper document is refused (and nothing recurses past the limit)
    auto nest = [](int n) { return std::string(size_t(n), '[') + std::string(size_t(n), ']'); };
    CHECK(parse(nest(12)));
    CHECK(!parse(nest(14)));
    CHECK(!parse(nest(100000)));                  // would blow the stack without the limit
    std::string objs;
    for (int i = 0; i < 100000; ++i) objs += "{\"a\":";
    CHECK(!parse(objs));

    // nodes: 4000 at most
    auto flat = [](int n) { std::string s = "["; for (int i = 0; i < n; ++i) s += i ? ",1" : "1"; return s + "]"; };
    CHECK(parse(flat(3990)));
    CHECK(!parse(flat(4100)));

    // size: 256 KiB of text at most (a long string would otherwise be copied into memory)
    CHECK(parse("\"" + std::string(100 * 1024, 'x') + "\""));
    CHECK(!parse("\"" + std::string(300 * 1024, 'x') + "\""));
    CHECK(!parse(std::string(300 * 1024, ' ') + "1"));

    // after a failed parse the document is empty and safe to read
    NpNet::Json j;
    CHECK(!j.Parse("{\"a\":", 5));
    CHECK(j.Root() < 0 && j.Size(j.Root()) == 0 && j.GetStr(j.Root(), "a").empty() && j.GetNum(j.Root(), "a", 7) == 7);
    CHECK(j.Parse("{\"a\":3}", 7) && j.GetNum(j.Root(), "a") == 3);
    CHECK(!j.Parse(nest(40).data(), 80) && j.Root() < 0);   // (and parsing again after a refusal starts clean)

    // mutation fuzz: random byte flips / truncations / splices of valid documents; nothing may crash (run under ASan/UBSan) or loop
    const char* seeds[] = {
        "{\"rows\":[{\"rank\":1,\"name\":\"Ana\",\"frames\":3600,\"players\":1,\"verified\":false,\"you\":true}],\"total\":1}",
        "{\"code\":\"K7QM4D\",\"members\":[{\"slot\":0,\"pid\":7,\"endpoint\":{\"public\":\"1.2.3.4:5\",\"candidates\":[{\"type\":\"srflx\",\"addr\":\"1.2.3.4:5\"}]}}],\"u\":\"\\u00e9\\ud83d\"}",
        "[1e308,-1e-308,0.5,\"\\n\",[[[]]],{}]",
    };
    uint32_t x = 12345;
    auto rnd = [&x]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
    int accepted = 0;
    for (int iter = 0; iter < 200000; ++iter) {
        std::string s = seeds[rnd() % 3];
        const int edits = 1 + int(rnd() % 4);
        for (int e = 0; e < edits && !s.empty(); ++e) {
            switch (rnd() % 4) {
            case 0: s[rnd() % s.size()] = char(rnd()); break;
            case 1: s.resize(rnd() % (s.size() + 1)); break;
            case 2: s.insert(rnd() % (s.size() + 1), 1, "[]{}\",:\\"[rnd() % 8]); break;
            default: s.erase(rnd() % s.size(), 1); break;
            }
        }
        NpNet::Json d;
        if (d.Parse(s.data(), s.size())) {
            ++accepted;
            (void)d.GetStr(d.Root(), "code");
            (void)d.Size(d.Get(d.Root(), "members"));
        }
    }
    CHECK(accepted > 100);   // the fuzz does reach valid documents too
    printf("json limits: %s (fuzz accepted %d of 200000)\n", g_fail ? "FAILED" : "ok", accepted);
    return g_fail ? 1 : 0;
}
