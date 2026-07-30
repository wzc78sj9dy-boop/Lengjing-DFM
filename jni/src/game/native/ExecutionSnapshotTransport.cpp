#include "game/native/ExecutionSnapshotTransport.h"

#include <cerrno>
#include <fcntl.h>
#include <new>
#include <utility>

#if defined(__ANDROID__) || defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace {

constexpr char kCarrierPath[] =
    "/sys/fs/cgroup/cgroup.controllers";

}  // namespace

struct ExecutionSnapshotTransport::Impl {
    explicit Impl(ExecutionSnapshotSubmit injectedSubmit = nullptr,
                  void* injectedContext = nullptr) noexcept
        : submit(injectedSubmit),
          context(injectedContext) {}

    ExecutionSnapshotSubmit submit = nullptr;
    void* context = nullptr;
    int carrierDescriptor = -1;
    ExecutionSnapshotTransportProbe probe{};

    ~Impl() {
        static_cast<void>(Stop());
        CloseEndpoint();
    }

    void SetError(ExecutionSnapshotTransportError error,
                  int systemError = 0) noexcept {
        probe.error = error;
        probe.systemError = systemError;
    }

    bool OpenEndpoint() noexcept {
        if (submit != nullptr || carrierDescriptor >= 0) return true;
#if defined(__ANDROID__) || defined(__linux__)
        const int descriptor =
            open(kCarrierPath, O_WRONLY | O_CLOEXEC);
        if (descriptor < 0) {
            SetError(
                ExecutionSnapshotTransportError::EndpointOpenFailed,
                errno != 0 ? -errno : -ENOENT);
            return false;
        }
        carrierDescriptor = descriptor;
        return true;
#else
        SetError(
            ExecutionSnapshotTransportError::EndpointOpenFailed,
            -ENOSYS);
        return false;
#endif
    }

    int Submit(std::uint32_t operation, void* payload) noexcept {
        if (operation == 0 || payload == nullptr) return -EINVAL;
        if (submit != nullptr) {
            const int result = submit(context, operation, payload);
            return result <= 0 ? result : -EPROTO;
        }
#if defined(__ANDROID__) || defined(__linux__)
        if (carrierDescriptor < 0) return -ENOTCONN;
        execution_snapshot_abi::Envelope envelope{
            operation,
            0,
            payload,
        };
        errno = 0;
        const ssize_t result = static_cast<ssize_t>(syscall(
            SYS_write,
            carrierDescriptor,
            &envelope,
            execution_snapshot_abi::kRequestCount));
        const int systemError = errno;
        if (result < 0) {
            return systemError != 0 ? -systemError : -EIO;
        }
        return result == 0 ? 0 : -EPROTO;
#else
        static_cast<void>(operation);
        static_cast<void>(payload);
        return -ENOSYS;
#endif
    }

    void CloseEndpoint() noexcept {
#if defined(__ANDROID__) || defined(__linux__)
        if (carrierDescriptor >= 0) {
            close(carrierDescriptor);
        }
#endif
        carrierDescriptor = -1;
    }

    bool BindProcess() noexcept {
        if (!OpenEndpoint()) {
            probe.active = false;
            probe.needsReconfigure = false;
            probe.cleanupPending = false;
            return false;
        }
        std::int32_t target = probe.processId;
        const int result = Submit(
            execution_snapshot_abi::kSetProcessOperation,
            &target);
        if (result == 0) {
            probe.sequence = 0;
            return true;
        }
        CloseEndpoint();
        SetError(
            result == -E2BIG
                ? ExecutionSnapshotTransportError::EndpointUnsupported
                : ExecutionSnapshotTransportError::ProcessBindFailed,
            result);
        probe.active = false;
        probe.needsReconfigure = false;
        probe.cleanupPending = false;
        return false;
    }

    bool Disarm(bool reportError) noexcept {
        std::uint32_t reset = 0;
        const int result = Submit(
            execution_snapshot_abi::kDisarmOperation,
            &reset);
        if (result == 0) return true;
        if (reportError) {
            SetError(
                ExecutionSnapshotTransportError::DisarmFailed,
                result);
        }
        return false;
    }

