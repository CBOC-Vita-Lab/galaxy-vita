#pragma once

#ifdef __cplusplus
#include <atomic>
#include <mutex>

namespace port::settings {
enum class Resource { Input, Interrupts, Clock, Producers, Audio, Renderer, Scanout, Count };
struct Hooks {
    void* context;
    // A failed acquire must undo its own partial work. Successful acquisitions
    // retain their previous state in context for exact restoration by release.
    bool (*acquire)(void*, Resource, unsigned timeoutMs);
    void (*release)(void*, Resource);
};
class Lease {
public:
    Lease() = default;
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    bool Acquire(Hooks& hooks, unsigned timeoutMs) {
        if (mCount) return false;
        for (; mCount != static_cast<unsigned>(Resource::Count); ++mCount) {
            if (!hooks.acquire(hooks.context, static_cast<Resource>(mCount), timeoutMs)) {
                Release(hooks);
                return false;
            }
        }
        return true;
    }
    void Release(Hooks& hooks) {
        while (mCount) hooks.release(hooks.context, static_cast<Resource>(--mCount));
    }
    bool Active() const { return mCount == static_cast<unsigned>(Resource::Count); }
private:
    unsigned mCount = 0;
};

// The owner may finish a non-cancellable device wait after its caller times out.
// Cancellation and acknowledgement share a lock so a late completion cannot
// overwrite the acknowledgement for a newer request.
class HoldGate {
public:
    unsigned Begin(unsigned requested = 0) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mRequested.load()) return 0;
        if (requested) mSequence = requested;
        else if (++mSequence == 0) ++mSequence;
        mAcknowledged.store(0);
        mRequested.store(mSequence);
        return mSequence;
    }
    unsigned Requested() const { return mRequested.load(std::memory_order_acquire); }
    bool Acknowledge(unsigned generation) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!generation || mRequested.load() != generation) return false;
        mAcknowledged.store(generation, std::memory_order_release);
        return true;
    }
    bool Acknowledged(unsigned generation) const {
        return generation && mAcknowledged.load(std::memory_order_acquire) == generation && Requested() == generation;
    }
    void Cancel(unsigned generation) {
        std::lock_guard<std::mutex> lock(mMutex);
        if (generation && mRequested.load() == generation) {
            mRequested.store(0, std::memory_order_release);
            mAcknowledged.store(0);
        }
    }
private:
    std::mutex mMutex;
    unsigned mSequence = 0;
    std::atomic<unsigned> mRequested{0}, mAcknowledged{0};
};
} // namespace port::settings
extern "C" {
#endif
void port_settings_request(void);
void port_settings_game_boundary(int safe);
int port_settings_active(void);
int port_settings_time_pause(int pause);
#ifdef __cplusplus
}
#endif
