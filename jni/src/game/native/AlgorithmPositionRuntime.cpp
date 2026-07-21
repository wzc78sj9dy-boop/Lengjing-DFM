#include "game/native/AlgorithmPositionRuntime.h"

#include "game/native/AlgorithmPositionPolicy.h"
#include "game/native/MemoryTransport.h"

#include <unicorn/unicorn.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uint64_t kPageSize = 4096;
constexpr std::uint64_t kPageMask = ~(kPageSize - 1);
constexpr std::uint64_t kGuestStackBase = 0x80000000ULL;
constexpr std::uint64_t kGuestStackSize = 0x200000ULL;
constexpr std::uint64_t kGuestStackPointer = 0x801FE000ULL;
constexpr std::uint64_t kStopPc = 0x1337C0DE000ULL;
constexpr std::uint64_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uint64_t kMaximumRemoteAddress = 0x10000000000ULL;
constexpr std::size_t kInstructionBudget = 50000000;
constexpr auto kExecutionTimeout = std::chrono::milliseconds(800);
constexpr auto kAsyncResultLifetime = std::chrono::milliseconds(1500);

constexpr std::uint32_t kPacgaMask = 0xFFE0FC00U;
constexpr std::uint32_t kPacgaOpcode = 0x9AC03000U;
constexpr std::uint32_t kSvcMask = 0xFFE0001FU;
constexpr std::uint32_t kSvcOpcode = 0xD4000001U;

constexpr std::uint64_t kSysFcntl = 25;
constexpr std::uint64_t kSysIoctl = 29;
constexpr std::uint64_t kSysLseek = 62;
constexpr std::uint64_t kSysFutex = 98;
constexpr std::uint64_t kSysGetPid = 172;
constexpr std::uint64_t kSysGetTid = 178;
constexpr std::uint64_t kSysGetRandom = 278;

/*
 * ARM v8.3-PAuth Operations
 *
 * Copyright (c) 2019 Linaro, Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 */
std::uint64_t ExtractBits64(std::uint64_t value, int start, int length) {
    assert(start >= 0 && length > 0 && length <= 64 - start);
    return (value >> start) & (~0ULL >> (64 - length));
}

std::uint32_t ExtractBits32(std::uint32_t value, int start, int length) {
    assert(start >= 0 && length > 0 && length <= 32 - start);
    return (value >> start) & (~0U >> (32 - length));
}

std::uint64_t PacCellShuffle(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= ExtractBits64(value, 52, 4);
    result |= ExtractBits64(value, 24, 4) << 4;
    result |= ExtractBits64(value, 44, 4) << 8;
    result |= ExtractBits64(value, 0, 4) << 12;
    result |= ExtractBits64(value, 28, 4) << 16;
    result |= ExtractBits64(value, 48, 4) << 20;
    result |= ExtractBits64(value, 4, 4) << 24;
    result |= ExtractBits64(value, 40, 4) << 28;
    result |= ExtractBits64(value, 32, 4) << 32;
    result |= ExtractBits64(value, 12, 4) << 36;
    result |= ExtractBits64(value, 56, 4) << 40;
    result |= ExtractBits64(value, 20, 4) << 44;
    result |= ExtractBits64(value, 8, 4) << 48;
    result |= ExtractBits64(value, 36, 4) << 52;
    result |= ExtractBits64(value, 16, 4) << 56;
    result |= ExtractBits64(value, 60, 4) << 60;
    return result;
}

std::uint64_t PacCellInverseShuffle(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= ExtractBits64(value, 12, 4);
    result |= ExtractBits64(value, 24, 4) << 4;
    result |= ExtractBits64(value, 48, 4) << 8;
    result |= ExtractBits64(value, 36, 4) << 12;
    result |= ExtractBits64(value, 56, 4) << 16;
    result |= ExtractBits64(value, 44, 4) << 20;
    result |= ExtractBits64(value, 4, 4) << 24;
    result |= ExtractBits64(value, 16, 4) << 28;
    result |= value & (0xFULL << 32);
    result |= ExtractBits64(value, 52, 4) << 36;
    result |= ExtractBits64(value, 28, 4) << 40;
    result |= ExtractBits64(value, 8, 4) << 44;
    result |= ExtractBits64(value, 20, 4) << 48;
    result |= ExtractBits64(value, 0, 4) << 52;
    result |= ExtractBits64(value, 40, 4) << 56;
    result |= value & (0xFULL << 60);
    return result;
}

std::uint64_t PacSubstitute(std::uint64_t value) {
    static constexpr std::array<std::uint8_t, 16> kSubstitution{
        0xB, 0x6, 0x8, 0xF, 0xC, 0x0, 0x9, 0xE,
        0x3, 0x7, 0x4, 0x5, 0xD, 0x2, 0x1, 0xA,
    };
    std::uint64_t result = 0;
    for (int bit = 0; bit < 64; bit += 4) {
        result |= static_cast<std::uint64_t>(
                      kSubstitution[(value >> bit) & 0xF])
            << bit;
    }
    return result;
}

std::uint64_t PacInverseSubstitute(std::uint64_t value) {
    static constexpr std::array<std::uint8_t, 16> kSubstitution{
        0x5, 0xE, 0xD, 0x8, 0xA, 0xB, 0x1, 0x9,
        0x2, 0x6, 0xF, 0x0, 0x4, 0xC, 0x7, 0x3,
    };
    std::uint64_t result = 0;
    for (int bit = 0; bit < 64; bit += 4) {
        result |= static_cast<std::uint64_t>(
                      kSubstitution[(value >> bit) & 0xF])
            << bit;
    }
    return result;
}

int RotatePacCell(int cell, int amount) {
    cell |= cell << 4;
    return static_cast<int>(ExtractBits32(
        static_cast<std::uint32_t>(cell), 4 - amount, 4));
}