    bool Arm() noexcept {
        execution_snapshot_abi::ArmPayload arm{};
        arm.processId = probe.processId;
        arm.instructionAddress = probe.instructionAddress;
        const int result = Submit(
            execution_snapshot_abi::kArmOperation,
            &arm);
        if (result == 0) {
            probe.sequence = 0;
            return true;
        }
        CloseEndpoint();
        SetError(
            result == -E2BIG
                ? ExecutionSnapshotTransportError::EndpointUnsupported
                : ExecutionSnapshotTransportError::ArmFailed,
            result);
        probe.active = false;
        probe.needsReconfigure = result != -E2BIG;
        probe.cleanupPending = false;
        return false;
    }

    bool Start(pid_t processId,
               std::uintptr_t instructionAddress) noexcept {
        if (processId <= 0 ||
            instructionAddress == 0 ||
            (instructionAddress & 3U) != 0) {
            SetError(
                ExecutionSnapshotTransportError::InvalidInput,
                -EINVAL);
            return false;
        }
        if (probe.active && !probe.cleanupPending &&
            probe.processId == processId &&
            probe.instructionAddress == instructionAddress) {
            return true;
        }
        if ((probe.active || probe.cleanupPending) && !Stop()) {
            return false;
        }
        probe.processId = processId;
        probe.instructionAddress = instructionAddress;
        probe.needsReconfigure = false;
        probe.cleanupPending = false;
        if (!BindProcess()) {
            return false;
        }
        static_cast<void>(Disarm(false));
        if (!Arm()) {
            return false;
        }
        probe.active = true;
        SetError(ExecutionSnapshotTransportError::None);
        return true;
    }

    void HardReset(
        ExecutionSnapshotTransportError error,
        int systemError) noexcept {
        const bool disarmed = Disarm(false);
        probe.active = !disarmed;
        probe.needsReconfigure = true;
        probe.cleanupPending = !disarmed;
        if (disarmed) probe.instructionAddress = 0;
        SetError(error, systemError);
    }

    ExecutionSnapshotPollResult Poll(
        ExecutionSnapshot& snapshot) noexcept {
        snapshot = {};
        if (probe.cleanupPending || !probe.active) {
            SetError(
                ExecutionSnapshotTransportError::InvalidInput,
                -EINVAL);
            return probe.needsReconfigure
                ? ExecutionSnapshotPollResult::Reconfigure
                : ExecutionSnapshotPollResult::Retry;
        }
        ++probe.pollCount;
        execution_snapshot_abi::PollPayload payload{};
        const int result = Submit(
            execution_snapshot_abi::kPollOperation,
            &payload);
        if (result != 0) {
            HardReset(
                ExecutionSnapshotTransportError::PollFailed,
                result);
            return ExecutionSnapshotPollResult::Reconfigure;
        }

        if (payload.state == 0 || payload.state == 2) {
            HardReset(
                ExecutionSnapshotTransportError::
                    SnapshotInvalid,
                0);
            return ExecutionSnapshotPollResult::Reconfigure;
        }
        if (payload.processId != probe.processId) {
            HardReset(
                ExecutionSnapshotTransportError::
                    SnapshotInvalid,
                -EPROTO);
            return ExecutionSnapshotPollResult::Reconfigure;
        }
        if (payload.hit == 0 ||
            payload.sequence == 0 ||
            payload.sequence == probe.sequence ||
            payload.instructionAddress !=
                probe.instructionAddress) {
            SetError(
                ExecutionSnapshotTransportError::
                    SnapshotInvalid);
            return ExecutionSnapshotPollResult::Retry;
        }

        probe.sequence = payload.sequence;
        std::uintptr_t candidate =
            execution_snapshot_abi::NormalizePointer(
                payload.primaryCandidate);
        bool usedFallback = false;
        if (candidate == 0) {
            candidate =
                execution_snapshot_abi::NormalizePointer(
                    payload.registers[23]);
            usedFallback = candidate != 0;
        }
        if (candidate == 0) {
            SetError(
                ExecutionSnapshotTransportError::
                    SnapshotInvalid);
            return ExecutionSnapshotPollResult::Retry;
        }
        ++probe.acceptedCount;
        SetError(ExecutionSnapshotTransportError::None);
        snapshot.candidate = candidate;
        snapshot.primaryCandidate =
            execution_snapshot_abi::NormalizePointer(
                payload.primaryCandidate);
        snapshot.fallbackCandidate =
            execution_snapshot_abi::NormalizePointer(
                payload.registers[23]);
        snapshot.instructionAddress =
            static_cast<std::uintptr_t>(
                payload.instructionAddress);
        snapshot.sequence = payload.sequence;
        snapshot.processId = payload.processId;
        snapshot.state = payload.state;
        snapshot.usedFallback = usedFallback;
        return ExecutionSnapshotPollResult::Accepted;
    }

