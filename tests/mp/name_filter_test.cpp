// The game's copy of the service's name filter (platform/src/mp/mp_name_filter.h) against the shared test names
// (server/test/fixtures/name_vectors.txt): every line must give the same answer as the service's names.js. Prints nothing about the list.
#include <stdio.h>
#include <string.h>
#include <string>
#include "mp_name_filter.h"

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "server/test/fixtures/name_vectors.txt";
    FILE* f = fopen(path, "r");
    if (!f) { printf("cannot open vectors\n"); return 1; }
    char line[256]; int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!l || line[0] == '#') continue;
        char* tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char* want = tab + 1;
        const int r = mp_nf::Reason(line, strlen(line));
        const char* got = r == mp_nf::OK ? "ok" : r == mp_nf::FILTERED ? "filtered" : "reserved";
        ++n;
        if (strcmp(got, want)) { ++bad; printf("MISMATCH vector %d: expected %s, got %s\n", n, want, got); }
    }
    fclose(f);
    // hostile input: very long, invalid UTF-8, nothing usable
    std::string big(5000, 'a');
    mp_nf::Reason(big.c_str(), big.size());
    mp_nf::Reason("\xff\xfe\xc3", 3);
    if (mp_nf::Reason("....", 4) != mp_nf::OK) { ++bad; printf("separators only\n"); }
    printf("name filter: %d vectors, %d mismatches\n", n, bad);
    return bad ? 1 : 0;
}
