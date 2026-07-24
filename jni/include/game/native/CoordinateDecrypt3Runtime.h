#pragma once

#if 0

#include "game/native/MemoryTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include <sys/types.h>

namespace lengjing::game::native {

namespace coordinate_decrypt3_abi {

inline constexpr std::uint32_t kSetProcessOperation = 0x80eU;
inline constexpr std::uint32_t kArmOperation = 0xc7a1U;
inline constexpr std::uint32_t kDisarmOperation = 0xc7a2U;
inline constexpr std::uint32_t kPollOperation = 0xc7a3U;
inline constexpr std::size_t kRequestCount = 0x9400U;
inline constexpr std::uint64_t kPointerPayloadMask =
    UINT64_C(0x00ffffffffffffff);

struct Envelope {
    std::uint32_t operation;
    std::uint32_t reserved;
    void* payload;
};

struct ArmPayload {
    std::int32_t processId;
    std::uint32_t reserved;
    std::uint64_t instructionAddress;
    std::uint64_t literalAddress;
    std::uint64_t protectedPage;
    std::uint32_t instruction;
    std::uint32_t literalSize;
};

struct PollPayload {
    std::uint64_t registers[31];
    std::uint64_t stackPointer;
    std::uint64_t programCounter;
    std::uint64_t processorState;
    std::uint64_t faultAddress;
    std::uint64_t instructionAddress;
    std::uint64_t literalAddress;
    std::uint64_t protectedPage;
    std::uint64_t primaryCandidate;
    std::uint64_t sequence;
    std::int32_t processId;
    std::uint32_t hit;
    std::uint32_t state;
    std::uint32_t reason;
    std::uint32_t syndrome;
    std::uint32_t instruction;
    std::uint32_t literalSize;
    std::uint32_t reserved;
};

static_assert(sizeof(void*) == 8, "coordinate decrypt3 requires 64-bit");
static_assert(sizeof(Envelope) == 16, "bad coordinate envelope layout");
static_assert(sizeof(ArmPayload) == 0x28, "bad coordinate arm layout");
static_assert(sizeof(PollPayload) == 0x160, "bad coordinate poll layout");
static_assert(offsetof(PollPayload, registers[23]) == 0xb8,
              "bad x23 register offset");
static_assert(offsetof(PollPayload, primaryCandidate) == 0x130,
              "bad primary candidate offset");
static_assert(offsetof(PollPayload, sequence) == 0x138,
              "bad sequence offset");
static_assert(offsetof(PollPayload, hit) == 0x144,
              "bad hit offset");
static_assert(offsetof(PollPayload, state) == 0x148,
              "bad state offset");

constexpr std::uintptr_t NormalizePointer(
    std::uint64_t value) noexcept {
    return static_cast<std::uintptr_t>(
        value & kPointerPayloadMask);
}

constexpr bool SelectSnapshotCandidate(
    const PollPayload& snapshot,
    std::uintptr_t expectedInstruction,
    std::uint64_t previousSequence,
    std::uintptr_t& candidate) noexcept {
    candidate = 0;
    if (expectedInstruction == 0 ||
        snapshot.hit == 0 ||
        snapshot.state == 0 ||
        snapshot.state == 2 ||
        snapshot.instructionAddress != expectedInstruction ||
        snapshot.sequence == 0 ||
        snapshot.sequence == previousSequence) {
        return false;
    }
    candidate = NormalizePointer(snapshot.primaryCandidate);
    if (candidate == 0) {
        candidate = NormalizePointer(snapshot.registers[23]);
    }
    return candidate != 0;
}

template <std::size_t Capacity>
constexpr std::uintptr_t SelectSampleMode(
    const std::array<std::uintptr_t, Capacity>& samples,
    std::size_t count) noexcept {
    if (count == 0 || count > Capacity) return 0;
    std::uintptr_t selected = samples[0];
    std::size_t selectedCount = 0;
    for (std::size_t left = 0; left < count; ++left) {
        std::size_t occurrences = 0;
        for (std::size_t right = 0; right < count; ++right) {
            if (samples[left] == samples[right]) ++occurrences;
        }
        if (occurrences > selectedCount) {
            selected = samples[left];
            selectedCount = occurrences;
        }
    }
    return selected;
}

}  // namespace coordinate_decrypt3_abi

struct CoordinateDecrypt3Position {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class CoordinateDecrypt3Backend : std::uint8_t {
    None,
    PageTrap,
    PageExecution,
};

enum class CoordinateDecrypt3Error : std::uint8_t {
    None,
    InvalidInput,
    EndpointUnavailable,
    ArmFailed,
    PollFailed,
    SnapshotInvalid,
    CandidateUnreadable,
    ManagerReadFailed,
    MetadataInvalid,
    ArrayReadFailed,
    ArrayChanged,
    NoPublishedCoordinates,
};

struct CoordinateDecrypt3Probe {
    CoordinateDecrypt3Backend backend = CoordinateDecrypt3Backend::None;
    CoordinateDecrypt3Error error = CoordinateDecrypt3Error::None;
    int systemError = 0;
    std::uintptr_t instructionAddress = 0;
    std::uintptr_t recordsBase = 0;
    std::uintptr_t manager = 0;
    std::uintptr_t idArray = 0;
    std::uint32_t recordCount = 0;
    std::size_t sampleCount = 0;
    std::size_t publishedCount = 0;
    std::uint64_t pollCount = 0;
    std::uint64_t acceptedSampleCount = 0;
    std::uint64_t sequence = 0;
    bool active = false;
};

class CoordinateDecrypt3Runtime final {
public:
    CoordinateDecrypt3Runtime();
    ~CoordinateDecrypt3Runtime();

    CoordinateDecrypt3Runtime(const CoordinateDecrypt3Runtime&) = delete;
    CoordinateDecrypt3Runtime& operator=(
        const CoordinateDecrypt3Runtime&) = delete;

    bool Start(MemoryTransport& memory,
               pid_t processId,
               std::uintptr_t instructionAddress) noexcept;
    bool Refresh(std::uintptr_t world) noexcept;
    bool Lookup(std::uintptr_t mesh,
                std::uintptr_t world,
                CoordinateDecrypt3Position& position) const noexcept;
    bool Stop() noexcept;
    void Reset() noexcept;

    bool IsActive() const noexcept;
    CoordinateDecrypt3Probe Probe() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native

#endif