    bool Rearm() noexcept {
        if (!probe.active || probe.cleanupPending) {
            SetError(
                ExecutionSnapshotTransportError::InvalidInput,
                -EINVAL);
            return false;
        }
        if (!Disarm(true)) {
            probe.cleanupPending = true;
            probe.needsReconfigure = true;
            return false;
        }
        probe.active = false;
        if (!Arm()) return false;
        probe.active = true;
        probe.cleanupPending = false;
        SetError(ExecutionSnapshotTransportError::None);
        return true;
    }

    bool Stop() noexcept {
        if (probe.active || probe.cleanupPending) {
            if (!Disarm(true)) {
                probe.active = true;
                probe.cleanupPending = true;
                probe.needsReconfigure = true;
                return false;
            }
        }
        CloseEndpoint();
        probe.active = false;
        probe.needsReconfigure = false;
        probe.cleanupPending = false;
        probe.instructionAddress = 0;
        return true;
    }
};

ExecutionSnapshotTransport::ExecutionSnapshotTransport()
    : impl_(std::make_unique<Impl>()) {}

ExecutionSnapshotTransport::ExecutionSnapshotTransport(
    ExecutionSnapshotSubmit submit,
    void* context) noexcept
    : impl_(new (std::nothrow) Impl(submit, context)) {}

ExecutionSnapshotTransport::~ExecutionSnapshotTransport() = default;

bool ExecutionSnapshotTransport::Start(
    pid_t processId,
    std::uintptr_t instructionAddress) noexcept {
    try {
        return impl_ != nullptr &&
            impl_->Start(processId, instructionAddress);
    } catch (...) {
        return false;
    }
}

ExecutionSnapshotPollResult ExecutionSnapshotTransport::Poll(
    ExecutionSnapshot& snapshot) noexcept {
    try {
        if (impl_ == nullptr) {
            snapshot = {};
            return ExecutionSnapshotPollResult::Retry;
        }
        return impl_->Poll(snapshot);
    } catch (...) {
        snapshot = {};
        return ExecutionSnapshotPollResult::Retry;
    }
}

bool ExecutionSnapshotTransport::Rearm() noexcept {
    try {
        return impl_ != nullptr && impl_->Rearm();
    } catch (...) {
        return false;
    }
}

bool ExecutionSnapshotTransport::Stop() noexcept {
    try {
        return impl_ == nullptr || impl_->Stop();
    } catch (...) {
        return false;
    }
}

void ExecutionSnapshotTransport::Reset() noexcept {
    if (impl_ == nullptr) return;
    try {
        if (impl_->Stop()) {
            impl_->probe = {};
        }
    } catch (...) {
    }
}

bool ExecutionSnapshotTransport::IsActive() const noexcept {
    return impl_ != nullptr && impl_->probe.active;
}

ExecutionSnapshotTransportProbe
ExecutionSnapshotTransport::Probe() const noexcept {
    return impl_ != nullptr
        ? impl_->probe
        : ExecutionSnapshotTransportProbe{};
}

}  // namespace lengjing::game::native
