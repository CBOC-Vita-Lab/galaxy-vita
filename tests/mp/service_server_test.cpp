// serverfix: the `server =` key of mp.cfg (a community's own online service) and the per-server account files.
// Build/run: bash tests/mp/service_server_test.sh  (links mp_service.cpp with mp_config.cpp; the build's own address is a compile-time constant)
#include "mp_config.h"
#include "mp_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

extern "C" void NpNetConfigureService(const char*, const char*, const char*, const char*) {}

static uint32_t Crc32(const uint8_t* p, size_t n) {   // the account file's own CRC (IEEE, reflected)
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) { c ^= p[i]; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void Put32(uint8_t* b, uint32_t v) { b[0] = uint8_t(v); b[1] = uint8_t(v >> 8); b[2] = uint8_t(v >> 16); b[3] = uint8_t(v >> 24); }

// writes an online*.dat the way mp_service.cpp reads it: "SMGO", version 1, pid, token[64], name[20], code[12], crc
static void WriteAccount(const std::string& path, int pid, const char* token43) {
    uint8_t b[112];
    memset(b, 0, sizeof b);
    memcpy(b, "SMGO", 4);
    Put32(b + 4, 1);
    Put32(b + 8, uint32_t(pid));
    memcpy(b + 12, token43, 43);
    memcpy(b + 12 + 64, "Tester", 6);
    memcpy(b + 12 + 64 + 20, "23456789", 8);   // an 8-character friend code from the code alphabet (kCodeAlphabet has these digits)
    Put32(b + 108, Crc32(b, 108));
    FILE* f = fopen(path.c_str(), "wb");
    if (f) { fwrite(b, 1, sizeof b, f); fclose(f); }
}

static std::string FnvName(const std::string& url) {
    uint32_t h = 2166136261u;
    for (char ch : url) { h ^= uint8_t(ch); h *= 16777619u; }
    char b[32];
    snprintf(b, sizeof b, "/online_%08x.dat", unsigned(h));
    return b;
}

static bool Creds(std::string* url, std::string* token) {
    char u[200], t[80];
    if (!mp_service_credentials(u, sizeof u, t, sizeof t)) return false;
    *url = u;
    *token = t;
    return true;
}

static void SetServer(const char* s) {
    MpConfig c = *mp_config();
    snprintf(c.server, sizeof c.server, "%s", s);
    mp_config_set(&c);
}

int main() {
    // ---- mp_server_url_clean
    char o[MP_SERVER_URL_MAX + 1];
    CHECK(mp_server_url_clean("https://galaxy.example.org", o, sizeof o) && !strcmp(o, "https://galaxy.example.org"));
    CHECK(mp_server_url_clean("  https://Galaxy.Example.ORG:8443/ \r\n", o, sizeof o) && !strcmp(o, "https://galaxy.example.org:8443"));
    CHECK(mp_server_url_clean("https://smg-online.someone.workers.dev", o, sizeof o));
    CHECK(mp_server_url_clean("https://a-b.c-d.example", o, sizeof o));
    const char* bad[] = {
        "", "http://galaxy.example.org", "ftp://x.example.org", "galaxy.example.org", "https://", "https://localhost", "https://example",
        "https://user@galaxy.example.org", "https://galaxy.example.org/path", "https://galaxy.example.org/?q=1", "https://galaxy.example.org#x",
        "https://galaxy.example.org:0", "https://galaxy.example.org:99999", "https://galaxy.example.org:", "https://galaxy.example.org:80a",
        "https://-a.example.org", "https://a-.example.org", "https://a..example.org", "https://.example.org", "https://example.org.",
        "https://exa mple.org", "https://example.org\n.evil.com", "https://[::1]", "https://exa_mple.org", "HTTPS://example.org",
        "https://galaxy.example.org//", "https://galaxy.example.org:443:443",
    };
    for (const char* b : bad) { const bool ok = mp_server_url_clean(b, o, sizeof o); if (ok) fprintf(stderr, "accepted: [%s]\n", b); CHECK(!ok && o[0] == 0); }
    std::string longUrl = "https://" + std::string(130, 'a') + ".example.org";
    CHECK(!mp_server_url_clean(longUrl.c_str(), o, sizeof o));
    CHECK(!mp_server_url_clean(nullptr, o, sizeof o));
    char tiny[8];
    CHECK(!mp_server_url_clean("https://galaxy.example.org", tiny, sizeof tiny));

    // ---- mp.cfg: read, ignore a bad value, keep it through a save
    char tmpl[] = "/tmp/sf_server_XXXXXX";
    CHECK(mkdtemp(tmpl) != nullptr);
    const std::string root = tmpl;
    mkdir((root + "/mp").c_str(), 0777);
    mp_config_set_dir((root + "/mp").c_str());
    auto writeCfg = [&](const char* server) {
        FILE* f = fopen((root + "/mp/mp.cfg").c_str(), "w");
        fprintf(f, "version = 1\nname = Tester\nserver = %s\n", server);
        fclose(f);
    };
    writeCfg("https://galaxy.example.org");
    CHECK(mp_config_load());
    CHECK(!strcmp(mp_config()->server, "https://galaxy.example.org"));
    writeCfg("http://insecure.example.org");
    mp_config_load();
    CHECK(mp_config()->server[0] == 0);
    writeCfg("https://galaxy.example.org:8443/");
    mp_config_load();
    CHECK(!strcmp(mp_config()->server, "https://galaxy.example.org:8443"));
    CHECK(mp_config_save());   // a settings save must not lose the key
    mp_config_load();
    CHECK(!strcmp(mp_config()->server, "https://galaxy.example.org:8443"));
    SetServer("not a url");
    CHECK(mp_config()->server[0] == 0);
    SetServer("");
    CHECK(mp_config_save());
    mp_config_load();
    CHECK(mp_config()->server[0] == 0);

    // ---- accounts are per server
    const std::string dir = root + "/online";
    mkdir(dir.c_str(), 0777);
    setenv("SMG_ONLINE_DIR", dir.c_str(), 1);
    unsetenv("SMG_ONLINE_URL");
    const char* tokA = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";   // 43
    const char* tokB = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    CHECK(strlen(tokA) == 43 && strlen(tokB) == 43);
    WriteAccount(dir + "/online.dat", 5, tokA);               // the build's own service (-DNP_ONLINE_URL)
    mp_service_shutdown();
    std::string url, token;
    CHECK(mp_service_enabled());
    CHECK(Creds(&url, &token) && url == "https://default.example.org" && token == tokA);

    // the player points the game at a community server: the build's token is NOT used for it, and nothing is sent there with it
    SetServer("https://community.example.net");
    CHECK(mp_service_enabled());
    url.clear();
    CHECK(!Creds(&url, &token));   // no account for this address yet: the first online action offers to register there
    CHECK(token.empty() || token == tokA);   // (never filled for the other address)
    CHECK(url.empty());

    // an account made for the community server lives in its own file and is used for it only
    WriteAccount(dir + FnvName("https://community.example.net"), 9, tokB);
    mp_service_shutdown();
    CHECK(Creds(&url, &token) && url == "https://community.example.net" && token == tokB);

    // back to the build's service: its own account again
    SetServer("");
    CHECK(Creds(&url, &token) && url == "https://default.example.org" && token == tokA);

    // another community server: yet another (missing) account
    SetServer("https://other.example.net");
    CHECK(!Creds(&url, &token));
    // the environment (desktop tests) beats the file; its account is online.dat as ever
    setenv("SMG_ONLINE_URL", "http://127.0.0.1:1", 1);
    SetServer("https://community.example.net");
    CHECK(Creds(&url, &token) && url == "http://127.0.0.1:1");   // (online.dat holds token A: the env address is the "build" address here)
    // an https address from env.txt (the Vita's run-time override, older than the mp.cfg key) is another server too: it never gets the build's token
    setenv("SMG_ONLINE_URL", "https://env.example.net/", 1);
    CHECK(!Creds(&url, &token));
    WriteAccount(dir + FnvName("https://env.example.net"), 11, tokB);
    mp_service_shutdown();
    CHECK(Creds(&url, &token) && url == "https://env.example.net" && token == tokB);
    unsetenv("SMG_ONLINE_URL");
    SetServer("");
    CHECK(Creds(&url, &token) && url == "https://default.example.org" && token == tokA);

    printf("service server key: %s\n", g_fail ? "FAILED" : "ok");
    return g_fail ? 1 : 0;
}
