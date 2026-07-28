#include "game/native/TersafePatchWorker.h"

#include "test_support.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

using lengjing::game::native::ResolveTersafePatchTargets;
namespace layout = lengjing::game::native::tersafe_patch_layout;

class SparseMemory final {
public:
    template <typename Value>
    void Store(std::uintptr_t address, const Value& value) {
        const auto* bytes =
            reinterpret_cast<const std::uint8_t*>(&value);
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            bytes_[address + index] = bytes[index];
        }
    }

    bool Read(std::uintptr_t address,
              void* destination,
              std::size_t size) const {
        if (destination == nullptr || size == 0) return false;
        auto* output = static_cast<std::uint8_t*>(destination);
        for (std::size_t index = 0; index < size; ++index) {
            const auto found = bytes_.find(address + index);
            if (found == bytes_.end()) return false;
            output[index] = found->second;
        }
        return true;
    }

private:
    std::unordered_map<std::uintptr_t, std::uint8_t> bytes_;
};

void TestTargetResolution() {
    constexpr std::uintptr_t moduleBase = 0x10000000;
    constexpr std::uintptr_t root = 0x20000000;
    constexpr std::uintptr_t rejectedCandidate = 0x30000000;
    constexpr std::uintptr_t selectedCandidate = 0x40000000;
    constexpr std::uintptr_t group = 0x50000000;
    constexpr std::array<std::uintptr_t, 3> expected{
        0x60000001,
        0x60000002,
        0x60000003,
    };

    SparseMemory memory;
    memory.Store(
        moduleBase + layout::kRootOffset,
        std::uint64_t{0xAB00000000000000ULL | root});
    memory.Store(
        root + layout::kCandidateOffsets[0],
        std::uint64_t{rejectedCandidate});
    memory.Store(
        rejectedCandidate,
        std::uint32_t{layout::kCandidateTag - 1});
    memory.Store(
        root + layout::kCandidateOffsets[1],
        std::uint64_t{0});
    memory.Store(
        root + layout::kCandidateOffsets[2],
        std::uint64_t{0xBC00000000000000ULL | selectedCandidate});
    memory.Store(
        selectedCandidate,
        std::uint32_t{layout::kCandidateTag});
    memory.Store(
        selectedCandidate + layout::kGroupOffset,
        std::uint64_t{0xCD00000000000000ULL | group});
    for (std::size_t index = 0; index < expected.size(); ++index) {
        memory.Store(
            group + layout::kTargetOffsets[index],
            std::uint64_t{
                0xEF00000000000000ULL | expected[index]});
    }

    const auto read = [&memory](
                          std::uintptr_t address,
                          void* destination,
                          std::size_t size) {
        return memory.Read(address, destination, size);
    };
    std::array<std::uintptr_t, 3> actual{};
    REQUIRE(ResolveTersafePatchTargets(moduleBase, read, actual));
    REQUIRE(actual == expected);
}

void TestRejectsInvalidGroup() {
    constexpr std::uintptr_t moduleBase = 0x11000000;
    constexpr std::uintptr_t root = 0x21000000;
    constexpr std::uintptr_t candidate = 0x31000000;

    SparseMemory memory;
    memory.Store(
        moduleBase + layout::kRootOffset,
        std::uint64_t{root});
    memory.Store(
        root + layout::kCandidateOffsets[0],
        std::uint64_t{candidate});
    memory.Store(candidate, std::uint32_t{layout::kCandidateTag});
    memory.Store(
        candidate + layout::kGroupOffset,
        std::uint64_t{0});

    const auto read = [&memory](
                          std::uintptr_t address,
                          void* destination,
                          std::size_t size) {
        return memory.Read(address, destination, size);
    };
    std::array<std::uintptr_t, 3> targets{
        1,
        2,
        3,
    };
    REQUIRE(!ResolveTersafePatchTargets(moduleBase, read, targets));
    const std::array<std::uintptr_t, 3> empty{};
    REQUIRE(targets == empty);
}

}  // namespace

void RunTersafePatchWorkerTests() {
    TestTargetResolution();
    TestRejectsInvalidGroup();
}
