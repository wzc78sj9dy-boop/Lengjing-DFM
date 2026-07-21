#include "game/native/ThreadExecutionContextProvider.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace lengjing::game::native {
namespace {

int ErrorFromCode(const std::error_code& error) {
    if (!error) return -EIO;
    if (error == std::errc::permission_denied) return -EACCES;
    if (error == std::errc::no_such_file_or_directory) return -ENOENT;
    return -EIO;
}

bool ParsePositiveId(std::string_view text, std::int32_t& value) {
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        text.data(),
        text.data() + text.size(),
        parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed <= 0 || parsed > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    value = static_cast<std::int32_t>(parsed);
    return true;
}

bool ReadThreadName(const std::filesystem::path& path, std::string& name) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !std::getline(stream, name)) return false;
    if (!name.empty() && name.back() == '\r') name.pop_back();
    return true;
}

bool ReadThreadStartTime(const std::filesystem::path& path,
                         std::string_view expectedName,
                         std::uint64_t& startTimeTicks) {
    std::ifstream stream(path, std::ios::binary);
    std::string stat;
    if (!stream || !std::getline(stream, stat)) return false;

    const std::size_t commBegin = stat.find('(');
    const std::size_t commEnd = stat.rfind(')');
    if (commBegin == std::string::npos || commEnd == std::string::npos ||
        commBegin >= commEnd || commEnd + 1 >= stat.size() ||
        std::string_view(stat).substr(
            commBegin + 1,
            commEnd - commBegin - 1) != expectedName) {
        return false;
    }

    std::istringstream fields(stat.substr(commEnd + 1));
    std::string field;
    for (int number = 3; number <= 22; ++number) {
        if (!(fields >> field)) return false;
        if (number != 22) continue;
        const auto result = std::from_chars(
            field.data(),
            field.data() + field.size(),
            startTimeTicks);
        return result.ec == std::errc{} &&
            result.ptr == field.data() + field.size();
    }
    return false;
}

std::uint64_t NextGeneration(std::uint64_t generation) {
    return generation == std::numeric_limits<std::uint64_t>::max()
        ? UINT64_C(1)
        : generation + 1;
}

bool IsRetryableContextError(int status) {
    return status == -EAGAIN || status == -ENOENT || status == -ESRCH;
}

}  // namespace

ProcTaskThreadLocator::ProcTaskThreadLocator(std::string procRoot)
    : procRoot_(std::move(procRoot)) {}

int ProcTaskThreadLocator::FindExact(
    std::int32_t processId,
    std::string_view threadName,
    TaskThreadIdentity& identity,
    std::int32_t preferredThreadId) const {
    identity = {};
    std::vector<TaskThreadIdentity> identities;
    const int result = FindAllExact(processId, threadName, identities);
    if (result != 0) return result;
    const auto preferred = std::find_if(
        identities.begin(),
        identities.end(),
        [preferredThreadId](const TaskThreadIdentity& candidate) {
            return candidate.threadId == preferredThreadId;
        });
    identity = preferred != identities.end() ? *preferred : identities.front();
    return 0;
}

int ProcTaskThreadLocator::FindAllExact(
    std::int32_t processId,
    std::string_view threadName,
    std::vector<TaskThreadIdentity>& identities) const {
    identities.clear();
    if (processId <= 0 || threadName.empty() ||
        threadName.find('\0') != std::string_view::npos || procRoot_.empty()) {
        return -EINVAL;
    }

    const std::filesystem::path taskRoot =
        std::filesystem::path(procRoot_) /
        std::to_string(processId) /
        "task";
    std::error_code error;
    std::filesystem::directory_iterator iterator(taskRoot, error);
    if (error) return ErrorFromCode(error);

    bool exactThreadRaced = false;
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) return ErrorFromCode(error);

        const std::string fileName = iterator->path().filename().string();
        std::int32_t threadId = 0;
        if (!ParsePositiveId(fileName, threadId)) continue;

        std::string actualName;
        if (!ReadThreadName(iterator->path() / "comm", actualName) ||
            actualName != threadName) {
            continue;
        }

        std::uint64_t startTimeTicks = 0;
        if (!ReadThreadStartTime(
                iterator->path() / "stat",
                threadName,
                startTimeTicks)) {
            exactThreadRaced = true;
            continue;
        }

        identities.push_back({threadId, startTimeTicks});
    }
    if (error) return ErrorFromCode(error);
    if (identities.empty()) return exactThreadRaced ? -EAGAIN : -ENOENT;
    std::sort(
        identities.begin(),
        identities.end(),
        [](const TaskThreadIdentity& left, const TaskThreadIdentity& right) {
            if (left.threadId != right.threadId) {
                return left.threadId < right.threadId;
            }
            return left.startTimeTicks < right.startTimeTicks;
        });
    return 0;
}

