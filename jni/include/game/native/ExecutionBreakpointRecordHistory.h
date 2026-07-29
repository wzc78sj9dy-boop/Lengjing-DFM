#pragma once

#include "game/native/MemoryTransport.h"

#include <algorithm>
#include <cstddef>
#include <deque>

namespace lengjing::game::native {

class ExecutionBreakpointRecordHistory final {
public:
    void Push(const ExecutionBreakpointRecord& record) {
        if (records_.size() >= kExecutionBreakpointRecordLimit) {
            records_.pop_front();
        }
        records_.push_back(record);
    }

    std::size_t CopyNewest(
        ExecutionBreakpointRecord* output,
        std::size_t capacity) const noexcept {
        if (output == nullptr || capacity == 0) return 0;
        const std::size_t requested =
            std::min(capacity, records_.size());
        const std::size_t first = records_.size() - requested;
        for (std::size_t index = 0; index < requested; ++index) {
            output[index] = records_[first + index];
        }
        return requested;
    }

    void Clear() noexcept {
        records_.clear();
    }

    std::size_t Size() const noexcept {
        return records_.size();
    }

private:
    std::deque<ExecutionBreakpointRecord> records_;
};

}  // namespace lengjing::game::native
