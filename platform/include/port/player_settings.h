#pragma once
#include "port/control_bindings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace port::settings {
constexpr size_t kDocumentLimit = 16384;
// Config::fpsLimit: 0 up to 60, 30 fixed 30, kSteady30 adaptive pacing with dynamic quality aimed at 30.
constexpr int kSteady30 = -30;
enum class AutoQuality : uint8_t { Default, Enabled, Disabled };
struct Config {
    uint32_t version = 1;
    int fpsLimit = 0;
    AutoQuality autoQuality = AutoQuality::Default;
    controls::ProfileId profile = controls::ProfileId::Handheld;
    std::string language;
    bool reducedMotion = false;
    std::array<controls::ProfileMapping, 2> profiles;
};
enum class StoreStatus : uint8_t { Ok, Missing, Recovered, Malformed, UnsupportedVersion, TooLarge, IoError };
struct Document { Config config; std::string source; bool editable = true; };
struct StoreResult { StoreStatus status; std::string message; };
struct FileInfo { uint64_t size; bool regular; };
struct FileOps {
    void* context;
    int64_t (*openRead)(void*, const char*);
    int64_t (*openWrite)(void*, const char*);
    int64_t (*read)(void*, int64_t, void*, size_t);
    int64_t (*write)(void*, int64_t, const void*, size_t);
    int (*sync)(void*, int64_t);
    int (*close)(void*, int64_t);
    int (*stat)(void*, const char*, FileInfo&);
    int (*rename)(void*, const char*, const char*);
    int (*remove)(void*, const char*);
};
Config DefaultConfig(const controls::ControlApi&);
bool Equal(const Config&, const Config&);
StoreResult ParseDocument(const std::string&, const controls::ControlApi&, Document&);
StoreResult SerializeDocument(const Document&, const Config&, const controls::ControlApi&, std::string&);
StoreResult LoadDocument(const char*, const controls::ControlApi&, const FileOps&, Document&);
StoreResult SaveDocument(const char*, const Document&, const Config&, const controls::ControlApi&, const FileOps&, Document&);
}
