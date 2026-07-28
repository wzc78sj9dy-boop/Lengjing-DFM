#pragma once

#include "game/native/CoordinateStabilityPolicy.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include <sys/types.h>

namespace lengjing::game::native {

class MemoryTransport;

struct SceneTransformProfile {
    std::uintptr_t bridgeRva = 0;
    std::uintptr_t bridgePointerOffset = 0;
    std::uintptr_t contextBackOffset = 0;
    std::uintptr_t entryOffset = 0;
    std::uintptr_t returnOffset = 0;
    std::uintptr_t secondaryRootRva = 0;
    std::uintptr_t secondaryPointerOffset = 0;
    std::uintptr_t secondaryFinalOffset = 0;
    std::uintptr_t actorRootOffset = 0;
    std::uintptr_t componentTransformOffset = 0;
    std::uintptr_t actorBaseOffset = 0;
    std::uint32_t initialFpsr = 0;
    std::uint32_t initialFpcr = 0;
    std::uint64_t instructionBudget = 0;

    constexpr bool IsValid() const noexcept {
        return bridgeRva != 0 && bridgePointerOffset != 0 &&
            contextBackOffset != 0 && entryOffset != 0 &&
            returnOffset != 0 && secondaryRootRva != 0 &&
            secondaryPointerOffset != 0 &&
            componentTransformOffset != 0 && actorBaseOffset != 0 &&
            instructionBudget != 0;
    }
};

inline constexpr SceneTransformProfile kSceneTransformDomesticProfile{
    UINT64_C(0x0E738950),
    UINT64_C(0x0C),
    UINT64_C(0x08),
    UINT64_C(0xA0),
    UINT64_C(0xA8),
    UINT64_C(0x0E73895C),
    UINT64_C(0xA0),
    UINT64_C(0),
    UINT64_C(0x180),
    UINT64_C(0x210),
    UINT64_C(0x44C),
    UINT32_C(0x0800009F),
    UINT32_C(0),
    UINT64_C(300000),
};

inline constexpr std::uint64_t kSceneTransformMinimumRemoteAddress =
    UINT64_C(0x4000000001);
inline constexpr std::uint64_t kSceneTransformRemoteSpan =
    UINT64_C(0xBFFFFFFFFF);

constexpr bool IsSceneTransformRemotePointer(
    std::uint64_t value) noexcept {
    return value - kSceneTransformMinimumRemoteAddress <
            kSceneTransformRemoteSpan ||
        value + UINT64_C(0x4BFFFFFFFFFFFFFF) <=
            UINT64_C(0xFFFFFFFFFFFFFE);
}

constexpr std::uint64_t NormalizeSceneTransformPointer(
    std::uint64_t value) noexcept {
    const std::uint64_t wide = value & UINT64_C(0x00FFFFFFFFFFFFFF);
    if (IsSceneTransformRemotePointer(wide)) return wide;

    const std::uint64_t narrow = value & UINT64_C(0x000000FFFFFFFFFF);
    if (value >= UINT64_C(0xFFFFFF0000000000) &&
        IsSceneTransformRemotePointer(narrow)) {
        return narrow;
    }

    const std::uint64_t tagged = value & UINT64_C(0x0000FFFFFFFFFFFF);
    if ((value >> 48U) == UINT64_C(0xB400) &&
        IsSceneTransformRemotePointer(tagged)) {
        return tagged;
    }
    return IsSceneTransformRemotePointer(value) ? value : 0;
}

struct SceneTransformInvocation {
    std::uintptr_t root = 0;
    std::uintptr_t initialX0 = 0;
    std::uintptr_t entryPc = 0;
    std::uintptr_t linkPc = 0;
    std::uintptr_t hookPc = 0;
    std::uintptr_t secondaryEntry = 0;

    constexpr bool IsValid() const noexcept {
        return IsSceneTransformRemotePointer(root) &&
            IsSceneTransformRemotePointer(initialX0) &&
            IsSceneTransformRemotePointer(entryPc) &&
            IsSceneTransformRemotePointer(linkPc) &&
            IsSceneTransformRemotePointer(hookPc) &&
            IsSceneTransformRemotePointer(secondaryEntry);
    }
};

enum class SceneTransformDecision : std::uint8_t {
    Pending,
    Accepted,
    RejectedRetained,
    InvalidRetained,
    InvalidCleared,
};

enum class SceneTransformError : std::uint16_t {
    None,
    InvalidConfiguration,
    ThreadContextUnavailable,
    InvocationUnavailable,
    EntryReadFailed,
    MetadataUnavailable,
    EngineSetupFailed,
    RegisterSetupFailed,
    RemoteReadFailed,
    GuestMapFailed,
    PacgaUnavailable,
    UnsupportedInstruction,
    EmulationFailed,
    StepLimit,
    FreshWriteMissing,
    SelectedAddressInvalid,
    TransformReadFailed,
    CoordinateInvalid,
    ReferenceRejected,
    HistoryRejected,
    StaleRequest,
};

struct SceneTransformSubject {
    std::uintptr_t world = 0;
    std::uintptr_t actor = 0;
    std::uintptr_t component = 0;
    StableCoordinate reference{};
    std::uint64_t frameSequence = 0;
};

struct SceneTransformResult {
    std::uintptr_t world = 0;
    std::uintptr_t actor = 0;
    std::uintptr_t component = 0;
    StableCoordinate position{};
    SceneTransformDecision decision = SceneTransformDecision::Pending;
    std::uint64_t generation = 0;
    std::uint64_t sourceFrameSequence = 0;
};

struct SceneTransformProbe {
    bool active = false;
    bool contextReady = false;
    bool metadataReady = false;
    SceneTransformError error = SceneTransformError::None;
    int systemError = 0;
    std::uint64_t generation = 0;
    std::uint64_t requested = 0;
    std::uint64_t completed = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t retained = 0;
    std::uint64_t cleared = 0;
    std::uint64_t pacgaCalls = 0;
    std::uint64_t instructionCount = 0;
    std::uint64_t mapHits = 0;
    std::uint64_t mapLearns = 0;
    std::uint32_t attempt = 0;
    std::uint32_t stopReason = 0;
    std::uintptr_t moduleBase = 0;
    std::uintptr_t stackLow = 0;
    std::uintptr_t stackHigh = 0;
    pid_t threadId = -1;
    std::uintptr_t threadPointer = 0;
    SceneTransformInvocation invocation{};
    std::uintptr_t actor = 0;
    std::uintptr_t component = 0;
    std::uintptr_t selectedAddress = 0;
    StableCoordinate translation{};
    StableCoordinate actorBase{};
    StableCoordinate candidate{};
    SceneTransformDecision decision = SceneTransformDecision::Pending;
};

const char* SceneTransformDecisionName(
    SceneTransformDecision decision) noexcept;
const char* SceneTransformErrorName(SceneTransformError error) noexcept;

class SceneTransformRuntime final {
public:
    SceneTransformRuntime();
    ~SceneTransformRuntime();

    SceneTransformRuntime(const SceneTransformRuntime&) = delete;
    SceneTransformRuntime& operator=(const SceneTransformRuntime&) = delete;

    bool Start(MemoryTransport& memory,
               pid_t processId,
               std::uintptr_t moduleBase,
               const SceneTransformProfile& profile =
                   kSceneTransformDomesticProfile) noexcept;
    void SetWorld(std::uintptr_t world) noexcept;
    void Request(const SceneTransformSubject& subject) noexcept;
    bool Lookup(const SceneTransformSubject& subject,
                SceneTransformResult& result) const noexcept;
    SceneTransformProbe Probe() const noexcept;
    void Stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
