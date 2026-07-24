#include "game/native/CoordinateEntryBranchPolicy.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

int main() {
    using namespace lengjing::game::native;

    constexpr std::uint64_t kBase = UINT64_C(0x100000);
    std::uint64_t target = 0;
    REQUIRE(DecodeArm64UnconditionalImmediateBranch(
                kBase, UINT32_C(0x14000002), target) ==
            CoordinateEntryBranchDecodeStatus::Resolved);
    REQUIRE(target == kBase + 8U);
    REQUIRE(DecodeArm64UnconditionalImmediateBranch(
                kBase, UINT32_C(0x17FFFFFF), target) ==
            CoordinateEntryBranchDecodeStatus::Resolved);
    REQUIRE(target == kBase - 4U);
    REQUIRE(DecodeArm64UnconditionalImmediateBranch(
                kBase, UINT32_C(0x54000040), target) ==
            CoordinateEntryBranchDecodeStatus::NotBranch);
    REQUIRE(DecodeArm64UnconditionalImmediateBranch(
                kBase, UINT32_C(0x94000002), target) ==
            CoordinateEntryBranchDecodeStatus::NotBranch);

    const auto resolve = [](
                             std::uint64_t entry,
                             const std::unordered_map<
                                 std::uint64_t,
                                 std::uint32_t>& instructions,
                             const std::unordered_set<std::uint64_t>& code) {
        return ResolveCoordinateEntryBranchChain(
            entry,
            [&instructions](std::uint64_t address, std::uint32_t& word) {
                const auto found = instructions.find(address);
                if (found == instructions.end()) return false;
                word = found->second;
                return true;
            },
            [&code](std::uint64_t address, std::size_t size) {
                return size == sizeof(std::uint32_t) &&
                    code.count(address) != 0;
            });
    };

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kBase, UINT32_C(0x14000002)},
            {kBase + 4U, UINT32_C(0xD65F03C0)},
            {kBase + 8U, UINT32_C(0x14000001)},
            {kBase + 12U, UINT32_C(0xFC190FE8)},
        };
        const std::unordered_set<std::uint64_t> code{
            kBase, kBase + 4U, kBase + 8U, kBase + 12U};
        const auto result = resolve(kBase, instructions, code);
        REQUIRE(result.status == CoordinateEntryResolveStatus::Resolved);
        REQUIRE(result.resolvedEntry == kBase + 12U);
        REQUIRE(result.hopCount == 2);
        REQUIRE(result.observationCount == 3);
        REQUIRE(result.observations[1].address == kBase + 8U);
    }

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kBase, UINT32_C(0x54000040)},
        };
        const std::unordered_set<std::uint64_t> code{kBase};
        const auto result = resolve(kBase, instructions, code);
        REQUIRE(result.status == CoordinateEntryResolveStatus::Resolved);
        REQUIRE(result.resolvedEntry == kBase);
        REQUIRE(result.hopCount == 0);
    }

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kBase, UINT32_C(0x94000002)},
        };
        const std::unordered_set<std::uint64_t> code{kBase};
        const auto result = resolve(kBase, instructions, code);
        REQUIRE(result.status == CoordinateEntryResolveStatus::Resolved);
        REQUIRE(result.resolvedEntry == kBase);
        REQUIRE(result.hopCount == 0);
        REQUIRE(result.observations[0].instruction ==
            UINT32_C(0x94000002));
    }

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions;
        const std::unordered_set<std::uint64_t> code{kBase};
        const auto result = resolve(kBase, instructions, code);
        REQUIRE(result.status == CoordinateEntryResolveStatus::ReadFailed);
        REQUIRE(result.resolvedEntry == kBase);
        REQUIRE(result.observationCount == 0);
    }

    {
        constexpr std::uint64_t kPageEnd = kBase + UINT64_C(0xFFC);
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kPageEnd, UINT32_C(0x14000001)},
            {kPageEnd + 4U, UINT32_C(0xD65F03C0)},
        };
        const std::unordered_set<std::uint64_t> code{
            kPageEnd,
            kPageEnd + 4U,
        };
        const auto result = resolve(kPageEnd, instructions, code);
        REQUIRE(result.status == CoordinateEntryResolveStatus::Resolved);
        REQUIRE(result.resolvedEntry == kPageEnd + 4U);
        REQUIRE(result.hopCount == 1);
        REQUIRE(result.observationCount == 2);
    }

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kBase, UINT32_C(0x14000000)},
        };
        const std::unordered_set<std::uint64_t> code{kBase};
        REQUIRE(resolve(kBase, instructions, code).status ==
            CoordinateEntryResolveStatus::Loop);
    }

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kBase, UINT32_C(0x14000001)},
            {kBase + 4U, UINT32_C(0x17FFFFFF)},
        };
        const std::unordered_set<std::uint64_t> code{kBase, kBase + 4U};
        REQUIRE(resolve(kBase, instructions, code).status ==
            CoordinateEntryResolveStatus::Loop);
    }

    {
        const std::unordered_map<std::uint64_t, std::uint32_t> instructions{
            {kBase, UINT32_C(0x14000002)},
        };
        const std::unordered_set<std::uint64_t> code{kBase};
        REQUIRE(resolve(kBase, instructions, code).status ==
            CoordinateEntryResolveStatus::TargetOutsideExecutable);
    }

    {
        std::unordered_map<std::uint64_t, std::uint32_t> instructions;
        std::unordered_set<std::uint64_t> code;
        for (std::size_t index = 0;
             index <= kCoordinateEntryMaximumDirectBranchHops;
             ++index) {
            const std::uint64_t address = kBase + index * 4U;
            instructions[address] = index ==
                    kCoordinateEntryMaximumDirectBranchHops
                ? UINT32_C(0xD65F03C0)
                : UINT32_C(0x14000001);
            code.insert(address);
        }
        const auto result = resolve(kBase, instructions, code);
        REQUIRE(result.status == CoordinateEntryResolveStatus::Resolved);
        REQUIRE(result.hopCount == kCoordinateEntryMaximumDirectBranchHops);
        REQUIRE(result.observationCount ==
            kCoordinateEntryMaximumDirectBranchHops + 1);

        instructions[kBase +
            kCoordinateEntryMaximumDirectBranchHops * 4U] =
            UINT32_C(0x14000001);
        code.insert(kBase +
            (kCoordinateEntryMaximumDirectBranchHops + 1) * 4U);
        REQUIRE(resolve(kBase, instructions, code).status ==
            CoordinateEntryResolveStatus::HopLimit);
    }

    return 0;
}
