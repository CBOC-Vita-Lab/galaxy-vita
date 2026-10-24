#include "../platform/src/os/os_alarm.cpp"
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    port_log_file(argv[1]);
    port_log("ordinary diagnostic %d", 42);
    port_log_flush();
    std::ifstream file(argv[1]);
    const std::string data((std::istreambuf_iterator<char>(file)), {});
    if (!data.empty()) {
        std::fprintf(stderr, "FAIL: quiet logging still formatted or wrote an ordinary diagnostic\n");
        return 1;
    }
    port_log_summary("fps: shown %.1f turns %.1f", 12.5, 50.0);
    port_log_flush();
    std::ifstream summaryFile(argv[1]);
    const std::string summary((std::istreambuf_iterator<char>(summaryFile)), {});
    if (summary.find("fps: shown 12.5 turns 50.0") == std::string::npos) return 1;
    std::puts("PASS: quiet logger suppresses ordinary diagnostic output");
}
