#pragma once

#include "game/native/MemoryTransport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>

namespace lengjing::game::native {

class ExecutionBreakpointRecordStore final {
public:
    bool Store(const ExecutionBreakpointRecord& record) noexcept {
        if (record.tid <= 0) return false;

        const std::uint64_t nextSequence =
            updateSequence_ == std::numeric_limits<std::uint64_t>::max()
            ? updateSequence_
            : updateSequence_ + 1;
        try {
            auto inserted = latest_.try_emplace(record.tid);
            inserted.first->second.record = record;
            inserted.first->second.updateSequence = nextSequence;
            updateSequence_ = nextSequence;

            if (inserted.second &&
                latest_.size() > kExecutionBreakpointRecordLimit) {
                auto oldest = latest_.begin();
                for (auto candidate = latest_.begin();
                     candidate != latest_.end();
                     ++candidate) {
                    if (candidate->second.updateSequence <
                        oldest->second.updateSequence) {
                        oldest = candidate;
                    }
                }
                latest_.erase(oldest);
            }

            if (history_.size() >= kExecutionBreakpointRecordLimit) {
                history_.pop_front();
            }
            history_.push_back(record);
            return true;
        } catch (...) {
            return false;
        }
    }

    const ExecutionBreakpointRecord* FindLatest(pid_t tid) const noexcept {
        const auto found = latest_.find(tid);
        return found == latest_.end() ? nullptr : &found->second.record;
    }

    std::size_t CopyMerged(
        ExecutionBreakpointRecord* output,
        std::size_t capacity,
        std::size_t& totalRecords) const noexcept {
        std::array<
            const ExecutionBreakpointRecord*,
            kExecutionBreakpointRecordLimit> historySupplement{};
        std::size_t supplementCount = 0;
        for (auto iterator = history_.rbegin();
             iterator != history_.rend() &&
             latest_.size() + supplementCount <
                 kExecutionBreakpointRecordLimit;
             ++iterator) {
            const auto latest = latest_.find(iterator->tid);
            bool duplicate = latest != latest_.end() &&
                SameRecordIdentity(
                    *iterator, latest->second.record);
            if (!duplicate) {
                for (std::size_t index = 0;
                     index < supplementCount;
                     ++index) {
                    if (SameRecordIdentity(
                            *iterator, *historySupplement[index])) {
                        duplicate = true;
                        break;
                    }
                }
            }
            if (!duplicate) {
                historySupplement[supplementCount++] = &*iterator;
            }
        }

        totalRecords = latest_.size() + supplementCount;
        if (output == nullptr || capacity == 0) return 0;
        std::size_t copied = 0;
        for (const auto& entry : latest_) {
            if (copied == capacity) return copied;
            output[copied++] = entry.second.record;
        }
        for (std::size_t index = supplementCount;
             index != 0 && copied < capacity;
             --index) {
            output[copied++] = *historySupplement[index - 1];
        }
        return copied;
    }

    void Clear() noexcept {
        latest_.clear();
        history_.clear();
        updateSequence_ = 0;
    }

    std::size_t LatestSize() const noexcept {
        return latest_.size();
    }

    std::size_t HistorySize() const noexcept {
        return history_.size();
    }

private:
    struct SavedRecord {
        ExecutionBreakpointRecord record{};
        std::uint64_t updateSequence = 0;
    };

    static bool SameRecordIdentity(
        const ExecutionBreakpointRecord& left,
        const ExecutionBreakpointRecord& right) noexcept {
        return left.tid == right.tid &&
            left.hitCount == right.hitCount &&
            left.pc == right.pc;
    }

    std::map<pid_t, SavedRecord> latest_;
    std::deque<ExecutionBreakpointRecord> history_;
    std::uint64_t updateSequence_ = 0;
};

}  // namespace lengjing::game::native
