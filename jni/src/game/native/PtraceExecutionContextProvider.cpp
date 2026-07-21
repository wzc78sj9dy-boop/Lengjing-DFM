#include "game/native/PtraceExecutionContextProvider.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <utility>

#if defined(__linux__) && defined(__aarch64__)
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace lengjing::game::native {
namespace {

constexpr std::uint32_t kPacgaX8X8X9 = UINT32_C(0x9AC93108);

std::uint64_t NextGeneration(std::uint64_t generation) noexcept {
    return generation == std::numeric_limits<std::uint64_t>::max()
        ? UINT64_C(1)
        : generation + 1;
}

#if defined(__linux__) && defined(__aarch64__)

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif
#ifndef NT_ARM_TLS
#define NT_ARM_TLS 0x401
#endif
#ifndef __WALL
#define __WALL 0x40000000
#endif

struct Arm64GeneralRegisters {
    std::uint64_t registers[31]{};
    std::uint64_t stackPointer = 0;
    std::uint64_t programCounter = 0;
    std::uint64_t processorState = 0;
};

bool WaitForThread(std::int32_t threadId,
                   int& status,
                   std::chrono::milliseconds timeout) noexcept {
    const auto deadline = std::chrono::steady_clock::now() +
        timeout;
    for (;;) {
        errno = 0;
        const pid_t result = ::waitpid(threadId, &status, __WALL | WNOHANG);
        if (result == threadId) return true;
        if (result == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                errno = ETIMEDOUT;
                return false;
            }
            ::usleep(1000);
            continue;
        }
        if (result < 0 && errno == EINTR) continue;
        return false;
    }
}

bool ReadRegisterSet(std::int32_t threadId,
                     unsigned long note,
                     void* data,
                     std::size_t size) noexcept {
    iovec io{data, size};
    return ::ptrace(
               PTRACE_GETREGSET,
               threadId,
               reinterpret_cast<void*>(note),
               &io) == 0 &&
        io.iov_len == size;
}

bool WriteRegisterSet(std::int32_t threadId,
                      unsigned long note,
                      void* data,
                      std::size_t size) noexcept {
    iovec io{data, size};
    return ::ptrace(
               PTRACE_SETREGSET,
               threadId,
               reinterpret_cast<void*>(note),
               &io) == 0;
}

#endif

}  // namespace

struct PtracePacgaOracleReader::Impl {
    std::mutex mutex;

#if defined(__linux__) && defined(__aarch64__)
    struct TraceSession {
        std::int32_t threadId = 0;
        Arm64GeneralRegisters original{};
        bool attached = false;
        bool stopped = false;
        bool registersStaged = false;
    } pending;

    bool Cleanup(std::chrono::milliseconds timeout) noexcept {
        if (!pending.attached) {
            pending = {};
            return true;
        }
        if (!pending.stopped) {
            int status = 0;
            if (!WaitForThread(pending.threadId, status, timeout)) {
                return false;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                pending = {};
                return true;
            }
            if (!WIFSTOPPED(status)) return false;
            pending.stopped = true;
        }
        if (pending.registersStaged) {
            for (int attempt = 0;
                 attempt < 8 && pending.registersStaged;
                 ++attempt) {
                if (WriteRegisterSet(
                        pending.threadId,
                        NT_PRSTATUS,
                        &pending.original,
                        sizeof(pending.original))) {
                    pending.registersStaged = false;
                    break;
                }
                if (errno == ESRCH) {
                    pending = {};
                    return true;
                }
                ::usleep(1000);
            }
            if (pending.registersStaged) return false;
        }
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (::ptrace(
                    PTRACE_DETACH,
                    pending.threadId,
                    nullptr,
                    nullptr) == 0 ||
                errno == ESRCH) {
                pending = {};
                return true;
            }
            ::usleep(1000);
        }
        return false;
    }
#endif
};

PtracePacgaOracleReader::PtracePacgaOracleReader()
    : impl_(std::make_unique<Impl>()) {}

PtracePacgaOracleReader::~PtracePacgaOracleReader() {
#if defined(__linux__) && defined(__aarch64__)
    if (impl_ != nullptr) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        static_cast<void>(impl_->Cleanup(std::chrono::seconds(2)));
    }
#endif
}

