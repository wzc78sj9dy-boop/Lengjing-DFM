#pragma once

#include <chrono>
#include <cstdint>

namespace lengjing::game::native {

class WorldObjectRefreshPolicy final {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    explicit WorldObjectRefreshPolicy(
        Duration interval = std::chrono::milliseconds(500)) noexcept
        : interval_(interval) {}

    bool ShouldRefresh(std::uintptr_t world,
                       std::uint64_t settingsSignature,
                       TimePoint now) const noexcept {
        return world != 0 &&
            (!ready_ || world_ != world ||
             settingsSignature_ != settingsSignature || now >= refreshAt_);
    }

    void MarkRefreshed(std::uintptr_t world,
                       std::uint64_t settingsSignature,
                       TimePoint now) noexcept {
        world_ = world;
        settingsSignature_ = settingsSignature;
        refreshAt_ = now + interval_;
        ready_ = world != 0;
    }

    void Invalidate() noexcept {
        world_ = 0;
        settingsSignature_ = 0;
        refreshAt_ = TimePoint{};
        ready_ = false;
    }

private:
    Duration interval_{};
    std::uintptr_t world_ = 0;
    std::uint64_t settingsSignature_ = 0;
    TimePoint refreshAt_{};
    bool ready_ = false;
};

}  // namespace lengjing::game::native
