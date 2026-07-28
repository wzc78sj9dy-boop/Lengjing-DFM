#include "game/native/TersafePatchWorker.h"

#if LENGJING_ENABLE_PROJECTILE_TRACKING

#include "game/native/MemoryTransport.h"

#include <array>
#include <chrono>

namespace lengjing::game::native {
namespace {

constexpr char kModuleName[] = "libtersafe.so";
constexpr std::chrono::milliseconds kRetryDelay{1000};
constexpr std::chrono::milliseconds kCompletedDelay{60000};

}  // namespace

TersafePatchWorker::~TersafePatchWorker() {
    Stop();
}

bool TersafePatchWorker::Start(
    MemoryTransport& memory,
    pid_t processId) noexcept {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (worker_.joinable()) {
        return memory_ == &memory && processId_ == processId;
    }
    if (processId <= 0 || !memory.IsOpen() ||
        !memory.CanWrite() || !memory.UsesKernelBackend()) {
        return false;
    }
    memory_ = &memory;
    processId_ = processId;
    stopRequested_ = false;

    try {
        worker_ = std::thread(&TersafePatchWorker::Run, this);
    } catch (...) {
        memory_ = nullptr;
        processId_ = -1;
        stopRequested_ = false;
        return false;
    }
    return true;
}

void TersafePatchWorker::Stop() noexcept {
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopRequested_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::lock_guard<std::mutex> lock(stateMutex_);
    memory_ = nullptr;
    processId_ = -1;
    stopRequested_ = false;
}

void TersafePatchWorker::Run() noexcept {
    bool completed = false;
    for (;;) {
        if (!completed) {
            try {
                completed = PatchOnce();
            } catch (...) {
                completed = false;
            }
        }
        if (WaitForStop(completed ? kCompletedDelay : kRetryDelay)) {
            return;
        }
    }
}

bool TersafePatchWorker::PatchOnce() {
    MemoryTransport* const memory = memory_;
    if (memory == nullptr || !memory->IsOpen() ||
        !memory->CanWrite() || !memory->UsesKernelBackend()) {
        return false;
    }

    const std::uintptr_t moduleBase = memory->ModuleBase(kModuleName);
    if (moduleBase == 0) return false;

    const auto read = [memory](
                          std::uintptr_t address,
                          void* destination,
                          std::size_t size) {
        return memory->Read(address, destination, size);
    };
    std::array<std::uintptr_t, 3> targets{};
    if (!ResolveTersafePatchTargets(moduleBase, read, targets)) {
        return false;
    }

    for (const std::uintptr_t address : targets) {
        std::uint8_t observed = 0;
        if (!memory->Read(address, &observed, sizeof(observed))) {
            return false;
        }
        if (observed == tersafe_patch_layout::kPatchValue) continue;
        const std::uint8_t value = tersafe_patch_layout::kPatchValue;
        if (!memory->Write(address, &value, sizeof(value)) ||
            !memory->Read(address, &observed, sizeof(observed)) ||
            observed != value) {
            return false;
        }
    }
    return true;
}

bool TersafePatchWorker::WaitForStop(
    std::chrono::milliseconds delay) noexcept {
    std::unique_lock<std::mutex> lock(stateMutex_);
    return wake_.wait_for(
        lock,
        delay,
        [this] { return stopRequested_; });
}

}  // namespace lengjing::game::native

#endif
