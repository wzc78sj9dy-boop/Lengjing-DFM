#include "render/PresentationRateTracker.h"

namespace lengjing::render {
namespace {

constexpr auto kPublishInterval = std::chrono::milliseconds(500);
constexpr auto kStaleInterval = std::chrono::milliseconds(1500);
constexpr auto kEarlySampleInterval = std::chrono::milliseconds(100);

}  // namespace

void PresentationRateTracker::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    windowStart_ = {};
    lastPresented_ = {};
    windowFrames_ = 0;
    publishedRate_ = 0.0f;
}

void PresentationRateTracker::Record() {
    RecordAt(Clock::now());
}

void PresentationRateTracker::RecordAt(TimePoint now) {
    std::lock_guard<std::mutex> lock(mutex_);
    lastPresented_ = now;
    if (windowStart_ == TimePoint{}) {
        windowStart_ = now;
        windowFrames_ = 0;
        return;
    }

    ++windowFrames_;
    const Clock::duration elapsed = now - windowStart_;
    if (elapsed < kPublishInterval) {
        return;
    }

    const float seconds = std::chrono::duration<float>(elapsed).count();
    publishedRate_ = seconds > 0.0f
        ? static_cast<float>(windowFrames_) / seconds
        : 0.0f;
    windowStart_ = now;
    windowFrames_ = 0;
}

float PresentationRateTracker::Read() const {
    return ReadAt(Clock::now());
}

float PresentationRateTracker::ReadAt(TimePoint now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lastPresented_ == TimePoint{} ||
        now - lastPresented_ > kStaleInterval) {
        return 0.0f;
    }
    if (publishedRate_ > 0.0f) {
        return publishedRate_;
    }

    const Clock::duration elapsed = now - windowStart_;
    if (windowFrames_ < 2 || elapsed < kEarlySampleInterval) {
        return 0.0f;
    }
    const float seconds = std::chrono::duration<float>(elapsed).count();
    return seconds > 0.0f
        ? static_cast<float>(windowFrames_) / seconds
        : 0.0f;
}

}  // namespace lengjing::render