int PtracePacgaOracleReader::Read(
    std::int32_t threadId,
    const PacgaOracleInstruction& instruction,
    std::uint64_t& tpidrEl0,
    std::uint64_t& result) {
    tpidrEl0 = 0;
    result = 0;
    if (threadId <= 0 || !instruction.IsValid()) return -EINVAL;

#if defined(__linux__) && defined(__aarch64__)
    if (impl_ == nullptr) return -ENOMEM;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    constexpr auto waitTimeout = std::chrono::milliseconds(250);
    if (!impl_->Cleanup(waitTimeout)) return -EBUSY;

    if (::ptrace(PTRACE_ATTACH, threadId, nullptr, nullptr) != 0) {
        return -errno;
    }
    impl_->pending = {};
    impl_->pending.threadId = threadId;
    impl_->pending.attached = true;
    Arm64GeneralRegisters original{};
    const auto fail = [&](int error) noexcept {
        static_cast<void>(impl_->Cleanup(waitTimeout));
        return -(error != 0 ? error : EIO);
    };

    int status = 0;
    if (!WaitForThread(threadId, status, waitTimeout)) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        impl_->pending = {};
        return -ESRCH;
    }
    if (!WIFSTOPPED(status)) {
        return fail(EIO);
    }
    impl_->pending.stopped = true;
    if (!ReadRegisterSet(
            threadId, NT_PRSTATUS, &original, sizeof(original)) ||
        !ReadRegisterSet(
            threadId, NT_ARM_TLS, &tpidrEl0, sizeof(tpidrEl0))) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    impl_->pending.original = original;

    errno = 0;
    const long encoded = ::ptrace(
        PTRACE_PEEKTEXT,
        threadId,
        reinterpret_cast<void*>(instruction.address),
        nullptr);
    if ((encoded == -1 && errno != 0) ||
        static_cast<std::uint32_t>(encoded) != kPacgaX8X8X9) {
        const int error = errno != 0 ? errno : ENOEXEC;
        return fail(error);
    }

    Arm64GeneralRegisters staged = original;
    staged.registers[8] = instruction.data;
    staged.registers[9] = instruction.modifier;
    staged.programCounter = instruction.address;
    if (!WriteRegisterSet(
            threadId, NT_PRSTATUS, &staged, sizeof(staged))) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    impl_->pending.registersStaged = true;

    if (::ptrace(PTRACE_SINGLESTEP, threadId, nullptr, nullptr) != 0) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    impl_->pending.stopped = false;
    if (!WaitForThread(threadId, status, waitTimeout)) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        impl_->pending = {};
        return -ESRCH;
    }
    if (!WIFSTOPPED(status)) return fail(EIO);
    impl_->pending.stopped = true;
    if (WSTOPSIG(status) != SIGTRAP) {
        return fail(EINTR);
    }

    Arm64GeneralRegisters completed{};
    if (!ReadRegisterSet(
            threadId, NT_PRSTATUS, &completed, sizeof(completed))) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    result = completed.registers[8];

    if (!WriteRegisterSet(
            threadId, NT_PRSTATUS, &original, sizeof(original))) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    impl_->pending.registersStaged = false;
    if (::ptrace(PTRACE_DETACH, threadId, nullptr, nullptr) != 0) {
        const int error = errno != 0 ? errno : EIO;
        return fail(error);
    }
    impl_->pending = {};
    return tpidrEl0 != 0 ? 0 : -ENODATA;
#else
    static_cast<void>(instruction);
    return -ENOTSUP;
#endif
}

PtraceExecutionContextProvider::PtraceExecutionContextProvider(
    std::int32_t processId,
    PacgaOracleReader& reader,
    std::string threadName,
    std::string procRoot)
    : processId_(processId),
      reader_(reader),
      threadName_(std::move(threadName)),
      locator_(std::move(procRoot)) {}