std::uint64_t PacMultiply(std::uint64_t value) {
    std::uint64_t result = 0;
    for (int bit = 0; bit < 16; bit += 4) {
        const int i0 = static_cast<int>(ExtractBits64(value, bit, 4));
        const int i4 = static_cast<int>(ExtractBits64(value, bit + 16, 4));
        const int i8 = static_cast<int>(ExtractBits64(value, bit + 32, 4));
        const int ic = static_cast<int>(ExtractBits64(value, bit + 48, 4));
        const int t0 = RotatePacCell(i8, 1) ^ RotatePacCell(i4, 2) ^
            RotatePacCell(i0, 1);
        const int t1 = RotatePacCell(ic, 1) ^ RotatePacCell(i4, 1) ^
            RotatePacCell(i0, 2);
        const int t2 = RotatePacCell(ic, 2) ^ RotatePacCell(i8, 1) ^
            RotatePacCell(i0, 1);
        const int t3 = RotatePacCell(ic, 1) ^ RotatePacCell(i8, 2) ^
            RotatePacCell(i4, 1);
        result |= static_cast<std::uint64_t>(t3) << bit;
        result |= static_cast<std::uint64_t>(t2) << (bit + 16);
        result |= static_cast<std::uint64_t>(t1) << (bit + 32);
        result |= static_cast<std::uint64_t>(t0) << (bit + 48);
    }
    return result;
}

std::uint64_t RotatePacTweakCell(std::uint64_t cell) {
    return (cell >> 1) | (((cell ^ (cell >> 1)) & 1) << 3);
}

std::uint64_t ShufflePacTweak(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= ExtractBits64(value, 16, 4);
    result |= ExtractBits64(value, 20, 4) << 4;
    result |= RotatePacTweakCell(ExtractBits64(value, 24, 4)) << 8;
    result |= ExtractBits64(value, 28, 4) << 12;
    result |= RotatePacTweakCell(ExtractBits64(value, 44, 4)) << 16;
    result |= ExtractBits64(value, 8, 4) << 20;
    result |= ExtractBits64(value, 12, 4) << 24;
    result |= RotatePacTweakCell(ExtractBits64(value, 32, 4)) << 28;
    result |= ExtractBits64(value, 48, 4) << 32;
    result |= ExtractBits64(value, 52, 4) << 36;
    result |= ExtractBits64(value, 56, 4) << 40;
    result |= RotatePacTweakCell(ExtractBits64(value, 60, 4)) << 44;
    result |= RotatePacTweakCell(ExtractBits64(value, 0, 4)) << 48;
    result |= ExtractBits64(value, 4, 4) << 52;
    result |= RotatePacTweakCell(ExtractBits64(value, 40, 4)) << 56;
    result |= RotatePacTweakCell(ExtractBits64(value, 36, 4)) << 60;
    return result;
}

std::uint64_t InverseRotatePacTweakCell(std::uint64_t cell) {
    return ((cell << 1) & 0xF) | ((cell & 1) ^ (cell >> 3));
}

std::uint64_t InverseShufflePacTweak(std::uint64_t value) {
    std::uint64_t result = 0;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 48, 4));
    result |= ExtractBits64(value, 52, 4) << 4;
    result |= ExtractBits64(value, 20, 4) << 8;
    result |= ExtractBits64(value, 24, 4) << 12;
    result |= ExtractBits64(value, 0, 4) << 16;
    result |= ExtractBits64(value, 4, 4) << 20;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 8, 4)) << 24;
    result |= ExtractBits64(value, 12, 4) << 28;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 28, 4)) << 32;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 60, 4)) << 36;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 56, 4)) << 40;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 16, 4)) << 44;
    result |= ExtractBits64(value, 32, 4) << 48;
    result |= ExtractBits64(value, 36, 4) << 52;
    result |= ExtractBits64(value, 40, 4) << 56;
    result |= InverseRotatePacTweakCell(ExtractBits64(value, 44, 4)) << 60;
    return result;
}

std::uint64_t ComputePacga(std::uint64_t data,
                           std::uint64_t modifier,
                           const AlgorithmPacgaKey& key) {
    static constexpr std::array<std::uint64_t, 5> kRoundConstants{
        0x0000000000000000ULL,
        0x13198A2E03707344ULL,
        0xA4093822299F31D0ULL,
        0x082EFA98EC4E6C89ULL,
        0x452821E638D01377ULL,
    };
    constexpr std::uint64_t kAlpha = 0xC0AC29B7C97C50DDULL;
    const std::uint64_t key0 = key.high;
    const std::uint64_t key1 = key.low;
    const std::uint64_t modifiedKey0 =
        (key0 << 63) | ((key0 >> 1) ^ (key0 >> 63));
    std::uint64_t runningModifier = modifier;
    std::uint64_t workingValue = data ^ key0;

    for (std::size_t round = 0; round <= 4; ++round) {
        workingValue ^= key1 ^ runningModifier;
        workingValue ^= kRoundConstants[round];
        if (round > 0) {
            workingValue = PacCellShuffle(workingValue);
            workingValue = PacMultiply(workingValue);
        }
        workingValue = PacSubstitute(workingValue);
        runningModifier = ShufflePacTweak(runningModifier);
    }

    workingValue ^= modifiedKey0 ^ runningModifier;
    workingValue = PacCellShuffle(workingValue);
    workingValue = PacMultiply(workingValue);
    workingValue = PacSubstitute(workingValue);
    workingValue = PacCellShuffle(workingValue);
    workingValue = PacMultiply(workingValue);
    workingValue ^= key1;
    workingValue = PacCellInverseShuffle(workingValue);
    workingValue = PacInverseSubstitute(workingValue);
    workingValue = PacMultiply(workingValue);
    workingValue = PacCellInverseShuffle(workingValue);
    workingValue ^= key0;
    workingValue ^= runningModifier;

    for (std::size_t round = 0; round <= 4; ++round) {
        workingValue = PacInverseSubstitute(workingValue);
        if (round < 4) {
            workingValue = PacMultiply(workingValue);
            workingValue = PacCellInverseShuffle(workingValue);
        }
        runningModifier = InverseShufflePacTweak(runningModifier);
        workingValue ^= kRoundConstants[4 - round];
        workingValue ^= key1 ^ runningModifier;
        workingValue ^= kAlpha;
    }
    return workingValue ^ modifiedKey0;
}

int XRegisterId(std::uint32_t index) {
    if (index <= 28) {
        return UC_ARM64_REG_X0 + static_cast<int>(index);
    }
    if (index == 29) return UC_ARM64_REG_X29;
    if (index == 30) return UC_ARM64_REG_X30;
    return UC_ARM64_REG_XZR;
}

