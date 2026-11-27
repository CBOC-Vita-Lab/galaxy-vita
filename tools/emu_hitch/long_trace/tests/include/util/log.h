#pragma once
#include <sstream>
#include <string>
#include <vector>

namespace test_log {
inline std::vector<std::string> lines;
template<class... T> void log(const char *fmt, T... args) {
    std::ostringstream text;
    text << fmt;
    ((text << ' ' << args), ...);
    lines.push_back(text.str());
}
}
#define LOG_INFO(...) test_log::log(__VA_ARGS__)