PtraceExecutionContextRefresh PtraceExecutionContextProvider::Refresh(
    const PacgaOracleInstruction& instruction) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (processId_ <= 0 || threadName_.empty() || !instruction.IsValid()) {
        retryInstruction_ = {};
        return InvalidateLocked(-EINVAL);
    }

    const auto now = std::chrono::steady_clock::now();
    if (!current_.IsUsable() &&
        retryAfter_.time_since_epoch().count() != 0 &&
        now < retryAfter_ && retryInstruction_ == instruction) {
        return {lastStatus_ != 0 ? lastStatus_ : -EAGAIN, {}};
    }
    retryInstruction_ = instruction;
    if (current_.IsUsable() && current_.instruction == instruction &&
        refreshAfter_.time_since_epoch().count() != 0 &&
        now < refreshAfter_) {
        return {0, current_};
    }

    std::vector<TaskThreadIdentity> identities;
    const int locateStatus = locator_.FindAllExact(
        processId_, threadName_, identities);
    if (locateStatus != 0) return InvalidateLocked(locateStatus);
    std::stable_sort(
        identities.begin(),
        identities.end(),
        [](const TaskThreadIdentity& left,
           const TaskThreadIdentity& right) {
            if (left.startTimeTicks != right.startTimeTicks) {
                return left.startTimeTicks < right.startTimeTicks;
            }
            return left.threadId < right.threadId;
        });

    if (current_.IsUsable() && current_.instruction == instruction) {
        const auto stableIdentity = std::find_if(
            identities.begin(),
            identities.end(),
            [this](const TaskThreadIdentity& identity) {
                return identity.threadId == current_.threadId &&
                    identity.startTimeTicks ==
                        current_.threadStartTimeTicks;
            });
        if (stableIdentity != identities.end()) {
            refreshAfter_ = now + std::chrono::seconds(1);
            retryAfter_ = {};
            lastStatus_ = 0;
            return {0, current_};
        }
        current_ = {};
        refreshAfter_ = {};
    }

    const auto isRejected = [this](const TaskThreadIdentity& identity) {
        return std::find_if(
                   rejected_.begin(),
                   rejected_.end(),
                   [&identity](const TaskThreadIdentity& rejected) {
                       return rejected.threadId == identity.threadId &&
                           rejected.startTimeTicks == identity.startTimeTicks;
                   }) != rejected_.end();
    };
    if (std::all_of(identities.begin(), identities.end(), isRejected)) {
        rejected_.clear();
    }
    if (current_.IsUsable()) {
        const auto preferred = std::find_if(
            identities.begin(),
            identities.end(),
            [this](const TaskThreadIdentity& identity) {
                return identity.threadId == current_.threadId &&
                    identity.startTimeTicks == current_.threadStartTimeTicks;
            });
        if (preferred != identities.end() && preferred != identities.begin()) {
            std::rotate(identities.begin(), preferred, preferred + 1);
        }
    }

    int readStatus = -ENOENT;
    PtraceExecutionContextSnapshot candidate{};
    for (const TaskThreadIdentity& identity : identities) {
        if (isRejected(identity)) continue;
        std::uint64_t tpidrEl0 = 0;
        std::uint64_t oracleResult = 0;
        readStatus = reader_.Read(
            identity.threadId, instruction, tpidrEl0, oracleResult);
        if (readStatus != 0) continue;

        std::vector<TaskThreadIdentity> verified;
        if (locator_.FindAllExact(processId_, threadName_, verified) != 0 ||
            std::find_if(
                verified.begin(),
                verified.end(),
                [&identity](const TaskThreadIdentity& item) {
                    return item.threadId == identity.threadId &&
                        item.startTimeTicks == identity.startTimeTicks;
                }) == verified.end()) {
            readStatus = -EAGAIN;
            continue;
        }
        candidate = {
            processId_,
            identity.threadId,
            identity.startTimeTicks,
            tpidrEl0,
            instruction,
            oracleResult,
            0,
        };
        break;
    }
    if (!candidate.IsUsable()) return InvalidateLocked(readStatus);

    const bool stable = current_.IsUsable() &&
        current_.threadId == candidate.threadId &&
        current_.threadStartTimeTicks == candidate.threadStartTimeTicks &&
        current_.tpidrEl0 == candidate.tpidrEl0 &&
        current_.instruction == candidate.instruction &&
        current_.result == candidate.result;
    if (stable) {
        candidate.generation = current_.generation;
    } else {
        generation_ = NextGeneration(generation_);
        candidate.generation = generation_;
    }
    current_ = candidate;
    refreshAfter_ = now + std::chrono::seconds(1);
    retryAfter_ = {};
    retryInstruction_ = {};
    lastStatus_ = 0;
    return {0, current_};
}

bool PtraceExecutionContextProvider::RejectCurrent() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!current_.IsUsable()) return false;
    try {
        rejected_.push_back({
            current_.threadId,
            current_.threadStartTimeTicks,
        });
    } catch (...) {
        return false;
    }
    current_ = {};
    refreshAfter_ = {};
    retryAfter_ = {};
    retryInstruction_ = {};
    lastStatus_ = 0;
    return true;
}

void PtraceExecutionContextProvider::Reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = {};
    rejected_.clear();
    generation_ = 0;
    refreshAfter_ = {};
    retryAfter_ = {};
    retryInstruction_ = {};
    lastStatus_ = 0;
}

PtraceExecutionContextRefresh
PtraceExecutionContextProvider::InvalidateLocked(int status) {
    current_ = {};
    refreshAfter_ = {};
    lastStatus_ = status != 0 ? status : -EIO;
    retryAfter_ = lastStatus_ == -EINVAL
        ? std::chrono::steady_clock::time_point{}
        : std::chrono::steady_clock::now() + std::chrono::seconds(1);
    return {lastStatus_, {}};
}

}  // namespace lengjing::game::native
