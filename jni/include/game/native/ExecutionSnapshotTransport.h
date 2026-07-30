#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <sys/types.h>

namespace lengjing::game::native {

namespace execution_snapshot_abi {

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

static_assert(sizeof(void*) == 8,
              "execution snapshot transport requires 64-bit");
static_assert(sizeof(Envelope) == 0x10,
              "bad execution snapshot envelope layout");
static_assert(offsetof(Envelope, payload) == 0x8,
              "bad execution snapshot envelope payload offset");
static_assert(sizeof(ArmPayload) == 0x28,
              "bad execution snapshot arm layout");
static_assert(sizeof(PollPayload) == 0x160,
              "bad execution snapshot poll layout");
static_assert(offsetof(PollPayload, registers[23]) == 0xb8,
              "bad execution snapshot x23 offset");
static_assert(offsetof(PollPayload, primaryCandidate) == 0x130,
              "bad execution snapshot primary candidate offset");
static_assert(offsetof(PollPayload, sequence) == 0x138,
              "bad execution snapshot sequence offset");
static_assert(offsetof(PollPayload, hit) == 0x144,
              "bad execution snapshot hit offset");
static_assert(offsetof(PollPayload, state) == 0x148,
              "bad execution snapshot state offset");

constexpr std::uintptr_t NormalizePointer(
    std::uint64_t value) noexcept {
    return static_cast<std::uintptr_t>(
        value & kPointerPayloadMask);
}

}  // namespace execution_snapshot_abi

struct ExecutionSnapshot {
    std::uintptr_t candidate = 0;
    std::uintptr_t primaryCandidate = 0;
    std::uintptr_t fallbackCandidate = 0;
    std::uintptr_t instructionAddress = 0;
    std::uint64_t sequence = 0;
    std::int32_t processId = -1;
    std::uint32_t state = 0;
    bool usedFallback = false;
};

enum class ExecutionSnapshotTransportError : std::uint8_t {
    None,
    InvalidInput,
    ArmFailed,
    PollFailed,
    SnapshotInvalid,
    DisarmFailed,
    EndpointOpenFailed,
    ProcessBindFailed,
    EndpointUnsupported,
};

enum class ExecutionSnapshotPollResult : std::uint8_t {
    Accepted,
    Retry,
    Reconfigure,
};

class ExecutionSnapshotFallbackPolicy final {
public:
    static constexpr std::uint32_t kReconfigureLimit = 3;

    void Observe(ExecutionSnapshotPollResult result) noexcept {
        if (result == ExecutionSnapshotPollResult::Accepted) {
            reconfigureFailures_ = 0;
        } else if (
            result == ExecutionSnapshotPollResult::Reconfigure &&
            reconfigureFailures_ < kReconfigureLimit) {
            ++reconfigureFailures_;
        }
    }

    void Reset() noexcept {
        reconfigureFailures_ = 0;
    }

    bool ShouldFallback() const noexcept {
        return reconfigureFailures_ >= kReconfigureLimit;
    }

    std::uint32_t ReconfigureFailures() const noexcept {
        return reconfigureFailures_;
    }

private:
    std::uint32_t reconfigureFailures_ = 0;
};

struct ExecutionSnapshotTransportProbe {
    ExecutionSnapshotTransportError error =
        ExecutionSnapshotTransportError::None;
    int systemError = 0;
    pid_t processId = -1;
    std::uintptr_t instructionAddress = 0;
    std::uint64_t sequence = 0;
    std::uint64_t pollCount = 0;
    std::uint64_t acceptedCount = 0;
    bool active = false;
    bool needsReconfigure = false;
    bool cleanupPending = false;
};

using ExecutionSnapshotSubmit = int (*)(
    void* context,
    std::uint32_t operation,
    void* payload) noexcept;

class ExecutionSnapshotTransport final {
public:
    ExecutionSnapshotTransport();
    ExecutionSnapshotTransport(
        ExecutionSnapshotSubmit submit,
        void* context) noexcept;
    ~ExecutionSnapshotTransport();

    ExecutionSnapshotTransport(
        const ExecutionSnapshotTransport&) = delete;
    ExecutionSnapshotTransport& operator=(
        const ExecutionSnapshotTransport&) = delete;

    bool Start(pid_t processId,
               std::uintptr_t instructionAddress) noexcept;
    ExecutionSnapshotPollResult Poll(
        ExecutionSnapshot& snapshot) noexcept;
    bool Rearm() noexcept;
    bool Stop() noexcept;
    void Reset() noexcept;

    bool IsActive() const noexcept;
    ExecutionSnapshotTransportProbe Probe() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::native
