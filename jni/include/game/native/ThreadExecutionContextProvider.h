#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace lengjing::game::native {

struct TaskThreadIdentity {
    std::int32_t threadId = 0;
    std::uint64_t startTimeTicks = 0;
};

struct ExecutionPacKey128 {
    std::uint64_t low = 0;
    std::uint64_t high = 0;
};

struct ThreadPacKeys {
    ExecutionPacKey128 apia{};
    ExecutionPacKey128 apib{};
    ExecutionPacKey128 apda{};
    ExecutionPacKey128 apdb{};
    ExecutionPacKey128 apga{};
};

struct ThreadContextReaderCapabilities {
    bool threadIdSubject = false;
    bool tpidrEl0 = false;
    bool pacKeys = false;

    constexpr bool IsUsable() const noexcept {
        return threadIdSubject && tpidrEl0 && pacKeys;
    }
};

class ThreadExecutionContextReader {
public:
    virtual ~ThreadExecutionContextReader() = default;

    virtual ThreadContextReaderCapabilities Capabilities() const noexcept = 0;
    virtual int ReadTpidrEl0(std::int32_t threadId,
                             std::uint64_t& value) = 0;
    virtual int ReadPacKeys(
        std::int32_t threadId,
        ThreadPacKeys& keys) = 0;
};

class ProcTaskThreadLocator final {
public:
    explicit ProcTaskThreadLocator(std::string procRoot = "/proc");

    int FindExact(std::int32_t processId,
                  std::string_view threadName,
                  TaskThreadIdentity& identity,
                  std::int32_t preferredThreadId = 0) const;
    int FindAllExact(std::int32_t processId,
                     std::string_view threadName,
                     std::vector<TaskThreadIdentity>& identities) const;

private:
    std::string procRoot_;
};

struct ThreadExecutionContextSnapshot {
    std::int32_t processId = 0;
    std::int32_t threadId = 0;
    std::uint64_t threadStartTimeTicks = 0;
    std::uint64_t tpidrEl0 = 0;
    ExecutionPacKey128 apga{};
    std::uint64_t generation = 0;

    constexpr bool IsUsable() const noexcept {
        return processId > 0 && threadId > 0 && tpidrEl0 != 0 &&
            (apga.low != 0 || apga.high != 0);
    }
};

enum class ThreadExecutionContextEvent : std::uint8_t {
    Missing,
    Acquired,
    Stable,
    ThreadRecreated,
    ValuesChanged,
    Lost,
};

struct ThreadExecutionContextRefresh {
    ThreadExecutionContextEvent event =
        ThreadExecutionContextEvent::Missing;
    int status = 0;
    ThreadExecutionContextSnapshot snapshot{};

    constexpr bool HasContext() const noexcept {
        return status == 0 && snapshot.IsUsable();
    }
};

class ThreadExecutionContextProvider final {
public:
    ThreadExecutionContextProvider(
        std::int32_t processId,
        ThreadExecutionContextReader& reader,
        std::string threadName = "GameThread",
        std::string procRoot = "/proc");

    ThreadExecutionContextRefresh Refresh();
    bool Current(ThreadExecutionContextSnapshot& snapshot) const;
    bool RejectCurrent() noexcept;
    void Reset() noexcept;

private:
    ThreadExecutionContextRefresh InvalidateLocked(int status);

    std::int32_t processId_;
    ThreadExecutionContextReader& reader_;
    std::string threadName_;
    ProcTaskThreadLocator locator_;
    mutable std::mutex mutex_;
    ThreadExecutionContextSnapshot current_{};
    std::vector<TaskThreadIdentity> rejected_;
    std::uint64_t generation_ = 0;
};

}  // namespace lengjing::game::native
