// Disc and language selection of platform/src/dvd/dvd.cpp on synthetic converted folders (no game data, no device): which disc a copy is,
// which languages it offers, which one the game starts in (setting, console language fallback, English fallback), and the game's own
// language table (Language.cpp's rows) for the Wii language codes the port returns.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../platform/src/dvd/dvd.cpp"

namespace {
void require(bool ok, const char* why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
void put32(std::vector<unsigned char>& v, size_t at, uint32_t x) { for (int i = 0; i < 4; i++) v[at + i] = (unsigned char)(x >> (24 - 8 * i)); }

// A copy with sys/fst.bin listing the root folders `dirs` (each a directory entry with no children) and files/<dir>/MessageData/Message.arc
// on disk for those in `withTexts`.
std::string makeCopy(const char* tag, const std::vector<std::string>& dirs, const std::vector<std::string>& withTexts) {
    char root[] = "/tmp/galaxy-dvd-lang-XXXXXX";
    require(mkdtemp(root), "private fixture directory");
    std::string r = root;
    (void)tag;
    mkdir((r + "/sys").c_str(), 0700);
    mkdir((r + "/files").c_str(), 0700);
    const size_t count = dirs.size() + 1;
    std::vector<unsigned char> fst(count * 12, 0);
    std::string names;
    fst[0] = 1;
    put32(fst, 8, (uint32_t)count);
    for (size_t i = 0; i < dirs.size(); i++) {
        unsigned char* e = &fst[(i + 1) * 12];
        e[0] = 1;
        uint32_t off = (uint32_t)names.size();
        e[1] = (unsigned char)(off >> 16); e[2] = (unsigned char)(off >> 8); e[3] = (unsigned char)off;
        put32(fst, (i + 1) * 12 + 8, (uint32_t)(i + 2));  // next entry after the (empty) folder
        names += dirs[i];
        names.push_back('\0');
    }
    FILE* f = fopen((r + "/sys/fst.bin").c_str(), "wb");
    require(f != nullptr, "fst written");
    fwrite(fst.data(), 1, fst.size(), f);
    fwrite(names.data(), 1, names.size(), f);
    fclose(f);
    for (const std::string& d : withTexts) {
        mkdir((r + "/files/" + d).c_str(), 0700);
        mkdir((r + "/files/" + d + "/MessageData").c_str(), 0700);
        FILE* m = fopen((r + "/files/" + d + "/MessageData/Message.arc").c_str(), "wb");
        require(m != nullptr, "texts written");
        fputc(1, m);
        fclose(m);
    }
    return r;
}
std::string names() {
    std::string s;
    for (int i = 0; i < port_language_count(); i++) s += std::string(i ? "," : "") + port_language_name(i);
    return s;
}
}  // namespace

extern "C" void port_host_alloc_scope_enter(void) {}
extern "C" void port_host_alloc_scope_leave(void) {}
extern "C" void port_gpu_memory_written(void*, uint32_t) {}
extern "C" void OSWakeupThread(OSThreadQueue*) {}
extern "C" void port_irq_raise(int) {}
// Never reached by this test (the worker and the interrupt layer are not started), but linked.
extern "C" void port_log(const char* fmt, ...) { (void)fmt; }
extern "C" void port_log_summary(const char* fmt, ...) { (void)fmt; }
extern "C" void port_fatal(const char* fmt, ...) { std::fprintf(stderr, "port_fatal: %s\n", fmt); std::exit(2); }
extern "C" BOOL OSDisableInterrupts(void) { return 0; }
extern "C" BOOL OSRestoreInterrupts(BOOL) { return 0; }
extern "C" OSThread* OSGetCurrentThread(void) { return nullptr; }
extern "C" void OSInitThreadQueue(OSThreadQueue*) {}
extern "C" void OSSleepThread(OSThreadQueue*) {}
extern "C" s32 OSSuspendThread(OSThread*) { return 0; }
extern "C" int64_t port_host_time_ns(void) { return 0; }
extern "C" void port_irq_set_handler(int, PortIrqHandler) {}
extern "C" void port_irq_poll(void) {}
extern "C" int64_t port_gap_threshold_ns(void) { return 0; }
extern "C" int64_t port_gap_age_ns(void) { return 0; }
namespace AudParams { const int streamBlockSize = 0, numStreamChannels = 0; }

int main() {
    unsetenv("PETARI_LANGUAGE");
    // ---- the US disc: English, French, Spanish
    const std::string us = makeCopy("us", {"UsEnglish", "UsFrench", "UsSpanish", "StageData"}, {"UsEnglish", "UsFrench", "UsSpanish"});
    require(port_dvd_identify(us.c_str()) && std::strcmp(port_dvd_identify(us.c_str())->id, "RMGE01") == 0, "US copy is RMGE01");
    port_dvd_set_root(us.c_str());
    require(names() == "english,french,spanish", "US copy offers English, French, Spanish");
    require(port_language_start() == SC_LANG_ENGLISH, "no setting, no console language: the disc's first (English)");
    port_language_set_fallback("spanish");
    require(port_language_start() == SC_LANG_SPANISH && port_language_wanted() == 2, "console language Spanish: the game starts in Spanish");
    port_language_set_fallback("french");
    require(port_language_start() == SC_LANG_FRENCH, "console language French: French");
    port_language_set_fallback("german");
    require(port_language_start() == SC_LANG_ENGLISH && port_language_wanted() == 0, "console language the disc lacks (German): English");
    port_language_set_fallback("japanese");
    require(port_language_start() == SC_LANG_ENGLISH, "console language Japanese on the US disc: English");
    port_language_set_fallback("");
    require(port_language_start() == SC_LANG_ENGLISH, "no console language: English");
    port_language_set_fallback("spanish");
    port_language_set("french");
    require(port_language_start() == SC_LANG_FRENCH, "the player's setting wins over the console language");
    port_language_set("German");
    require(port_language_start() == SC_LANG_ENGLISH, "a setting the disc lacks: English (the console language is not used for a named setting)");
    port_language_set("");
    require(port_language_start() == SC_LANG_SPANISH, "setting cleared (System Language): the console language again");
    port_language_set_fallback("");

    // ---- a US copy whose Spanish texts were not converted: the language is not offered
    const std::string usNoEs = makeCopy("us2", {"UsEnglish", "UsFrench", "UsSpanish"}, {"UsEnglish", "UsFrench"});
    port_dvd_set_root(usNoEs.c_str());
    require(names() == "english,french", "a language without converted texts is not offered");
    port_language_set_fallback("spanish");
    require(port_language_start() == SC_LANG_ENGLISH, "console Spanish, no Spanish texts: English");
    port_language_set_fallback("");

    // ---- the European disc: English, French, German, Spanish, Italian (EuDutch holds English texts and is not offered)
    const std::string eu = makeCopy("eu", {"EuEnglish", "EuFrench", "EuGerman", "EuSpanish", "EuItalian", "EuDutch"},
                                    {"EuEnglish", "EuFrench", "EuGerman", "EuSpanish", "EuItalian", "EuDutch"});
    require(port_dvd_identify(eu.c_str()) && std::strcmp(port_dvd_identify(eu.c_str())->id, "RMGP01") == 0, "EU copy is RMGP01");
    port_dvd_set_root(eu.c_str());
    require(names() == "english,french,german,spanish,italian", "EU copy offers five languages");
    port_language_set_fallback("german");
    require(port_language_start() == SC_LANG_GERMAN, "EU, console German: German");
    port_language_set_fallback("italian");
    require(port_language_start() == SC_LANG_ITALIAN, "EU, console Italian: Italian");
    port_language_set_fallback("dutch");
    require(port_language_start() == SC_LANG_ENGLISH, "EU, console Dutch: English");
    port_language_set_fallback("");

    // ---- other / empty copies
    const std::string jp = makeCopy("jp", {"JpJapanese"}, {"JpJapanese"});
    require(port_dvd_identify(jp.c_str()) && std::strcmp(port_dvd_identify(jp.c_str())->id, "RMGJ01") == 0, "JP copy is RMGJ01");
    const std::string none = makeCopy("none", {"StageData"}, {});
    require(port_dvd_identify(none.c_str()) == nullptr, "a copy without any texts is no disc");

    // ---- PETARI_LANGUAGE
    setenv("PETARI_LANGUAGE", "Spanish", 1);
    port_dvd_set_root(us.c_str());
    require(port_language_start() == SC_LANG_SPANISH, "PETARI_LANGUAGE picks the language");
    std::puts("PASS: dvd disc and language selection (US, EU, JP, partial copies, setting, console language, fallback)");
    return 0;
}
