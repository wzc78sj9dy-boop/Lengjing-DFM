#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace lengjing::render {

class PresentationRateTracker final {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    void Reset();
    void Record();
    float Read() const;

    void RecordAt(TimePoint now);
    float ReadAt(TimePoint now) const;

private:
    mutable std::mutex mutex_;
    TimePoint windowStart_{};
    TimePoint lastPresented_{};
    std::uint32_t windowFrames_ = 0;
    float publishedRate_ = 0.0f;
};

}  // namespace lengjing::render