std::uint64_t ReadHostCtrEl0() {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

std::uint64_t ReadHostCounterFrequency() {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

std::uint64_t ReadHostVirtualCounter() {
#if defined(__aarch64__)
    std::uint64_t value = 0;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
#else
    return static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

bool IsFinitePosition(const AlgorithmPosition& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) &&
        (position.x != 0.0f || position.y != 0.0f ||
         position.z != 0.0f);
}

}  // namespace

std::uint64_t ComputeAlgorithmPositionPacga(
    std::uint64_t data,
    std::uint64_t modifier,
    const AlgorithmPacgaKey& key) noexcept {
    return FormatAlgorithmPacgaResult(ComputePacga(data, modifier, key));
}

struct AlgorithmPositionRuntime::Impl {
    struct CachedPage {
        std::uint64_t guestAddress = 0;
        std::uint64_t remoteAddress = 0;
        std::vector<uc_hook> instructionHooks;
    };

    struct PendingExecution {
        MemoryTransport* memory = nullptr;
        std::uintptr_t guestPc = 0;
        std::uintptr_t entityAddress = 0;
        std::uint64_t tpidrEl0 = 0;
        AlgorithmPacgaKey pacgaKey{};
        std::uint64_t generation = 0;
        bool refreshCachedPages = true;
    };

    struct CompletedExecution {
        AlgorithmPosition position{};
        std::chrono::steady_clock::time_point updatedAt{};
    };

    ~Impl() {
        Reset();
    }

    bool Execute(MemoryTransport& memory,
                 std::uintptr_t moduleBase,
                 std::uintptr_t decryptRva,
                 std::uintptr_t entityAddress,
                 std::uint64_t tpidrEl0,
                 const AlgorithmPacgaKey& pacgaKey,
                 AlgorithmPosition& position,
                 bool refreshCachedPages) noexcept {
        std::uint64_t guestPc = 0;
        if (!ResolveRelativeEntry(moduleBase, decryptRva, guestPc)) {
            position = {};
            return false;
        }
        return ExecuteAtGuestPc(
            memory,
            static_cast<std::uintptr_t>(guestPc),
            entityAddress,
            tpidrEl0,
            pacgaKey,
            position,
            refreshCachedPages);
    }

    bool ExecuteAtGuestPc(MemoryTransport& memory,
                          std::uintptr_t guestPc,
                          std::uintptr_t entityAddress,
                          std::uint64_t tpidrEl0,
                          const AlgorithmPacgaKey& pacgaKey,
                          AlgorithmPosition& position,
                          bool refreshCachedPages) noexcept {
        position = {};
        try {
            if (!memory.IsOpen() ||
                !ValidateExecutionTarget(guestPc, entityAddress)) {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (stopping_) return false;
            EnsureWorkerLocked();
            UpdateExecutionContextLocked(
                memory, guestPc, tpidrEl0, pacgaKey);
            if (refreshCachedPages) pageRefreshRequested_ = true;

            PendingExecution execution{
                &memory,
                guestPc,
                entityAddress,
                tpidrEl0,
                pacgaKey,
                generation_,
                false,
            };
            const auto pending = pendingExecutions_.find(entityAddress);
            if (pending == pendingExecutions_.end()) {
                if (pendingExecutions_.size() >=
                    kAlgorithmPositionMaximumCachedPages) {
                    DropOldestPendingLocked();
                }
                pendingOrder_.push_back(entityAddress);
                pendingExecutions_.emplace(entityAddress, execution);
            } else {
                pending->second = execution;
            }
            queueReady_.notify_one();

            const auto completed = completedExecutions_.find(entityAddress);
            if (completed == completedExecutions_.end()) {
                return false;
            }
            if (now - completed->second.updatedAt > kAsyncResultLifetime) {
                completedExecutions_.erase(completed);
                return false;
            }
            position = completed->second.position;
            return true;
        } catch (...) {
            position = {};
            return false;
        }
    }

    void Reset() noexcept {
        std::lock_guard<std::mutex> resetLock(resetMutex_);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopping_ = true;
            resetRequested_.store(true, std::memory_order_release);
            AdvanceGenerationLocked();
            pendingOrder_.clear();
            pendingExecutions_.clear();
            completedOrder_.clear();
            completedExecutions_.clear();
            pageRefreshRequested_ = false;
            ClearExecutionContextLocked();
            StopActiveEmulationLocked();
            queueReady_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
        CloseEngine();
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopping_ = false;
            resetRequested_.store(false, std::memory_order_release);
        }
    }

    void Invalidate() noexcept {
        std::lock_guard<std::mutex> resetLock(resetMutex_);
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (stopping_) return;
        AdvanceGenerationLocked();
        pendingOrder_.clear();
        pendingExecutions_.clear();
        completedOrder_.clear();
        completedExecutions_.clear();
        pageRefreshRequested_ = false;
        ClearExecutionContextLocked();
        StopActiveEmulationLocked();
    }

private:
    void AdvanceGenerationLocked() noexcept {
        ++generation_;
        publishedGeneration_.store(generation_, std::memory_order_release);
    }

    void ClearExecutionContextLocked() noexcept {
        executionContextMemory_ = nullptr;
        executionContextGuestPc_ = 0;
        executionContextTpidrEl0_ = 0;
        executionContextPacgaKey_ = {};
    }

    bool IsExecutionCurrent(std::uint64_t generation) const noexcept {
        return !resetRequested_.load(std::memory_order_acquire) &&
            publishedGeneration_.load(std::memory_order_acquire) == generation;
    }

    void StopActiveEmulationLocked() noexcept {
        std::lock_guard<std::mutex> executionLock(executionControlMutex_);
        if (emulationRunning_ && engine_ != nullptr) {
            static_cast<void>(uc_emu_stop(engine_));
        }
    }

    void EnsureWorkerLocked() {
        if (!worker_.joinable()) {
            worker_ = std::thread([this]() { WorkerMain(); });
        }
    }

    void UpdateExecutionContextLocked(
        MemoryTransport& memory,
        std::uintptr_t guestPc,
        std::uint64_t tpidrEl0,
        const AlgorithmPacgaKey& pacgaKey) {
        const bool changed = executionContextMemory_ != &memory ||
            executionContextGuestPc_ != guestPc ||
            executionContextTpidrEl0_ != tpidrEl0 ||
            executionContextPacgaKey_.low != pacgaKey.low ||
            executionContextPacgaKey_.high != pacgaKey.high;
        if (!changed) return;

        executionContextMemory_ = &memory;
        executionContextGuestPc_ = guestPc;
        executionContextTpidrEl0_ = tpidrEl0;
        executionContextPacgaKey_ = pacgaKey;
        AdvanceGenerationLocked();
        pendingOrder_.clear();
        pendingExecutions_.clear();
        completedOrder_.clear();
        completedExecutions_.clear();
        pageRefreshRequested_ = false;
    }

    void DropOldestPendingLocked() {
        while (!pendingOrder_.empty()) {
            const std::uintptr_t entityAddress = pendingOrder_.front();
            pendingOrder_.pop_front();
            if (pendingExecutions_.erase(entityAddress) != 0) return;
        }
    }

    void PruneCompletedLocked(
        std::chrono::steady_clock::time_point now) {
        while (!completedOrder_.empty()) {
            const auto oldest = completedOrder_.front();
            const auto completed = completedExecutions_.find(oldest.first);
            const bool current = completed != completedExecutions_.end() &&
                completed->second.updatedAt == oldest.second;
            const bool expired = current &&
                now - completed->second.updatedAt > kAsyncResultLifetime;
            const bool overLimit = current &&
                completedExecutions_.size() >=
                    kAlgorithmPositionMaximumCachedPages;
            if (current && !expired && !overLimit) return;
            if (current) completedExecutions_.erase(completed);
            completedOrder_.pop_front();
        }
    }

    void WorkerMain() noexcept {
        while (true) {
            PendingExecution execution{};
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                queueReady_.wait(lock, [this]() {
                    return stopping_ || !pendingOrder_.empty();
                });
                if (stopping_) return;

                while (!pendingOrder_.empty()) {
                    const std::uintptr_t entityAddress = pendingOrder_.front();
                    pendingOrder_.pop_front();
                    const auto pending =
                        pendingExecutions_.find(entityAddress);
                    if (pending == pendingExecutions_.end()) continue;
                    execution = pending->second;
                    pendingExecutions_.erase(pending);
                    execution.refreshCachedPages = pageRefreshRequested_;
                    pageRefreshRequested_ = false;
                    break;
                }
                if (execution.memory == nullptr) continue;
            }

            AlgorithmPosition candidate{};
            const bool succeeded = ExecuteSynchronously(
                *execution.memory,
                execution.guestPc,
                execution.entityAddress,
                execution.tpidrEl0,
                execution.pacgaKey,
                execution.generation,
                execution.refreshCachedPages,
                candidate);
            if (!succeeded) continue;

            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (!stopping_ && execution.generation == generation_) {
                try {
                    PruneCompletedLocked(now);
                    completedExecutions_[execution.entityAddress] =
                        CompletedExecution{candidate, now};
                    completedOrder_.emplace_back(
                        execution.entityAddress, now);
                } catch (...) {
                    completedExecutions_.erase(execution.entityAddress);
                }
            }
        }
    }

    bool ExecuteSynchronously(
        MemoryTransport& memory,
        std::uintptr_t guestPc,
        std::uintptr_t entityAddress,
        std::uint64_t tpidrEl0,
        const AlgorithmPacgaKey& pacgaKey,
        std::uint64_t generation,
        bool refreshCachedPages,
        AlgorithmPosition& position) noexcept {
        position = {};
        try {
            if (!IsExecutionCurrent(generation)) return false;
            if (workerGeneration_ != generation) {
                positionCache_.Clear();
                workerGeneration_ = generation;
            }
            if (!memory.IsOpen() ||
                !ValidateExecutionTarget(guestPc, entityAddress) ||
                !EnsureEngine(memory) ||
                !ClearCoordinatePagesIfOverLimit()) {
                return false;
            }
            if (!IsExecutionCurrent(generation)) return false;

            AlgorithmPosition first{};
            const AlgorithmPositionRefreshPlan refreshPlan =
                MakeAlgorithmPositionRefreshPlan(refreshCachedPages);
            if (!ExecuteOnce(
                    memory,
                    guestPc,
                    entityAddress,
                    tpidrEl0,
                    pacgaKey,
                    generation,
                    refreshPlan.first,
                    first)) {
                return false;
            }

            const auto historyNow =
                AlgorithmPositionResultCache::Clock::now();
            AlgorithmPositionHistorySample history{};
            const bool hasHistory = positionCache_.Lookup(
                entityAddress, historyNow, history);

            AlgorithmPosition selected = first;
            if (hasHistory &&
                EvaluateAlgorithmPositionFirst(history.position, first) ==
                    AlgorithmPositionFirstDecision::Rerun) {
                AlgorithmPosition discarded{};
                if (!ExecuteOnce(
                    memory,
                    guestPc,
                    entityAddress,
                    tpidrEl0,
                    pacgaKey,
                    generation,
                    refreshPlan.discarded,
                    discarded)) {
                    return false;
                }
                AlgorithmPosition candidate{};
                const bool hasCandidate = ExecuteOnce(
                    memory,
                    guestPc,
                    entityAddress,
                    tpidrEl0,
                    pacgaKey,
                    generation,
                    refreshPlan.candidate,
                    candidate);
                if (EvaluateAlgorithmPositionSecond(
                        history.position,
                        first,
                        hasCandidate ? &candidate : nullptr) ==
                    AlgorithmPositionSecondDecision::AcceptSecond) {
                    selected = candidate;
                } else {
                    selected = history.position;
                }
            }

            position = selected;
            positionCache_.Store(
                entityAddress,
                selected,
                AlgorithmPositionResultCache::Clock::now());
            return true;
        } catch (...) {
            ClearActiveExecution();
            position = {};
            return false;
        }
    }

    bool ExecuteOnce(MemoryTransport& memory,
                     std::uint64_t entry,
                     std::uint64_t entityAddress,
                     std::uint64_t tpidrEl0,
                     const AlgorithmPacgaKey& pacgaKey,
                     std::uint64_t generation,
                     bool refreshCachedPages,
                     AlgorithmPosition& position) {
        position = {};
        if (!IsExecutionCurrent(generation)) return false;
        const auto startedAt = std::chrono::steady_clock::now();
        activeMemory_ = &memory;
        activePacgaKey_ = pacgaKey;
        activeTpidrEl0_ = tpidrEl0;
        activeExecutionGeneration_ = generation;
        hookFailed_ = false;

        if ((refreshCachedPages &&
             !RefreshCachedPages(memory, generation)) ||
            !PrepareRegisters(entry, entityAddress, tpidrEl0)) {
            ClearActiveExecution();
            return false;
        }
        if (!IsExecutionCurrent(generation)) {
            ClearActiveExecution();
            return false;
        }

        const auto deadline = startedAt + kExecutionTimeout;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            ClearActiveExecution();
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(
                deadline - now);
        const std::uint64_t timeout = static_cast<std::uint64_t>(
            std::max<std::int64_t>(1, remaining.count()));
        {
            std::lock_guard<std::mutex> lock(executionControlMutex_);
            if (!IsExecutionCurrent(generation)) {
                ClearActiveExecution();
                return false;
            }
            emulationRunning_ = true;
        }
        const uc_err error = uc_emu_start(
            engine_, entry, kStopPc, timeout, kInstructionBudget);
        {
            std::lock_guard<std::mutex> lock(executionControlMutex_);
            emulationRunning_ = false;
        }

        std::uint64_t pc = 0;
        const bool stoppedAtReturn =
            uc_reg_read(engine_, UC_ARM64_REG_PC, &pc) == UC_ERR_OK &&
            pc == kStopPc;
        const bool finishedInTime =
            std::chrono::steady_clock::now() <= deadline;
        if (error != UC_ERR_OK || hookFailed_ || !stoppedAtReturn ||
            !finishedInTime) {
            ClearActiveExecution();
            return false;
        }

        AlgorithmPosition candidate{};
        const bool valid =
            ReadPosition(candidate) && IsFinitePosition(candidate);
        ClearActiveExecution();
        if (!valid) return false;
        position = candidate;
        return true;
    }

    static bool MemoryFaultHook(uc_engine*,
                                uc_mem_type,
                                std::uint64_t address,
                                int,
                                std::int64_t,
                                void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr) return false;
        try {
            const bool loaded = self->LoadRemotePage(address);
            if (!loaded) self->hookFailed_ = true;
            return loaded;
        } catch (...) {
            self->hookFailed_ = true;
            return false;
        }
    }

    static void CodeHook(uc_engine*,
                         std::uint64_t address,
                         std::uint32_t,
                         void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || self->hookFailed_) return;
        if (!self->IsExecutionCurrent(self->activeExecutionGeneration_)) {
            self->FailHook();
            return;
        }
        self->HandleInstruction(address);
    }

    static std::uint32_t MrsHook(uc_engine* engine,
                                 uc_arm64_reg destination,
                                 const uc_arm64_cp_reg* systemRegister,
                                 void* userData) {
        auto* self = static_cast<Impl*>(userData);
        if (self == nullptr || systemRegister == nullptr) return 0;
        std::uint64_t value = 0;
        if (systemRegister->op0 != 3 || systemRegister->op1 != 3) {
            return 0;
        }
        if (systemRegister->crn == 0 && systemRegister->crm == 0 &&
            systemRegister->op2 == 1) {
            value = ReadHostCtrEl0();
        } else if (systemRegister->crn == 13 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 2) {
            value = self->activeTpidrEl0_;
        } else if (systemRegister->crn == 14 &&
                   systemRegister->crm == 0 &&
                   systemRegister->op2 == 0) {
            value = ReadHostCounterFrequency();
        } else if (systemRegister->crn == 14 &&
                   systemRegister->crm == 0 &&
                   (systemRegister->op2 == 2 || systemRegister->op2 == 6)) {
            value = ReadHostVirtualCounter();
        } else {
            return 0;
        }
        if (uc_reg_write(engine, destination, &value) != UC_ERR_OK) {
            self->FailHook();
        }
        return 1;
    }

    bool ResolveRelativeEntry(std::uintptr_t moduleBase,
                              std::uintptr_t decryptRva,
                              std::uint64_t& entry) const {
        if (!AlgorithmPositionRuntimeConfig{decryptRva}.IsConfigured() ||
            !IsRemoteAddress(NormalizeAlgorithmPositionRemoteAddress(
                moduleBase)) ||
            decryptRva > std::numeric_limits<std::uintptr_t>::max() -
                moduleBase) {
            return false;
        }
        entry = moduleBase + decryptRva;
        return ValidateGuestPc(entry);
    }

    static bool ValidateExecutionTarget(std::uint64_t guestPc,
                                        std::uint64_t entityAddress) {
        return ValidateGuestPc(guestPc) &&
            IsRemoteAddress(NormalizeAlgorithmPositionRemoteAddress(
                entityAddress));
    }

    static bool ValidateGuestPc(std::uint64_t guestPc) {
        return (guestPc & 3U) == 0 &&
            IsRemoteAddress(NormalizeAlgorithmPositionRemoteAddress(
                guestPc));
    }

    static bool IsRemoteAddress(std::uint64_t address) {
        return address >= kMinimumRemoteAddress &&
            address < kMaximumRemoteAddress;
    }

    static bool RequiresInstructionHook(std::uint32_t instruction) noexcept {
        return (instruction & kPacgaMask) == kPacgaOpcode ||
            (instruction & kSvcMask) == kSvcOpcode;
    }

    bool RemovePageInstructionHooks(CachedPage& page) noexcept {
        bool removed = true;
        if (engine_ != nullptr) {
            for (const uc_hook hook : page.instructionHooks) {
                removed = uc_hook_del(engine_, hook) == UC_ERR_OK && removed;
            }
        }
        page.instructionHooks.clear();
        return removed;
    }

    bool InstallPageInstructionHooks(CachedPage& page,
                                     const std::uint8_t* bytes,
                                     std::size_t size) {
        if (engine_ == nullptr || bytes == nullptr || size < 4 ||
            page.guestAddress >
                std::numeric_limits<std::uint64_t>::max() - size) {
            return false;
        }

        std::vector<std::uint64_t> addresses;
        addresses.reserve(size / 256 + 4);
        for (std::size_t offset = 0; offset <= size - 4; offset += 4) {
            std::uint32_t instruction = 0;
            std::memcpy(&instruction, bytes + offset, sizeof(instruction));
            if (RequiresInstructionHook(instruction)) {
                addresses.push_back(page.guestAddress + offset);
            }
        }

        page.instructionHooks.reserve(addresses.size());
        for (const std::uint64_t address : addresses) {
            uc_hook hook = 0;
            if (uc_hook_add(
                    engine_,
                    &hook,
                    UC_HOOK_CODE,
                    reinterpret_cast<void*>(CodeHook),
                    this,
                    address,
                    address) != UC_ERR_OK) {
                static_cast<void>(RemovePageInstructionHooks(page));
                return false;
            }
            page.instructionHooks.push_back(hook);
        }
        return true;
    }

    bool EnsureEngine(MemoryTransport& memory) {
        if (engine_ != nullptr && activeContextMemory_ == &memory) {
            return true;
        }
        CloseEngine();
        if (uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &engine_) != UC_ERR_OK) {
            engine_ = nullptr;
            return false;
        }
        static_cast<void>(
            uc_ctl_set_cpu_model(engine_, UC_CPU_ARM64_MAX));
        if (uc_mem_map(
                engine_,
                kGuestStackBase,
                kGuestStackSize,
                UC_PROT_ALL) != UC_ERR_OK ||
            uc_hook_add(
                engine_,
                &memoryHook_,
                UC_HOOK_MEM_READ_UNMAPPED |
                    UC_HOOK_MEM_WRITE_UNMAPPED |
                    UC_HOOK_MEM_FETCH_UNMAPPED,
                reinterpret_cast<void*>(MemoryFaultHook),
                this,
                1,
                0) != UC_ERR_OK ||
            uc_hook_add(
                engine_,
                &mrsHook_,
                UC_HOOK_INSN,
                reinterpret_cast<void*>(MrsHook),
                this,
                1,
                0,
                UC_ARM64_INS_MRS) != UC_ERR_OK ||
            uc_context_alloc(engine_, &baseline_) != UC_ERR_OK ||
            uc_context_save(engine_, baseline_) != UC_ERR_OK) {
            CloseEngine();
            return false;
        }
        activeContextMemory_ = &memory;
        return true;
    }

    void CloseEngine() noexcept {
        ClearActiveExecution();
        cachedPages_.clear();
        positionCache_.Clear();
        if (baseline_ != nullptr) {
            static_cast<void>(uc_context_free(baseline_));
            baseline_ = nullptr;
        }
        if (engine_ != nullptr) {
            static_cast<void>(uc_close(engine_));
            engine_ = nullptr;
        }
        activeContextMemory_ = nullptr;
        memoryHook_ = 0;
        mrsHook_ = 0;
    }

    bool PrepareRegisters(std::uint64_t entry,
                          std::uint64_t entityAddress,
                          std::uint64_t tpidrEl0) {
        if (baseline_ == nullptr ||
            uc_context_restore(engine_, baseline_) != UC_ERR_OK) {
            return false;
        }

        const std::uint64_t zero = 0;
        for (std::uint32_t index = 0; index <= 29; ++index) {
            const std::uint64_t value = index == 0 ? entityAddress : zero;
            if (uc_reg_write(engine_, XRegisterId(index), &value) !=
                UC_ERR_OK) {
                return false;
            }
        }
        if (uc_reg_write(engine_, UC_ARM64_REG_LR, &kStopPc) != UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_SP, &kGuestStackPointer) !=
                UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_TPIDR_EL0, &tpidrEl0) !=
                UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_NZCV, &zero) != UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_FPCR, &zero) != UC_ERR_OK ||
            uc_reg_write(engine_, UC_ARM64_REG_FPSR, &zero) != UC_ERR_OK) {
            return false;
        }

        const std::array<std::uint8_t, 16> zeroVector{};
        for (int index = 0; index < 32; ++index) {
            if (uc_reg_write(
                    engine_,
                    UC_ARM64_REG_V0 + index,
                    zeroVector.data()) != UC_ERR_OK) {
                return false;
            }
        }
        if (uc_mem_write(
                engine_,
                kGuestStackPointer,
                zeroVector.data(),
                zeroVector.size()) != UC_ERR_OK) {
            return false;
        }
        return uc_reg_write(engine_, UC_ARM64_REG_PC, &entry) == UC_ERR_OK;
    }

    bool LoadRemotePage(std::uint64_t faultAddress) {
        if (activeMemory_ == nullptr || engine_ == nullptr ||
            !IsExecutionCurrent(activeExecutionGeneration_)) {
            return false;
        }
        const std::uint64_t guestPage = faultAddress & kPageMask;
        if (guestPage >= kGuestStackBase &&
            guestPage < kGuestStackBase + kGuestStackSize) {
            return true;
        }
        if (FindCachedPage(guestPage) != cachedPages_.end()) return true;

        const std::uint64_t remotePage =
            NormalizeAlgorithmPositionRemoteAddress(guestPage) & kPageMask;
        if (!IsRemoteAddress(remotePage) ||
            remotePage > kMaximumRemoteAddress - kPageSize) {
            return false;
        }

        std::array<std::uint8_t, kPageSize> data{};
        if (uc_mem_map(engine_, guestPage, kPageSize, UC_PROT_ALL) !=
            UC_ERR_OK) {
            return false;
        }
        if (!activeMemory_->Read(remotePage, data.data(), data.size()) ||
            uc_mem_write(engine_, guestPage, data.data(), data.size()) !=
                UC_ERR_OK) {
            static_cast<void>(uc_mem_unmap(engine_, guestPage, kPageSize));
            return false;
        }
        CachedPage page{guestPage, remotePage, {}};
        if (!InstallPageInstructionHooks(page, data.data(), data.size())) {
            static_cast<void>(uc_mem_unmap(engine_, guestPage, kPageSize));
            return false;
        }
        try {
            cachedPages_.push_back(std::move(page));
        } catch (...) {
            static_cast<void>(RemovePageInstructionHooks(page));
            static_cast<void>(uc_mem_unmap(engine_, guestPage, kPageSize));
            throw;
        }
        return TrimCachedPages();
    }

    bool TrimCachedPages() {
        if (cachedPages_.size() <= kAlgorithmPositionMaximumCachedPages) {
            return true;
        }
        while (cachedPages_.size() >
               kAlgorithmPositionRetainedCachedPages) {
            CachedPage& page = cachedPages_.front();
            if (!RemovePageInstructionHooks(page) ||
                uc_mem_unmap(engine_, page.guestAddress, kPageSize) !=
                    UC_ERR_OK) {
                return false;
            }
            cachedPages_.pop_front();
        }
        return true;
    }

    bool ClearCoordinatePagesIfOverLimit() {
        if (!ShouldClearAlgorithmPositionCoordinatePages(
                cachedPages_.size())) {
            return true;
        }
        while (!cachedPages_.empty()) {
            CachedPage& page = cachedPages_.front();
            if (!RemovePageInstructionHooks(page) ||
                uc_mem_unmap(engine_, page.guestAddress, kPageSize) !=
                    UC_ERR_OK) {
                return false;
            }
            cachedPages_.pop_front();
        }
        return true;
    }

    bool RefreshCachedPages(MemoryTransport& memory,
                            std::uint64_t generation) {
        std::vector<std::uint8_t> data;
        std::vector<MemoryReadRequest> requests;
        std::vector<std::uint8_t> status;
        data.reserve(kAlgorithmPositionRefreshBatchSize * kPageSize);
        requests.reserve(kAlgorithmPositionRefreshBatchSize);
        status.reserve(kAlgorithmPositionRefreshBatchSize);
        for (std::size_t begin = 0; begin < cachedPages_.size();
             begin += kAlgorithmPositionRefreshBatchSize) {
            if (!IsExecutionCurrent(generation)) return false;
            const std::size_t end = std::min(
                cachedPages_.size(),
                begin + kAlgorithmPositionRefreshBatchSize);
            const std::size_t count = end - begin;
            data.resize(count * kPageSize);
            requests.clear();
            status.assign(count, 0);
            for (std::size_t index = begin; index < end; ++index) {
                const CachedPage& page = cachedPages_[index];
                requests.push_back(MemoryReadRequest{
                    static_cast<std::uintptr_t>(page.remoteAddress),
                    data.data() + (index - begin) * kPageSize,
                    kPageSize,
                });
            }
            const std::size_t refreshed = memory.ReadBatch(
                requests.data(), requests.size(), status.data());
            if (refreshed != requests.size()) return false;
            for (std::size_t index = begin; index < end; ++index) {
                if (status[index - begin] == 0) return false;
                const CachedPage& page = cachedPages_[index];
                if (uc_mem_write(
                        engine_,
                        page.guestAddress,
                        data.data() + (index - begin) * kPageSize,
                        kPageSize) != UC_ERR_OK) {
                    return false;
                }
            }
        }
        return true;
    }

    std::deque<CachedPage>::iterator FindCachedPage(
        std::uint64_t guestPage) {
        return std::find_if(
            cachedPages_.begin(),
            cachedPages_.end(),
            [guestPage](const CachedPage& page) {
                return page.guestAddress == guestPage;
            });
    }

    bool EnsureGuestRange(std::uint64_t address, std::size_t size) {
        if (size == 0) return true;
        if (address > std::numeric_limits<std::uint64_t>::max() -
                (size - 1)) {
            return false;
        }
        std::uint64_t page = address & kPageMask;
        const std::uint64_t lastPage = (address + size - 1) & kPageMask;
        while (true) {
            const bool stackPage = page >= kGuestStackBase &&
                page < kGuestStackBase + kGuestStackSize;
            if (!stackPage && FindCachedPage(page) == cachedPages_.end() &&
                !LoadRemotePage(page)) {
                return false;
            }
            if (page == lastPage) break;
            if (page > std::numeric_limits<std::uint64_t>::max() -
                    kPageSize) {
                return false;
            }
            page += kPageSize;
        }
        return true;
    }

    bool WriteGuestMemory(std::uint64_t address,
                          const void* data,
                          std::size_t size) {
        return data != nullptr && EnsureGuestRange(address, size) &&
            uc_mem_write(engine_, address, data, size) == UC_ERR_OK;
    }

    void HandleInstruction(std::uint64_t address) noexcept {
        std::uint32_t instruction = 0;
        if (uc_mem_read(
                engine_, address, &instruction, sizeof(instruction)) !=
            UC_ERR_OK) {
            FailHook();
            return;
        }
        if ((instruction & kPacgaMask) == kPacgaOpcode) {
            HandlePacga(address, instruction);
        } else if ((instruction & kSvcMask) == kSvcOpcode) {
            HandleSvc(address);
        }
    }

    void HandlePacga(std::uint64_t address,
                     std::uint32_t instruction) noexcept {
        const std::uint32_t destination = instruction & 0x1FU;
        const std::uint32_t source = (instruction >> 5U) & 0x1FU;
        const std::uint32_t modifier = (instruction >> 16U) & 0x1FU;
        std::uint64_t sourceValue = 0;
        std::uint64_t modifierValue = 0;
        if (!ReadXRegister(source, sourceValue) ||
            !ReadXRegister(modifier, modifierValue)) {
            FailHook();
            return;
        }
        const std::uint64_t result = FormatAlgorithmPacgaResult(
            ComputePacga(sourceValue, modifierValue, activePacgaKey_));
        if (!WriteXRegister(destination, result) ||
            !SkipInstruction(address)) {
            FailHook();
        }
    }

    void HandleSvc(std::uint64_t address) noexcept {
        std::uint64_t number = 0;
        if (!ReadXRegister(8, number)) {
            FailHook();
            return;
        }
        if (!IsSupportedAlgorithmPositionSvc(number)) {
            FailHook();
            return;
        }

        std::uint64_t result = 0;
        bool success = true;
        if (number == kSysFcntl) {
            std::uint64_t command = 0;
            success = ReadXRegister(1, command);
            result = command == 1 ? 1 : 0;
        } else if (number == kSysFutex) {
            std::uint64_t addressArgument = 0;
            std::uint64_t operation = 0;
            success = ReadXRegister(0, addressArgument) &&
                ReadXRegister(1, operation);
            const std::uint64_t kind = operation & 0x7FU;
            if (success && (kind == 0 || kind == 9)) {
                const std::uint32_t zero = 0;
                success = WriteGuestMemory(
                    addressArgument, &zero, sizeof(zero));
                result = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(-11));
            }
        } else if (number == kSysGetPid || number == kSysGetTid) {
            result = 1000;
        } else if (number == kSysGetRandom) {
            std::uint64_t destination = 0;
            std::uint64_t requested = 0;
            success = ReadXRegister(0, destination) &&
                ReadXRegister(1, requested);
            const std::size_t count = static_cast<std::size_t>(
                std::min<std::uint64_t>(requested, 16));
            std::array<std::uint8_t, 16> randomBytes{};
            randomBytes.fill(66);
            if (success && count != 0) {
                success = WriteGuestMemory(
                    destination, randomBytes.data(), count);
            }
            result = count;
        } else if (number == kSysIoctl || number == kSysLseek) {
            result = 0;
        }

        if (!success || !WriteXRegister(0, result) ||
            !SkipInstruction(address)) {
            FailHook();
        }
    }

    bool ReadXRegister(std::uint32_t index,
                       std::uint64_t& value) const {
        value = 0;
        return index == 31 ||
            uc_reg_read(engine_, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool WriteXRegister(std::uint32_t index, std::uint64_t value) {
        return index == 31 ||
            uc_reg_write(engine_, XRegisterId(index), &value) == UC_ERR_OK;
    }

    bool SkipInstruction(std::uint64_t address) {
        if (address > std::numeric_limits<std::uint64_t>::max() - 4) {
            return false;
        }
        const std::uint64_t next = address + 4;
        return uc_reg_write(engine_, UC_ARM64_REG_PC, &next) == UC_ERR_OK;
    }

    void FailHook() noexcept {
        hookFailed_ = true;
        if (engine_ != nullptr) {
            static_cast<void>(uc_emu_stop(engine_));
        }
    }

    bool ReadPosition(AlgorithmPosition& position) const {
        std::array<std::uint8_t, 16> vector{};
        std::array<float, 3> values{};
        for (int index = 0; index < 3; ++index) {
            vector.fill(0);
            if (uc_reg_read(
                    engine_,
                    UC_ARM64_REG_V0 + index,
                    vector.data()) != UC_ERR_OK) {
                return false;
            }
            std::memcpy(
                &values[static_cast<std::size_t>(index)],
                vector.data(),
                sizeof(float));
        }
        position = AlgorithmPosition{values[0], values[1], values[2]};
        return true;
    }

    void ClearActiveExecution() noexcept {
        activeMemory_ = nullptr;
        activePacgaKey_ = {};
        activeTpidrEl0_ = 0;
        activeExecutionGeneration_ = 0;
        hookFailed_ = false;
    }

    std::mutex resetMutex_;
    std::mutex queueMutex_;
    std::mutex executionControlMutex_;
    std::condition_variable queueReady_;
    std::thread worker_;
    std::deque<std::uintptr_t> pendingOrder_;
    std::unordered_map<std::uintptr_t, PendingExecution> pendingExecutions_;
    bool pageRefreshRequested_ = false;
    std::deque<std::pair<
        std::uintptr_t,
        std::chrono::steady_clock::time_point>> completedOrder_;
    std::unordered_map<std::uintptr_t, CompletedExecution>
        completedExecutions_;
    MemoryTransport* executionContextMemory_ = nullptr;
    std::uintptr_t executionContextGuestPc_ = 0;
    std::uint64_t executionContextTpidrEl0_ = 0;
    AlgorithmPacgaKey executionContextPacgaKey_{};
    std::uint64_t generation_ = 0;
    std::atomic<std::uint64_t> publishedGeneration_{0};
    std::atomic_bool resetRequested_{false};
    std::uint64_t workerGeneration_ = 0;
    bool stopping_ = false;
    bool emulationRunning_ = false;
    uc_engine* engine_ = nullptr;
    uc_context* baseline_ = nullptr;
    uc_hook memoryHook_ = 0;
    uc_hook mrsHook_ = 0;
    std::deque<CachedPage> cachedPages_;
    AlgorithmPositionResultCache positionCache_;
    MemoryTransport* activeContextMemory_ = nullptr;
    MemoryTransport* activeMemory_ = nullptr;
    AlgorithmPacgaKey activePacgaKey_{};
    std::uint64_t activeTpidrEl0_ = 0;
    std::uint64_t activeExecutionGeneration_ = 0;
    bool hookFailed_ = false;
};

AlgorithmPositionRuntime::AlgorithmPositionRuntime()
    : impl_(std::make_unique<Impl>()) {}

AlgorithmPositionRuntime::~AlgorithmPositionRuntime() = default;

bool AlgorithmPositionRuntime::Execute(
    MemoryTransport& memory,
    std::uintptr_t moduleBase,
    std::uintptr_t decryptRva,
    std::uintptr_t entityAddress,
    std::uint64_t tpidrEl0,
    const AlgorithmPacgaKey& pacgaKey,
    AlgorithmPosition& position,
    bool refreshCachedPages) noexcept {
    return impl_ != nullptr && impl_->Execute(
        memory,
        moduleBase,
        decryptRva,
        entityAddress,
        tpidrEl0,
        pacgaKey,
        position,
        refreshCachedPages);
}

bool AlgorithmPositionRuntime::ExecuteAtGuestPc(
    MemoryTransport& memory,
    std::uintptr_t guestPc,
    std::uintptr_t entityAddress,
    std::uint64_t tpidrEl0,
    const AlgorithmPacgaKey& pacgaKey,
    AlgorithmPosition& position,
    bool refreshCachedPages) noexcept {
    return impl_ != nullptr && impl_->ExecuteAtGuestPc(
        memory,
        guestPc,
        entityAddress,
        tpidrEl0,
        pacgaKey,
        position,
        refreshCachedPages);
}

void AlgorithmPositionRuntime::Reset() noexcept {
    if (impl_ != nullptr) impl_->Reset();
}

void AlgorithmPositionRuntime::Invalidate() noexcept {
    if (impl_ != nullptr) impl_->Invalidate();
}

}  // namespace lengjing::game::native
