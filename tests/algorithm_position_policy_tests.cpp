#include "game/native/AlgorithmPositionPolicy.h"
#include "game/native/AlgorithmReplayPolicy.h"
#include "game/native/MemoryTransport.h"
#include "test_support.h"

#include <chrono>
#include <limits>

void RunAlgorithmPositionPolicyTests() {
    using lengjing::game::native::AlgorithmPosition;
    using lengjing::game::native::AlgorithmPositionFirstDecision;
    using lengjing::game::native::AlgorithmPositionHistorySample;
    using lengjing::game::native::AlgorithmPositionResultCache;
    using lengjing::game::native::AlgorithmPositionRuntimeConfig;
    using lengjing::game::native::AlgorithmPositionRefreshPlan;
    using lengjing::game::native::AlgorithmPositionSecondDecision;
    using lengjing::game::native::AlgorithmPositionPendingDecision;
    using lengjing::game::native::AlgorithmPositionPendingSample;
    using lengjing::game::native::AlgorithmExecutionContextRefreshKey;
    using lengjing::game::native::AlgorithmExecutionContextRefreshPolicy;
    using lengjing::game::native::AlgorithmReplayBackoffPolicy;
    using lengjing::game::native::AlgorithmReplayPageKey;
    using lengjing::game::native::AlgorithmReplayPagePolicy;
    using lengjing::game::native::EvaluateAlgorithmPositionFirst;
    using lengjing::game::native::EvaluateAlgorithmPositionSecond;
    using lengjing::game::native::ObserveAlgorithmPositionPending;
    using lengjing::game::native::FormatAlgorithmPacgaResult;
    using lengjing::game::native::MakeAlgorithmPositionRefreshPlan;
    using lengjing::game::native::MakeAlgorithmPositionRefreshPlan;
    using lengjing::game::native::ParseAlgorithmPositionDecryptRva;
    using lengjing::game::native::ProcessExecutionContext;
    using lengjing::game::native::ShouldAttemptAlgorithmPosition;
    using lengjing::game::native::ShouldRequireAlgorithmPosition;
    using namespace std::chrono_literals;

    constexpr std::uintptr_t entity = 0x100000;
    constexpr std::uintptr_t otherEntity = 0x200000;
    const AlgorithmPositionResultCache::TimePoint start{};
    const AlgorithmPosition history{0.0f, 0.0f, 0.0f};

    constexpr AlgorithmPositionRefreshPlan poolRefresh =
        MakeAlgorithmPositionRefreshPlan(false);
    REQUIRE(!poolRefresh.first);
    REQUIRE(poolRefresh.discarded);
    REQUIRE(!poolRefresh.candidate);

    static_assert(FormatAlgorithmPacgaResult(
                      UINT64_C(0x123456789ABCDEF0)) ==
                  UINT64_C(0x1234567800000000));
    REQUIRE(FormatAlgorithmPacgaResult(UINT64_MAX) ==
            UINT64_C(0xFFFFFFFF00000000));
    REQUIRE(FormatAlgorithmPacgaResult(UINT64_C(0x00000000FFFFFFFF)) == 0);

    AlgorithmPositionResultCache cache;
    AlgorithmPositionHistorySample sample{};
    REQUIRE(!cache.Lookup(entity, start, sample));

    cache.Store(entity, history, start);
    REQUIRE(cache.Lookup(entity, start + 1500ms, sample));
    REQUIRE(sample.position.x == history.x);
    REQUIRE(sample.position.y == history.y);
    REQUIRE(sample.position.z == history.z);
    REQUIRE(!cache.Lookup(otherEntity, start + 1500ms, sample));
    REQUIRE(!cache.Lookup(entity, start + 1501ms, sample));

    cache.Store(entity, history, start + 2000ms);
    cache.Store(otherEntity, history, start + 1000ms);
    REQUIRE(cache.Lookup(entity, start + 2501ms, sample));
    REQUIRE(!cache.Lookup(otherEntity, start + 2501ms, sample));
    cache.Clear();
    REQUIRE(!cache.Lookup(entity, start + 2501ms, sample));

    const AlgorithmPosition firstAtLimit{900.0f, 1200.0f, 0.0f};
    REQUIRE(EvaluateAlgorithmPositionFirst(history, firstAtLimit) ==
            AlgorithmPositionFirstDecision::AcceptFirst);
    const AlgorithmPosition firstOutsideLimit{901.0f, 1200.0f, 0.0f};
    REQUIRE(EvaluateAlgorithmPositionFirst(history, firstOutsideLimit) ==
            AlgorithmPositionFirstDecision::Rerun);

    const AlgorithmPosition first{2000.0f, 0.0f, 0.0f};
    const AlgorithmPosition secondNearHistory{300.0f, 400.0f, 0.0f};
    REQUIRE(EvaluateAlgorithmPositionSecond(
                history, first, &secondNearHistory) ==
            AlgorithmPositionSecondDecision::AcceptSecond);

    const AlgorithmPosition secondNearFirst{1700.0f, 400.0f, 0.0f};
    REQUIRE(EvaluateAlgorithmPositionSecond(
                history, first, &secondNearFirst) ==
            AlgorithmPositionSecondDecision::NeedsCrossFrameConfirmation);

    const AlgorithmPosition inconsistentSecond{1000.0f, 1000.0f, 0.0f};
    REQUIRE(EvaluateAlgorithmPositionSecond(
                history, first, &inconsistentSecond) ==
            AlgorithmPositionSecondDecision::FallbackHistory);
    REQUIRE(EvaluateAlgorithmPositionSecond(history, first, nullptr) ==
            AlgorithmPositionSecondDecision::FallbackHistory);

    AlgorithmPositionPendingSample pending;
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{2000.0f, 0.0f, 0.0f},
                10,
                start + 10ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(pending.count == 1);
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{2000.0f, 0.0f, 0.0f},
                10,
                start + 11ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(pending.count == 1);
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{2050.0f, 20.0f, 0.0f},
                11,
                start + 20ms) ==
            AlgorithmPositionPendingDecision::Confirmed);
    REQUIRE(pending.count == 0);

    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{4000.0f, 0.0f, 0.0f},
                20,
                start + 30ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{5000.0f, 0.0f, 0.0f},
                21,
                start + 40ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{7000.0f, 0.0f, 0.0f},
                23,
                start + 50ms) ==
            AlgorithmPositionPendingDecision::Confirmed);
    REQUIRE(pending.count == 0);

    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{8000.0f, 0.0f, 0.0f},
                30,
                start + 60ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{12000.0f, 0.0f, 0.0f},
                31,
                start + 70ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{8000.0f, 0.0f, 0.0f},
                32,
                start + 80ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(pending.count == 1);

    REQUIRE(ObserveAlgorithmPositionPending(
                pending,
                AlgorithmPosition{9000.0f, 0.0f, 0.0f},
                33,
                start + 400ms) ==
            AlgorithmPositionPendingDecision::Pending);
    REQUIRE(pending.count == 1);

    constexpr auto refreshPlan = MakeAlgorithmPositionRefreshPlan(false);
    static_assert(!refreshPlan.first);
    static_assert(refreshPlan.discarded);
    static_assert(!refreshPlan.candidate);

    const AlgorithmPositionRuntimeConfig configured{0x1234};
    REQUIRE(configured.IsConfigured());
    REQUIRE(!AlgorithmPositionRuntimeConfig{}.IsConfigured());
    REQUIRE(!AlgorithmPositionRuntimeConfig{2}.IsConfigured());
    REQUIRE(!AlgorithmPositionRuntimeConfig{0x1235}.IsConfigured());
    if constexpr (sizeof(std::uintptr_t) > sizeof(std::uint32_t)) {
        REQUIRE(!AlgorithmPositionRuntimeConfig{
            static_cast<std::uintptr_t>(
                std::numeric_limits<std::uint32_t>::max()) + 1U}
                     .IsConfigured());
    }

    REQUIRE(ParseAlgorithmPositionDecryptRva("0x1234").decryptRva ==
            configured.decryptRva);
    REQUIRE(ParseAlgorithmPositionDecryptRva("0X1234").decryptRva ==
            configured.decryptRva);
    REQUIRE(ParseAlgorithmPositionDecryptRva("1234").decryptRva ==
            configured.decryptRva);
    REQUIRE(!ParseAlgorithmPositionDecryptRva("0x0").IsConfigured());
    REQUIRE(!ParseAlgorithmPositionDecryptRva("0x1235").IsConfigured());
    REQUIRE(!ParseAlgorithmPositionDecryptRva("0x12xz").IsConfigured());

    REQUIRE(ShouldAttemptAlgorithmPosition(
        false, true, true, configured));
    REQUIRE(!ShouldAttemptAlgorithmPosition(
        true, true, true, configured));
    REQUIRE(!ShouldAttemptAlgorithmPosition(
        false, false, true, configured));
    REQUIRE(!ShouldAttemptAlgorithmPosition(
        false, true, false, configured));
    REQUIRE(!ShouldAttemptAlgorithmPosition(
        false, true, true, AlgorithmPositionRuntimeConfig{}));

    REQUIRE(ShouldRequireAlgorithmPosition(false, true));
    REQUIRE(!ShouldRequireAlgorithmPosition(true, true));
    REQUIRE(!ShouldRequireAlgorithmPosition(false, false));

    REQUIRE(!ProcessExecutionContext{}.IsUsable());
    REQUIRE((!ProcessExecutionContext{1, 0, 0, 7}.IsUsable()));
    REQUIRE((!ProcessExecutionContext{0, 1, 0, 7}.IsUsable()));
    REQUIRE((!ProcessExecutionContext{1, 1, 0, 0}.IsUsable()));
    REQUIRE((ProcessExecutionContext{1, 1, 0, 7}.IsUsable()));
    REQUIRE((ProcessExecutionContext{1, 0, 1, 7}.IsUsable()));

    const AlgorithmExecutionContextRefreshPolicy::TimePoint contextStart{};
    const AlgorithmExecutionContextRefreshKey contextKey{
        123,
        0x100000,
        0x101000,
        0xD503201F,
        false,
    };
    AlgorithmExecutionContextRefreshPolicy contextRefresh(100ms);
    REQUIRE(contextRefresh.ShouldRefresh(contextKey, contextStart));
    contextRefresh.MarkSucceeded(contextKey, contextStart);
    REQUIRE(!contextRefresh.ShouldRefresh(contextKey, contextStart));
    REQUIRE(!contextRefresh.ShouldRefresh(
        contextKey, contextStart + 99ms));
    REQUIRE(contextRefresh.ShouldRefresh(
        contextKey, contextStart + 100ms));

    const auto requireContextRefresh =
        [contextKey, contextStart](
            AlgorithmExecutionContextRefreshKey changed) {
        AlgorithmExecutionContextRefreshPolicy policy(100ms);
        policy.MarkSucceeded(contextKey, contextStart);
        REQUIRE(policy.ShouldRefresh(changed, contextStart + 1ms));
        policy.MarkSucceeded(changed, contextStart + 1ms);
        REQUIRE(!policy.ShouldRefresh(changed, contextStart + 2ms));
    };
    AlgorithmExecutionContextRefreshKey changedContextKey = contextKey;
    ++changedContextKey.processId;
    requireContextRefresh(changedContextKey);
    changedContextKey = contextKey;
    ++changedContextKey.moduleBase;
    requireContextRefresh(changedContextKey);
    changedContextKey = contextKey;
    changedContextKey.guestPc += 4;
    requireContextRefresh(changedContextKey);
    changedContextKey = contextKey;
    ++changedContextKey.entryInstruction;
    requireContextRefresh(changedContextKey);
    changedContextKey = contextKey;
    changedContextKey.coordinatePoolSelected = true;
    requireContextRefresh(changedContextKey);

    contextRefresh.MarkSucceeded(contextKey, contextStart + 101ms);
    contextRefresh.MarkFailed();
    REQUIRE(contextRefresh.ShouldRefresh(contextKey, contextStart + 102ms));
    contextRefresh.MarkSucceeded(contextKey, contextStart + 103ms);
    contextRefresh.Invalidate();
    REQUIRE(contextRefresh.ShouldRefresh(contextKey, contextStart + 104ms));

    AlgorithmExecutionContextRefreshPolicy noContextCache(0ms);
    noContextCache.MarkSucceeded(contextKey, contextStart);
    REQUIRE(noContextCache.ShouldRefresh(contextKey, contextStart));

    const AlgorithmReplayBackoffPolicy::TimePoint replayStart{};
    AlgorithmReplayBackoffPolicy replayBackoff(100ms, 2);
    REQUIRE(replayBackoff.BeginFrame(replayStart));
    replayBackoff.ObserveFrame(15, 0, replayStart);
    REQUIRE(!replayBackoff.IsBackingOff());
    REQUIRE(replayBackoff.BeginFrame(replayStart + 1ms));
    replayBackoff.ObserveFrame(15, 0, replayStart + 1ms);
    REQUIRE(replayBackoff.IsBackingOff());
    REQUIRE(!replayBackoff.BeginFrame(replayStart + 100ms));
    REQUIRE(replayBackoff.BeginFrame(replayStart + 101ms));
    REQUIRE(!replayBackoff.BeginFrame(replayStart + 200ms));
    REQUIRE(replayBackoff.BeginFrame(replayStart + 201ms));
    replayBackoff.MarkSucceeded();
    REQUIRE(!replayBackoff.IsBackingOff());
    REQUIRE(replayBackoff.BeginFrame(replayStart + 202ms));

    replayBackoff.ObserveFrame(0, 0, replayStart + 203ms);
    REQUIRE(!replayBackoff.IsBackingOff());
    replayBackoff.ObserveFrame(10, 1, replayStart + 204ms);
    REQUIRE(!replayBackoff.IsBackingOff());

    AlgorithmReplayBackoffPolicy immediateBackoff(100ms, 0);
    immediateBackoff.ObserveFrame(1, 0, replayStart);
    REQUIRE(immediateBackoff.IsBackingOff());
    immediateBackoff.Reset();
    REQUIRE(!immediateBackoff.IsBackingOff());

    const AlgorithmReplayPageKey pageKey{
        0x1000,
        0x2000,
        10,
        300,
        4,
    };
    AlgorithmReplayPagePolicy pagePolicy;
    REQUIRE(pagePolicy.ConsumeRefresh(pageKey));
    REQUIRE(!pagePolicy.ConsumeRefresh(pageKey));
    pagePolicy.BeginFrame();
    REQUIRE(pagePolicy.ConsumeRefresh(pageKey));
    REQUIRE(!pagePolicy.ConsumeRefresh(pageKey));

    const auto requirePageRefresh = [&](AlgorithmReplayPageKey changed) {
        pagePolicy.Invalidate();
        REQUIRE(pagePolicy.ConsumeRefresh(pageKey));
        REQUIRE(pagePolicy.ConsumeRefresh(changed));
        REQUIRE(!pagePolicy.ConsumeRefresh(changed));
    };
    AlgorithmReplayPageKey changedPageKey = pageKey;
    changedPageKey.guestPc += 4;
    requirePageRefresh(changedPageKey);
    changedPageKey = pageKey;
    ++changedPageKey.tpidrEl0;
    requirePageRefresh(changedPageKey);
    changedPageKey = pageKey;
    ++changedPageKey.threadId;
    requirePageRefresh(changedPageKey);
    changedPageKey = pageKey;
    ++changedPageKey.threadStartTimeTicks;
    requirePageRefresh(changedPageKey);
    changedPageKey = pageKey;
    ++changedPageKey.generation;
    requirePageRefresh(changedPageKey);
}
