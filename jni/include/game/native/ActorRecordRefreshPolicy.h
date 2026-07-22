#pragma once

#include <chrono>
#include <cstdint>

namespace lengjing::game::native {

struct ActorRecordSnapshotKey {
    std::uintptr_t moduleBase = 0;
    std::uintptr_t world = 0;
    std::uintptr_t level = 0;
    std::uintptr_t actorArray = 0;
    std::int32_t actorCount = 0;
    std::uintptr_t localPawn = 0;
    bool decodedRequired = false;
};

struct ActorRecordSnapshotIdentity {
    std::uintptr_t moduleBase = 0;
    std::uintptr_t world = 0;
    std::uintptr_t level = 0;
    std::uintptr_t localPawn = 0;
};

constexpr ActorRecordSnapshotIdentity ActorRecordIdentity(
    const ActorRecordSnapshotKey& key) noexcept {
    return {
        key.moduleBase,
        key.world,
        key.level,
        key.localPawn,
    };
}

constexpr bool operator==(const ActorRecordSnapshotIdentity& left,
                          const ActorRecordSnapshotIdentity& right) noexcept {
    return left.moduleBase == right.moduleBase &&
        left.world == right.world && left.level == right.level &&
        left.localPawn == right.localPawn;
}

inline bool CanRetainDecodedActorSnapshot(
    const ActorRecordSnapshotIdentity& cached,
    const ActorRecordSnapshotIdentity& current,
    bool cachedReady,
    bool currentReady,
    std::chrono::steady_clock::time_point capturedAt,
    std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::duration retention =
        std::chrono::milliseconds(1500)) noexcept {
    return !currentReady && cachedReady && cached == current &&
        capturedAt.time_since_epoch().count() != 0 && now >= capturedAt &&
        now - capturedAt <= retention;
}

constexpr bool operator==(const ActorRecordSnapshotKey& left,
                          const ActorRecordSnapshotKey& right) noexcept {
    return left.moduleBase == right.moduleBase &&
        left.world == right.world && left.level == right.level &&
        left.actorArray == right.actorArray &&
        left.actorCount == right.actorCount &&
        left.localPawn == right.localPawn &&
        left.decodedRequired == right.decodedRequired;
}

constexpr bool operator!=(const ActorRecordSnapshotKey& left,
                          const ActorRecordSnapshotKey& right) noexcept {
    return !(left == right);
}

class ActorRecordRefreshPolicy final {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using TimePoint = Clock::time_point;

    explicit ActorRecordRefreshPolicy(
        Duration interval = std::chrono::milliseconds(100)) noexcept
        : interval_(interval) {}

    bool ShouldRefresh(const ActorRecordSnapshotKey& key,
                       TimePoint now) const noexcept {
        return !ready_ || key_ != key || now >= refreshAt_;
    }

    void MarkRefreshed(const ActorRecordSnapshotKey& key,
                       TimePoint now) noexcept {
        key_ = key;
        refreshAt_ = now + interval_;
        ready_ = true;
    }

    void Invalidate() noexcept {
        key_ = ActorRecordSnapshotKey{};
        refreshAt_ = TimePoint{};
        ready_ = false;
    }

private:
    Duration interval_{};
    ActorRecordSnapshotKey key_{};
    TimePoint refreshAt_{};
    bool ready_ = false;
};

}  // namespace lengjing::game::native
