#include "game/native/CoordinateExecutionCandidate.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define REQUIRE(condition)                                                    \
    do {                                                                      \
        if (!(condition)) {                                                   \
            throw std::runtime_error(                                         \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +     \
                ": requirement failed: " #condition);                       \
        }                                                                     \
    } while (false)

namespace {

namespace game = lengjing::game::native;

class SparseMemory {
public:
    virtual ~SparseMemory() = default;

    template <typename T>
    void Write(std::uint64_t address, const T& value) {
        const auto* bytes = reinterpret_cast<const std::byte*>(&value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_[address + index] = bytes[index];
        }
    }

    virtual bool Read(std::uint64_t address,
                      void* output,
                      std::size_t size) {
        reads_.emplace_back(address, size);
        auto* bytes = static_cast<std::byte*>(output);
        std::memset(bytes, 0, size);
        for (auto item = bytes_.lower_bound(address);
             item != bytes_.end() && item->first - address < size;
             ++item) {
            bytes[item->first - address] = item->second;
        }
        return true;
    }

    game::CoordinateExecutionReadCallback Callback() {
        return [this](std::uint64_t address,
                      void* output,
                      std::size_t size) {
            return Read(address, output, size);
        };
    }

    bool HasReadSize(std::size_t size) const {
        return std::any_of(
            reads_.begin(),
            reads_.end(),
            [size](const auto& read) { return read.second == size; });
    }

    bool HasReadLargerThan(std::size_t size) const {
        return std::any_of(
            reads_.begin(),
            reads_.end(),
            [size](const auto& read) { return read.second > size; });
    }

protected:
    std::map<std::uint64_t, std::byte> bytes_;
    std::vector<std::pair<std::uint64_t, std::size_t>> reads_;
};

class FlippingMemory final : public SparseMemory {
public:
    explicit FlippingMemory(std::uint64_t watchedAddress)
        : watchedAddress_(watchedAddress) {}

    bool Read(std::uint64_t address,
              void* output,
              std::size_t size) override {
        const bool result = SparseMemory::Read(address, output, size);
        if (address == watchedAddress_ && size == sizeof(std::uint64_t) &&
            ++matchingReads_ == 2) {
            std::memset(output, 0, size);
            for (std::size_t index = 0; index < size; ++index) {
                bytes_[address + index] = std::byte{0};
            }
        }
        return result;
    }

private:
    std::uint64_t watchedAddress_ = 0;
    unsigned matchingReads_ = 0;
};

constexpr std::uint32_t EncodeLdrLiteral(std::int32_t wordDisplacement,
                                         unsigned targetRegister) {
    return UINT32_C(0x58000000) |
        ((static_cast<std::uint32_t>(wordDisplacement) & 0x7FFFFU) << 5U) |
        (targetRegister & 0x1FU);
}

constexpr std::uint32_t EncodeAdrp(std::int32_t pageDisplacement,
                                   unsigned targetRegister) {
    const std::uint32_t immediate =
        static_cast<std::uint32_t>(pageDisplacement) & 0x1FFFFFU;
    return UINT32_C(0x90000000) | ((immediate & 3U) << 29U) |
        (((immediate >> 2U) & 0x7FFFFU) << 5U) |
        (targetRegister & 0x1FU);
}

constexpr std::uint32_t EncodeLdrUnsigned64(unsigned byteOffset,
                                             unsigned targetRegister,
                                             unsigned baseRegister) {
    return UINT32_C(0xF9400000) |
        (((byteOffset / 8U) & 0xFFFU) << 10U) |
        ((baseRegister & 0x1FU) << 5U) |
        (targetRegister & 0x1FU);
}

constexpr std::uint32_t EncodeBr(unsigned targetRegister) {
    return UINT32_C(0xD61F0000) | ((targetRegister & 0x1FU) << 5U);
}

constexpr game::CoordinateExecutionLayout SyntheticLayout() {
    game::CoordinateExecutionLayout layout{};
    layout.discovery.rootOffset = UINT64_C(0x234000);
    layout.discovery.pointerOffset = UINT64_C(0x18);
    layout.discovery.entryOffset = UINT64_C(0xB8);
    layout.discovery.returnStubMagic =
        (static_cast<std::uint64_t>(EncodeBr(17)) << 32U) |
        EncodeLdrLiteral(3, 17);
    layout.result = {UINT64_C(0x248), UINT64_C(0x178)};
    layout.hooks = {
        0x410, 0x434, 0x458, 0x47C, 0x4A0, 0x4C4, 0x4E8,
        0x50C, 0x530, 0x554, 0x578, 0x59C, 0x5C0, 0x5E4,
        0x608, 0x62C, 0x650, 0x674, 0x698, 0x6BC, 0x6E0,
        0x704, 0x728, 0x74C, 0x770, 0x794,
    };
    layout.fields = {
        0x818, 0x83C, 0x860, 0x884, 0x8A8,
        0x8CC, 0x8F0, 0x914, 0x938, 0x95C,
        0x980, 0x9A4, 0x9C8, 0x9EC, 0xA10,
    };
    return layout;
}

template <typename Memory>
void InstallVeneer(Memory& memory,
                   std::uint64_t ldrPc,
                   std::uint64_t rawThunk) {
    constexpr auto kLayout = SyntheticLayout();
    memory.Write(ldrPc - 4, UINT32_C(0xD503201F));
    memory.Write(ldrPc, kLayout.discovery.returnStubMagic);
    memory.Write(ldrPc + 12, rawThunk);
}

template <typename Memory>
void InstallLiteralThunk(Memory& memory,
                         std::uint64_t thunk,
                         std::uint64_t rawQ0,
                         std::uint64_t absoluteEntry) {
    memory.Write(thunk, EncodeLdrLiteral(4, 0));
    memory.Write(thunk + 4, EncodeLdrLiteral(5, 1));
    memory.Write(thunk + 8, EncodeBr(2));
    memory.Write(thunk + 16, rawQ0);
    memory.Write(thunk + 24, absoluteEntry);
}

template <typename Memory>
void InstallCandidate(Memory& memory,
                      std::uint64_t ldrPc,
                      std::uint64_t thunk,
                      std::uint64_t q0,
                      std::uint64_t absoluteEntry) {
    InstallVeneer(memory, ldrPc, thunk);
    InstallLiteralThunk(memory, thunk, q0, absoluteEntry);
}

void InstallNegativeImmediateThunk(SparseMemory& memory,
                                   std::uint64_t thunk,
                                   std::uint64_t rawQ0,
                                   std::uint64_t absoluteEntry) {
    memory.Write(thunk + 0x10, rawQ0);
    memory.Write(thunk + 0x20, EncodeLdrLiteral(-4, 0));

    constexpr unsigned kTargetRegister = 5;
    memory.Write(thunk + 0x40, EncodeAdrp(-1, kTargetRegister));
    memory.Write(
        thunk + 0x44,
        EncodeLdrUnsigned64(
            0x100, kTargetRegister, kTargetRegister));
    memory.Write(thunk + 0x48, EncodeBr(kTargetRegister));
    memory.Write(thunk - 0xF00, absoluteEntry);
}

game::CoordinateExecutionModuleSnapshot Snapshot(
    std::uint64_t base,
    std::size_t size) {
    static const std::byte code{};
    return game::CoordinateExecutionModuleSnapshot{base, &code, size};
}

constexpr std::uint64_t kCodeBase = UINT64_C(0x6800000000);
constexpr std::size_t kCodeSize = 0x200000;

void TestCodeRangeSelection() {
    constexpr std::uint64_t kFirstBase = UINT64_C(0x6800000000);
    constexpr std::uint64_t kSecondBase = UINT64_C(0x6900000000);
    constexpr std::uint64_t kThirdBase = UINT64_C(0x6A00000000);
    const std::vector<game::CoordinateExecutionCodeRange> ranges{
        {kFirstBase, kFirstBase + 0x40000},
        {kFirstBase, kFirstBase + 0x90000},
        {kSecondBase, kSecondBase + 0x100000},
        {kThirdBase, kThirdBase + 0x200000},
    };
    game::CoordinateExecutionCodeRange selected{};

    REQUIRE(game::SelectCoordinateExecutionCodeRange(
        ranges,
        kSecondBase + 0x100,
        kThirdBase + 0x200,
        &selected));
    REQUIRE(selected.begin == kThirdBase);

    REQUIRE(game::SelectCoordinateExecutionCodeRange(
        ranges,
        kSecondBase + 0x100,
        UINT64_C(0x180000),
        &selected));
    REQUIRE(selected.begin == kSecondBase);

    REQUIRE(game::SelectCoordinateExecutionCodeRange(
        ranges, 0, UINT64_C(0x80000), &selected));
    REQUIRE(selected.begin == kFirstBase);
    REQUIRE(game::IsCoordinateExecutionCodeRangeCompatible(
        selected, 0, UINT64_C(0x80000)));
    std::uint64_t address = 0;
    REQUIRE(game::ResolveCoordinateExecutionCodeAddress(
        selected, UINT64_C(0x80000), &address));
    REQUIRE(address == kFirstBase + UINT64_C(0x80000));
    REQUIRE(game::ResolveCoordinateExecutionCodeAddress(
        ranges.back(), kThirdBase + 0x200, &address));
    REQUIRE(address == kThirdBase + 0x200);

    REQUIRE(!game::SelectCoordinateExecutionCodeRange(
        ranges, 0, UINT64_C(0x80002), &selected));
    REQUIRE(!game::ResolveCoordinateExecutionCodeAddress(
        selected, UINT64_C(0x80002), &address));
    REQUIRE(!game::SelectCoordinateExecutionCodeRange(
        ranges, 0, 0, &selected));
}

void TestLayoutAndDiscovery() {
    constexpr game::CoordinateExecutionLayout kLayout = SyntheticLayout();
    static_assert(kLayout.IsValid());
    static_assert(!game::CoordinateExecutionLayout{}.IsValid());
    REQUIRE(kLayout == SyntheticLayout());

    SparseMemory memory;
    constexpr std::uint64_t kModuleBase = UINT64_C(0x7000000000);
    constexpr std::size_t kModuleSize = 0x10000000;
    constexpr std::uint64_t kRelativeEntry = UINT64_C(0x4000);
    constexpr std::uint64_t kRoot = UINT64_C(0x6000003000);
    constexpr std::uint64_t kTaggedRoot = UINT64_C(0xEF00006000003000);
    constexpr std::uint64_t kThunk = UINT64_C(0x6000008000);
    constexpr std::uint64_t kTaggedThunk =
        UINT64_C(0xCD00006000008000);
    constexpr std::uint64_t kTaggedQ0 = UINT64_C(0xAB00006000009000);
    const std::uint64_t anchor =
        kModuleBase + kLayout.discovery.rootOffset;
    memory.Write(anchor + kLayout.discovery.pointerOffset, kTaggedRoot);
    memory.Write(
        kRoot + kLayout.discovery.entryOffset,
        UINT64_C(0xAB00000000000000) +
            kCodeBase + kRelativeEntry);
    const std::uint64_t ldrPc = anchor + 0x104;
    InstallVeneer(memory, ldrPc, kTaggedThunk);
    InstallLiteralThunk(
        memory,
        kThunk,
        kTaggedQ0,
        kCodeBase + kRelativeEntry);

    const auto module = Snapshot(kModuleBase, kModuleSize);
    const auto code = Snapshot(kCodeBase, kCodeSize);

    game::CoordinateExecutionDiscoveryInput input{};
    REQUIRE(game::ResolveCoordinateExecutionDiscoveryInput(
        memory.Callback(), kModuleBase, kLayout, &input));
    REQUIRE(input.scanAnchor == anchor);
    REQUIRE(input.root == kRoot);
    REQUIRE(
        (input.rawEntry & game::kCoordinateExecutionPointerMask) ==
        kCodeBase + kRelativeEntry);

    game::CoordinateExecutionCandidate candidate{};
    REQUIRE(game::DiscoverFirstCoordinateExecutionCandidate(
        memory.Callback(),
        module,
        code,
        kModuleBase,
        kLayout,
        &candidate));
    REQUIRE(candidate.q0 == kTaggedQ0);
    REQUIRE(candidate.q1 == kRelativeEntry);
    REQUIRE(candidate.q2 == kThunk);
    REQUIRE(candidate.q3 == ldrPc - 4);

    memory.Write(
        kRoot + kLayout.discovery.entryOffset,
        kCodeBase + kRelativeEntry + 4);
    REQUIRE(!game::DiscoverFirstCoordinateExecutionCandidate(
        memory.Callback(),
        module,
        code,
        kModuleBase,
        kLayout,
        &candidate));

    memory.Write(
        kRoot + kLayout.discovery.entryOffset,
        kModuleBase + kRelativeEntry);
    REQUIRE(!game::DiscoverFirstCoordinateExecutionCandidate(
        memory.Callback(),
        module,
        code,
        kModuleBase,
        kLayout,
        &candidate));

    memory.Write(
        kRoot + kLayout.discovery.entryOffset,
        kCodeBase + kRelativeEntry);
    memory.Write(kThunk + 24, kModuleBase + kRelativeEntry);
    REQUIRE(!game::DiscoverFirstCoordinateExecutionCandidate(
        memory.Callback(),
        module,
        code,
        kModuleBase,
        kLayout,
        &candidate));
    memory.Write(kThunk + 24, kCodeBase + kRelativeEntry);

    memory.Write(kRoot + kLayout.discovery.entryOffset, kRelativeEntry);
    REQUIRE(game::DiscoverFirstCoordinateExecutionCandidate(
        memory.Callback(),
        module,
        code,
        kModuleBase,
        kLayout,
        &candidate));
    REQUIRE(candidate.q1 == kRelativeEntry);

    memory.Write(
        kRoot + kLayout.discovery.entryOffset,
        kRelativeEntry + 2);
    REQUIRE(!game::DiscoverFirstCoordinateExecutionCandidate(
        memory.Callback(),
        module,
        code,
        kModuleBase,
        kLayout,
        &candidate));
}

void TestNegativeAarch64Immediates() {
    SparseMemory memory;
    constexpr std::uint64_t kModuleBase = UINT64_C(0x7000000000);
    constexpr std::size_t kModuleSize = 0x200000;
    constexpr std::uint64_t kRelativeEntry = UINT64_C(0x1A000);
    constexpr std::uint64_t kThunk = UINT64_C(0x6000102000);
    constexpr std::uint64_t kTaggedThunk =
        UINT64_C(0xA500006000102000);
    constexpr std::uint64_t kTaggedQ0 = UINT64_C(0xB600006000108000);
    constexpr std::uint64_t kLdrPc = kModuleBase + 0x120;

    InstallVeneer(memory, kLdrPc, kTaggedThunk);
    InstallNegativeImmediateThunk(
        memory, kThunk, kTaggedQ0, kCodeBase + kRelativeEntry);

    game::CoordinateExecutionCandidate candidate{};
    REQUIRE(game::ScanFirstCoordinateExecutionCandidate(
        memory.Callback(),
        Snapshot(kModuleBase, kModuleSize),
        Snapshot(kCodeBase, kCodeSize),
        kLdrPc,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));
    REQUIRE((candidate == game::CoordinateExecutionCandidate{
        kTaggedQ0,
        kRelativeEntry,
        kThunk,
        kLdrPc - 4,
    }));
}

void TestPriorityOrderDedupAndChunkOverlap() {
    constexpr std::uint64_t kModuleBase = UINT64_C(0x7000000000);
    constexpr std::size_t kModuleSize = 0x300000;
    constexpr std::uint64_t kRelativeEntry = UINT64_C(0x4000);
    const auto module = Snapshot(kModuleBase, kModuleSize);
    const auto code = Snapshot(kCodeBase, kCodeSize);

    SparseMemory orderMemory;
    const std::uint64_t exactLdr = kModuleBase + 0x180104;
    const std::uint64_t centerLdr = kModuleBase + 0x100104;
    const std::uint64_t lowerLdr = kModuleBase + 0x104;
    const std::uint64_t upperLdr = kModuleBase + 0x200104;
    InstallCandidate(
        orderMemory,
        exactLdr,
        UINT64_C(0x6000200000),
        UINT64_C(0x6000300000),
        kCodeBase + kRelativeEntry);
    InstallCandidate(
        orderMemory,
        centerLdr,
        UINT64_C(0x6000210000),
        UINT64_C(0x6000310000),
        kCodeBase + kRelativeEntry);
    InstallCandidate(
        orderMemory,
        lowerLdr,
        UINT64_C(0x6000220000),
        UINT64_C(0x6000320000),
        kCodeBase + kRelativeEntry);
    InstallCandidate(
        orderMemory,
        upperLdr,
        UINT64_C(0x6000230000),
        UINT64_C(0x6000330000),
        kCodeBase + kRelativeEntry);

    game::CoordinateExecutionCandidate first{};
    REQUIRE(game::ScanFirstCoordinateExecutionCandidate(
        orderMemory.Callback(),
        module,
        code,
        exactLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &first));
    REQUIRE(first.q0 == UINT64_C(0x6000300000));

    const game::CoordinateExecutionCandidateScanResult all =
        game::ScanCoordinateExecutionCandidates(
            orderMemory.Callback(),
            module,
            code,
            exactLdr,
            kRelativeEntry,
            SyntheticLayout());
    REQUIRE(!all.truncated);
    REQUIRE(all.candidates.size() == 4);
    REQUIRE(all.candidates[0].q0 == UINT64_C(0x6000300000));
    REQUIRE(all.candidates[1].q0 == UINT64_C(0x6000310000));
    REQUIRE(all.candidates[2].q0 == UINT64_C(0x6000320000));
    REQUIRE(all.candidates[3].q0 == UINT64_C(0x6000330000));

    SparseMemory overlapMemory;
    const std::uint64_t boundaryLdr =
        kModuleBase + UINT64_C(0x100000) - 4;
    InstallCandidate(
        overlapMemory,
        boundaryLdr,
        UINT64_C(0x6000240000),
        UINT64_C(0x6000340000),
        kCodeBase + kRelativeEntry);
    REQUIRE(game::ScanFirstCoordinateExecutionCandidate(
        overlapMemory.Callback(),
        module,
        code,
        0,
        kRelativeEntry,
        SyntheticLayout(),
        &first));
    REQUIRE(first.q3 == boundaryLdr - 4);
    REQUIRE(overlapMemory.HasReadSize(4096));
    REQUIRE(!overlapMemory.HasReadLargerThan(4096));
}

void TestBoundsAndStableMagic() {
    constexpr std::uint64_t kModuleBase = UINT64_C(0x7000000000);
    constexpr std::size_t kModuleSize = 0x2000;
    constexpr std::uint64_t kRelativeEntry = UINT64_C(0x100);
    constexpr std::uint64_t kThunk = UINT64_C(0x6000400000);
    constexpr std::uint64_t kQ0 = UINT64_C(0x6000410000);
    const auto module = Snapshot(kModuleBase, kModuleSize);
    const auto code = Snapshot(kCodeBase, kCodeSize);

    SparseMemory lowerMemory;
    const std::uint64_t lowerLdr = kModuleBase + 4;
    InstallCandidate(
        lowerMemory,
        lowerLdr,
        kThunk,
        kQ0,
        kCodeBase + kRelativeEntry);
    game::CoordinateExecutionCandidate candidate{};
    REQUIRE(game::ScanFirstCoordinateExecutionCandidate(
        lowerMemory.Callback(),
        module,
        code,
        lowerLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));
    REQUIRE(candidate.q3 == kModuleBase);

    SparseMemory upperMemory;
    const std::uint64_t upperLdr = kModuleBase + kModuleSize - 8;
    InstallCandidate(
        upperMemory,
        upperLdr,
        kThunk,
        kQ0,
        kCodeBase + kRelativeEntry);
    REQUIRE(game::ScanFirstCoordinateExecutionCandidate(
        upperMemory.Callback(),
        module,
        code,
        upperLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));
    REQUIRE(candidate.q3 == kModuleBase + kModuleSize - 12);

    SparseMemory outsideMemory;
    const std::uint64_t invalidLdr = kModuleBase + kModuleSize - 4;
    InstallCandidate(
        outsideMemory,
        invalidLdr,
        kThunk,
        kQ0,
        kCodeBase + kRelativeEntry);
    REQUIRE(!game::ScanFirstCoordinateExecutionCandidate(
        outsideMemory.Callback(),
        module,
        code,
        invalidLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));

    SparseMemory moduleEndMemory;
    const std::uint64_t moduleEndThunk = kModuleBase + kModuleSize;
    const std::uint64_t moduleEndLdr = kModuleBase + 0x800;
    InstallCandidate(
        moduleEndMemory,
        moduleEndLdr,
        moduleEndThunk,
        UINT64_C(0x6000420000),
        kCodeBase + kRelativeEntry);
    REQUIRE(game::ScanFirstCoordinateExecutionCandidate(
        moduleEndMemory.Callback(),
        module,
        code,
        moduleEndLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));
    REQUIRE(candidate.q2 == moduleEndThunk);

    SparseMemory internalMemory;
    const std::uint64_t internalThunk = kCodeBase + 0x1000;
    const std::uint64_t internalLdr = kModuleBase + 0xA00;
    InstallCandidate(
        internalMemory,
        internalLdr,
        internalThunk,
        kQ0,
        kCodeBase + kRelativeEntry);
    REQUIRE(!game::ScanFirstCoordinateExecutionCandidate(
        internalMemory.Callback(),
        module,
        code,
        internalLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));

    const std::uint64_t racingLdr = kModuleBase + 0x400;
    FlippingMemory racingMemory(racingLdr);
    InstallCandidate(
        racingMemory,
        racingLdr,
        kThunk,
        kQ0,
        kCodeBase + kRelativeEntry);
    REQUIRE(!game::ScanFirstCoordinateExecutionCandidate(
        racingMemory.Callback(),
        module,
        code,
        racingLdr,
        kRelativeEntry,
        SyntheticLayout(),
        &candidate));
}

void TestCandidateLimit() {
    SparseMemory memory;
    constexpr std::uint64_t kModuleBase = UINT64_C(0x7000000000);
    constexpr std::size_t kModuleSize = 0x200000;
    constexpr std::uint64_t kRelativeEntry = UINT64_C(0x4000);
    constexpr std::uint64_t kFirstStub = kModuleBase + 0x100;
    for (std::size_t index = 0;
         index <= game::kCoordinateExecutionCandidateLimit;
         ++index) {
        const std::uint64_t ldrPc =
            kFirstStub + index * UINT64_C(0x1000) + 4;
        const std::uint64_t thunk =
            UINT64_C(0x6001000000) + index * UINT64_C(0x1000);
        const std::uint64_t q0 =
            UINT64_C(0x6002000000) + index * UINT64_C(0x1000);
        InstallCandidate(
            memory,
            ldrPc,
            thunk,
            q0,
            kCodeBase + kRelativeEntry);
    }

    const game::CoordinateExecutionCandidateScanResult result =
        game::ScanCoordinateExecutionCandidates(
            memory.Callback(),
            Snapshot(kModuleBase, kModuleSize),
            Snapshot(kCodeBase, kCodeSize),
            kFirstStub + 4,
            kRelativeEntry,
            SyntheticLayout());
    REQUIRE(result.truncated);
    REQUIRE(
        result.candidates.size() ==
        game::kCoordinateExecutionCandidateLimit);
    REQUIRE(result.candidates.front().q3 == kFirstStub);
    REQUIRE(
        result.candidates.back().q3 ==
        kFirstStub +
            (game::kCoordinateExecutionCandidateLimit - 1) *
                UINT64_C(0x1000));
}

}  // namespace

int main() {
    TestCodeRangeSelection();
    TestLayoutAndDiscovery();
    TestNegativeAarch64Immediates();
    TestPriorityOrderDedupAndChunkOverlap();
    TestBoundsAndStableMagic();
    TestCandidateLimit();
    std::cout << "coordinate execution candidate tests passed\n";
    return 0;
}