ThreadExecutionContextProvider::ThreadExecutionContextProvider(
    std::int32_t processId,
    ThreadExecutionContextReader& reader,
    std::string threadName,
    std::string procRoot)
    : processId_(processId),
      reader_(reader),
      threadName_(std::move(threadName)),
      locator_(std::move(procRoot)) {}

ThreadExecutionContextRefresh ThreadExecutionContextProvider::Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (processId_ <= 0 || threadName_.empty()) {
        return InvalidateLocked(-EINVAL);
    }
    if (!reader_.Capabilities().IsUsable()) {
        return InvalidateLocked(-ENOTSUP);
    }

    ThreadExecutionContextSnapshot candidate{};
    int readStatus = -ENOENT;
    bool found = false;
    for (int scanAttempt = 0; scanAttempt < 2 && !found; ++scanAttempt) {
        std::vector<TaskThreadIdentity> identities;
        readStatus = locator_.FindAllExact(
            processId_,
            threadName_,
            identities);
        if (readStatus != 0) {
            if (scanAttempt == 0 && IsRetryableContextError(readStatus)) {
                continue;
            }
            return InvalidateLocked(readStatus);
        }

        const auto isRejected = [this](const TaskThreadIdentity& identity) {
            return std::find_if(
                       rejected_.begin(),
                       rejected_.end(),
                       [&identity](const TaskThreadIdentity& rejected) {
                           return rejected.threadId == identity.threadId &&
                               rejected.startTimeTicks ==
                                   identity.startTimeTicks;
                       }) != rejected_.end();
        };
        if (!identities.empty() &&
            std::all_of(
                identities.begin(), identities.end(), isRejected)) {
            rejected_.clear();
        }

        if (current_.IsUsable()) {
            const auto preferred = std::find_if(
                identities.begin(),
                identities.end(),
                [this](const TaskThreadIdentity& identity) {
                    return identity.threadId == current_.threadId &&
                        identity.startTimeTicks ==
                            current_.threadStartTimeTicks;
                });
            if (preferred != identities.end() &&
                preferred != identities.begin()) {
                std::rotate(
                    identities.begin(),
                    preferred,
                    preferred + 1);
            }
        }

        bool retryableFailure = false;
        for (const TaskThreadIdentity& identity : identities) {
            if (isRejected(identity)) continue;
            std::uint64_t tpidrEl0 = 0;
            ThreadPacKeys keys{};
            int candidateStatus = reader_.ReadTpidrEl0(
                identity.threadId,
                tpidrEl0);
            if (candidateStatus == 0) {
                candidateStatus = reader_.ReadPacKeys(
                    identity.threadId,
                    keys);
            }
            if (candidateStatus != 0) {
                readStatus = candidateStatus;
                retryableFailure = retryableFailure ||
                    IsRetryableContextError(candidateStatus);
                continue;
            }

            const ThreadExecutionContextSnapshot resolved{
                processId_,
                identity.threadId,
                identity.startTimeTicks,
                tpidrEl0,
                keys.apga,
                0,
            };
            if (!resolved.IsUsable()) {
                readStatus = -ENODATA;
                continue;
            }
            candidate = resolved;
            found = true;
            break;
        }
        if (!found && !(scanAttempt == 0 && retryableFailure)) {
            return InvalidateLocked(readStatus);
        }
    }
    if (!found) return InvalidateLocked(readStatus);

    ThreadExecutionContextEvent event =
        ThreadExecutionContextEvent::Acquired;
    if (current_.IsUsable()) {
        if (current_.threadId != candidate.threadId ||
            current_.threadStartTimeTicks !=
                candidate.threadStartTimeTicks) {
            event = ThreadExecutionContextEvent::ThreadRecreated;
        } else if (current_.tpidrEl0 != candidate.tpidrEl0 ||
                   current_.apga.low != candidate.apga.low ||
                   current_.apga.high != candidate.apga.high) {
            event = ThreadExecutionContextEvent::ValuesChanged;
        } else {
            event = ThreadExecutionContextEvent::Stable;
        }
    }

    if (event == ThreadExecutionContextEvent::Stable) {
        candidate.generation = current_.generation;
    } else {
        generation_ = NextGeneration(generation_);
        candidate.generation = generation_;
    }
    current_ = candidate;
    return {event, 0, current_};
}

bool ThreadExecutionContextProvider::Current(
    ThreadExecutionContextSnapshot& snapshot) const {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot = current_;
    return snapshot.IsUsable();
}

bool ThreadExecutionContextProvider::RejectCurrent() noexcept {
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
    return true;
}

void ThreadExecutionContextProvider::Reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = {};
    rejected_.clear();
    generation_ = 0;
}

ThreadExecutionContextRefresh
ThreadExecutionContextProvider::InvalidateLocked(int status) {
    const bool hadContext = current_.IsUsable();
    current_ = {};
    return {
        hadContext ? ThreadExecutionContextEvent::Lost
                   : ThreadExecutionContextEvent::Missing,
        status != 0 ? status : -EIO,
        {},
    };
}

}  // namespace lengjing::game::native
