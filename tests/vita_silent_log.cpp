#include "../platform/src/os/os_alarm.cpp"
#include <fstream>
#include <iterator>
#include <string>

static std::string readFile(const std::string& path) {
    std::ifstream file(path);
    return std::string((std::istreambuf_iterator<char>(file)), {});
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    if (std::string(argv[1]) == "fatal") port_fatal("fatal logging must be silent");
    const std::string path = argv[1];
    std::remove(path.c_str());
    std::remove((path + ".prev").c_str());
    port_log_file(path.c_str());
    port_log_flush();
    if (std::ifstream(path).good()) {
        std::fprintf(stderr, "FAIL: silent logger created a file\n");
        return 1;
    }
    { std::ofstream file(path); file << "existing log remains untouched"; }
    port_log_file(path.c_str());
    int formatted = -1;
    port_log("diagnostic%n", &formatted);
    port_log_summary("summary%n", &formatted);
    OSReport("game report%n", &formatted);
    port_log_flush();
    if (formatted != -1 || readFile(path) != "existing log remains untouched" ||
        std::ifstream(path + ".prev").good()) {
        std::fprintf(stderr, "FAIL: silent logger formatted, wrote, or rotated a log\n");
        return 1;
    }
    std::fprintf(stderr, "renderer diagnostic%n", &formatted);
    std::printf("library diagnostic%n", &formatted);
    if (formatted != -1) return 1;
    FILE* data = std::tmpfile();
    if (!data) return 1;
    const bool wrote = std::fprintf(data, "%s:%d", "save", 42) == 7 &&
                       std::fwrite("!", 1, 1, data) == 1;
    std::rewind(data);
    char contents[9] = {};
    const bool read = std::fread(contents, 1, 8, data) == 8;
    std::fclose(data);
    if (!wrote || !read || std::string(contents) != "save:42!") return 1;
    std::puts("PASS: silent logger leaves files and format arguments untouched");
}
