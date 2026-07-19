#include "render/CpuMailboxPolicy.h"

#include "test_support.h"

void RunCpuMailboxPolicyTests() {
    using lengjing::render::CpuMailboxGenerationSnapshot;
    using lengjing::render::CpuMailboxNeedsCompleteCopy;

    const CpuMailboxGenerationSnapshot producerSnapshot{3, 5};
    const std::uint64_t consumerBufferGeneration = 3;
    const std::uint64_t consumerPresentedGeneration = 5;

    REQUIRE(producerSnapshot.ReusableGeneration() == 3);
    REQUIRE(4 > producerSnapshot.ReusableGeneration());
    REQUIRE(5 > producerSnapshot.ReusableGeneration());
    REQUIRE(CpuMailboxNeedsCompleteCopy(
        producerSnapshot.availableBufferGeneration,
        consumerBufferGeneration));

    const CpuMailboxGenerationSnapshot consumerFirstSnapshot{
        consumerPresentedGeneration,
        consumerBufferGeneration,
    };
    REQUIRE(consumerFirstSnapshot.ReusableGeneration() == 3);

    const CpuMailboxGenerationSnapshot stableSnapshot{5, 5};
    REQUIRE(stableSnapshot.ReusableGeneration() == 5);
    REQUIRE(!CpuMailboxNeedsCompleteCopy(
        stableSnapshot.availableBufferGeneration,
        stableSnapshot.availableBufferGeneration));
}
