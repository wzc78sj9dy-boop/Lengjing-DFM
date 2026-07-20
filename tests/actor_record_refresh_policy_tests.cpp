#include "game/native/ActorRecordRefreshPolicy.h"
#include "test_support.h"

#include <chrono>

void RunActorRecordRefreshPolicyTests() {
    using Key = lengjing::game::native::ActorRecordSnapshotKey;
    using Policy = lengjing::game::native::ActorRecordRefreshPolicy;
    using namespace std::chrono_literals;

    const Key base{
        0x10000000,
        0x20000000,
        0x30000000,
        0x40000000,
        120,
        0x50000000,
        false,
    };
    const Policy::TimePoint start{};
    Policy policy(100ms);

    REQUIRE(policy.ShouldRefresh(base, start));
    policy.MarkRefreshed(base, start);
    REQUIRE(!policy.ShouldRefresh(base, start + 99ms));
    REQUIRE(policy.ShouldRefresh(base, start + 100ms));

    const auto requireKeyChangeRefresh = [&](Key changed) {
        REQUIRE(policy.ShouldRefresh(changed, start + 1ms));
    };

    Key changed = base;
    changed.moduleBase += 0x1000;
    requireKeyChangeRefresh(changed);
    changed = base;
    changed.world += 0x1000;
    requireKeyChangeRefresh(changed);
    changed = base;
    changed.level += 0x1000;
    requireKeyChangeRefresh(changed);
    changed = base;
    changed.actorArray += 0x1000;
    requireKeyChangeRefresh(changed);
    changed = base;
    ++changed.actorCount;
    requireKeyChangeRefresh(changed);
    changed = base;
    changed.localPawn += 0x1000;
    requireKeyChangeRefresh(changed);
    changed = base;
    changed.decodedRequired = true;
    requireKeyChangeRefresh(changed);

    const Key decoded{
        base.moduleBase,
        base.world,
        base.level,
        base.actorArray,
        base.actorCount,
        base.localPawn,
        true,
    };
    policy.MarkRefreshed(decoded, start + 1ms);
    REQUIRE(policy.ShouldRefresh(base, start + 2ms));

    policy.Invalidate();
    REQUIRE(policy.ShouldRefresh(base, start + 2ms));
}
