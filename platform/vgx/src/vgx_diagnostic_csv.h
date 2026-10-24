#pragma once

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace vgx::diagnosticcsv {

inline bool RamEnabled() {
#if defined(__vita__) || defined(__VGX_VITA__)
    return true;
#else
    if (const char* value = std::getenv("GXV_STATS_RAM")) return std::strcmp(value, "1") == 0;
    return false;
#endif
}

struct Row {
    char data[4096];
    size_t size = 0;
    bool valid = true;
    __attribute__((format(printf, 2, 3))) void Add(const char* format, ...) {
        if (!valid) return;
        va_list args;
        va_start(args, format);
        const int count = std::vsnprintf(data + size, sizeof data - size, format, args);
        va_end(args);
        if (count < 0 || static_cast<size_t>(count) >= sizeof data - size) valid = false;
        else size += static_cast<size_t>(count);
    }
};

// One producer. Published bytes never change; a paused consumer reads only that
// immutable prefix. Storage remains alive for the process, including late appends.
class Buffer {
public:
    static constexpr size_t kCapacity = 1024 * 1024;
    bool Arm(const char* path, size_t capacity = kCapacity) {
        if (attempted_) return false;
        attempted_ = true;
        if (!path || !*path || std::strlen(path) >= sizeof path_ || !capacity || capacity > kCapacity) return false;
        bytes_ = static_cast<char*>(std::malloc(capacity));
        if (!bytes_) return false;
        capacity_ = capacity;
        std::strcpy(path_, path);
        ready_.store(true, std::memory_order_release);
        return true;
    }
    bool Append(const Row& row) {
        if (!ready_.load(std::memory_order_relaxed)) return false;
        if (frozen_.load(std::memory_order_acquire)) return false;
        if (row.valid && !row.size) return true;
        if (full_ || !row.valid || !row.size || row.data[row.size - 1] != '\n' || row.size > capacity_ - used_) {
            full_ = true;
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::memcpy(bytes_ + used_, row.data, row.size);
        used_ += row.size;
        committed_.store(used_, std::memory_order_release);
        return true;
    }
    // No calls to open/write/close occur before this explicit paused/shutdown path.
    int Export() {
        if (!ready_.load(std::memory_order_acquire)) return 0;
        if (!committed_.load(std::memory_order_acquire) && !dropped_.load(std::memory_order_relaxed)) return 0;
        if (exported_.exchange(true, std::memory_order_acq_rel)) return result_.load(std::memory_order_acquire);
        frozen_.store(true, std::memory_order_release);
        const size_t size = committed_.load(std::memory_order_acquire);
        exportedBytes_.store(size, std::memory_order_release);
        if (!size) { result_.store(-ENODATA, std::memory_order_release); return -ENODATA; }
        const int file = ::open(path_, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (file < 0) { const int error = errno; result_.store(-error, std::memory_order_release); return -error; }
        size_t offset = 0;
        int error = 0;
        while (offset < size) {
            const auto written = ::write(file, bytes_ + offset, size - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = written < 0 ? errno : EIO; break; }
            offset += static_cast<size_t>(written);
        }
        if (::close(file) != 0 && !error) error = errno;
        if (error) ::unlink(path_);
        result_.store(-error, std::memory_order_release);
        return -error;
    }
    size_t Committed() const { return committed_.load(std::memory_order_acquire); }
    size_t ExportedBytes() const { return exportedBytes_.load(std::memory_order_acquire); }
    uint32_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
    bool Frozen() const { return frozen_.load(std::memory_order_acquire); }
private:
    char path_[256] = {};
    char* bytes_ = nullptr;
    size_t capacity_ = 0, used_ = 0;
    bool attempted_ = false, full_ = false;
    std::atomic<bool> ready_{false}, frozen_{false}, exported_{false};
    std::atomic<size_t> committed_{0};
    std::atomic<size_t> exportedBytes_{0};
    std::atomic<uint32_t> dropped_{0};
    std::atomic<int> result_{-EBUSY};
};

class Log {
public:
    void Open(const char* path) {
        if (opened_) return;
        opened_ = true;
        ram_ = RamEnabled();
        if (ram_) active_ = buffer_.Arm(path);
        else active_ = (file_ = std::fopen(path, "w")) != nullptr;
    }
    bool Enabled() const { return active_.load(std::memory_order_acquire) && (!ram_ || !buffer_.Frozen()); }
    void Append(const Row& row) {
        if (ram_) buffer_.Append(row);
        else if (file_ && row.valid) std::fwrite(row.data, 1, row.size, file_);
    }
    void FlushLive() { if (file_) std::fflush(file_); }
    // Producer thread or after joining it. Only ExportPaused is a live consumer API.
    void Close() {
        if (!active_.load(std::memory_order_acquire)) return;
        if (file_) { std::fclose(file_); file_ = nullptr; }
        else buffer_.Export();
        active_ = false;
    }
    int ExportPaused() { return buffer_.Export(); }
    const Buffer& Capture() const { return buffer_; }
private:
    Buffer buffer_;
    FILE* file_ = nullptr;
    bool ram_ = false, opened_ = false;
    std::atomic<bool> active_{false};
};

} // namespace vgx::diagnosticcsv
