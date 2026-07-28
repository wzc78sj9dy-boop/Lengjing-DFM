#include "test_support.h"

#include "game/native/CharacterPositionResolver.h"
#include "game/native/PositionReadModePolicy.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace {

class PositionMemory final {
public:
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        std::vector<std::uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        values_[address] = std::move(bytes);
    }

    void Erase(std::uintptr_t address) { values_.erase(address); }

    bool Read(std::uintptr_t address, void* destination, std::size_t size) const {
        const auto found = values_.find(address);
        if (found == values_.end() || found->second.size() != size) return false;
        std::memcpy(destination, found->second.data(), size);
        return true;
    }

private:
    std::unordered_map<std::uintptr_t, std::vector<std::uint8_t>> values_;
};

}  // namespace

void RunPositionResolverTests() {
    using lengjing::game::native::CharacterPositionResolver;
    using lengjing::game::native::CharacterPositionLayout;
    using lengjing::game::native::CharacterPositionSource;
    using lengjing::game::native::PositionReadMode;
    using lengjing::game::native::ShouldAlignBoneFrameToCharacterPosition;
    using Coordinate = CharacterPositionResolver::Coordinate;

    REQUIRE(ShouldAlignBoneFrameToCharacterPosition(
        CharacterPositionSource::Decoded));
    REQUIRE(!ShouldAlignBoneFrameToCharacterPosition(
        CharacterPositionSource::Standard));
    REQUIRE(!ShouldAlignBoneFrameToCharacterPosition(
        CharacterPositionSource::None));

    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter("NC_BP_DFMCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter(
        "BP_RangeTargetCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter(
        "BP_RangeTargeCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter(
        "BP_DFMRangeTargetCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsAiCharacter(
        "NC_BP_DFMAICharacter_Soldier_C"));
    REQUIRE(CharacterPositionResolver::IsTargetCharacter(
        "NC_BP_DFMCharacter_AI_DT_C"));
    REQUIRE(CharacterPositionResolver::IsTargetCharacter(
        "NC_BP_DFMCharacter_TutorialPlayerAi_C"));
    REQUIRE(CharacterPositionResolver::IsTargetCharacter(
        "BP_RangeTargeCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsTargetCharacter(
        "NC_BP_DFMCharacter_Boss05_C"));
    REQUIRE(!CharacterPositionResolver::IsPrimaryCharacter("Pickup_C"));
    REQUIRE(!CharacterPositionResolver::IsTargetCharacter("Pickup_C"));

    constexpr std::uintptr_t actor = 0x100000;
    constexpr std::uintptr_t component = 0x200000;
    constexpr std::uintptr_t actorComponentOffset = 0x38;
    constexpr std::uintptr_t preferredPositionOffset = 0x58;
    constexpr CharacterPositionLayout layout{
        actorComponentOffset, preferredPositionOffset};
    PositionMemory memory;
    memory.Put(actor + actorComponentOffset, component);
    memory.Put(
        component + preferredPositionOffset,
        Coordinate{10.0f, 20.0f, 30.0f});
    memory.Put(component + 0x220, Coordinate{40.0f, 50.0f, 60.0f});
    auto read = [&memory](std::uintptr_t address, void* output, std::size_t size) {
        return memory.Read(address, output, size);
    };

    CharacterPositionResolver resolver;
    Coordinate coordinate{};
    bool unconfiguredReadAttempted = false;
    auto unconfiguredRead = [&unconfiguredReadAttempted](
                                std::uintptr_t,
                                void*,
                                std::size_t) {
        unconfiguredReadAttempted = true;
        return false;
    };
    REQUIRE(!resolver.Read(
        actor, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        true, coordinate, unconfiguredRead));
    REQUIRE(coordinate == Coordinate{});
    REQUIRE(!unconfiguredReadAttempted);
    REQUIRE(!resolver.Configure({}));
    REQUIRE(resolver.Configure(layout));
    REQUIRE(resolver.Read(
        actor, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        true, coordinate, read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));

    for (const char* targetClass : {
             "NC_BP_DFMCharacter_TutorialPlayerAi_C",
             "NC_BP_DFMCharacter_AI_DT_C",
             "NC_BP_DFMAICharacter_Soldier_C",
             "NC_BP_DFMCharacter_Boss05_C",
             "BP_RangeTargetCharacter_C",
             "BP_RangeTargeCharacter_C"}) {
        REQUIRE(resolver.Read(
            actor, targetClass, PositionReadMode::Standard,
            false, coordinate, read));
        REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));
    }

    REQUIRE(resolver.Read(
        actor, "", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));

    constexpr std::uintptr_t rangeTargetRoot = 0x280000;
    memory.Put(
        rangeTargetRoot + 0x220,
        Coordinate{70.0f, 80.0f, 90.0f});
    REQUIRE(resolver.ReadWithRoot(
        actor,
        rangeTargetRoot,
        "BP_RangeTargetCharacter_C",
        PositionReadMode::Standard,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({70.0f, 80.0f, 90.0f}));

    memory.Put(rangeTargetRoot + 0x220, Coordinate{});
    REQUIRE(resolver.ReadWithRoot(
        actor,
        rangeTargetRoot,
        "BP_RangeTargetCharacter_C",
        PositionReadMode::Standard,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));
    memory.Erase(rangeTargetRoot + 0x220);
    REQUIRE(resolver.ReadWithRoot(
        actor,
        rangeTargetRoot,
        "BP_RangeTargeCharacter_C",
        PositionReadMode::Standard,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));

    REQUIRE(resolver.ReadLocalWithRoot(
        actor, 0, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));

    memory.Put(component + preferredPositionOffset, Coordinate{});
    REQUIRE(resolver.ReadLocalWithRoot(
        actor, 0, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({40.0f, 50.0f, 60.0f}));
    REQUIRE(resolver.Read(
        actor, "", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({40.0f, 50.0f, 60.0f}));
    memory.Put(
        component + preferredPositionOffset,
        Coordinate{10.0f, 20.0f, 30.0f});

    memory.Put(component + 0x220, Coordinate{
        std::numeric_limits<float>::quiet_NaN(), 50.0f, 60.0f});
    REQUIRE(resolver.Read(
        actor, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));
    memory.Put(component + 0x220, Coordinate{40.0f, 50.0f, 60.0f});

    memory.Erase(component + preferredPositionOffset);
    memory.Erase(component + 0x220);
    REQUIRE(resolver.Read(
        actor, "Pickup_C", PositionReadMode::Standard,
        true, coordinate, read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));

    resolver.Clear();
    REQUIRE(!resolver.Read(
        actor, "Pickup_C", PositionReadMode::Standard,
        true, coordinate, read));
}
