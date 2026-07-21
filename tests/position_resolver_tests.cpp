#include "test_support.h"

#include "game/native/CharacterPositionResolver.h"
#include "game/native/PositionReadModePolicy.h"

#include <cmath>
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
    using lengjing::game::native::PositionReadMode;
    using lengjing::game::native::ResolveDecodedCharacterZ;
    using lengjing::game::native::ResolvePositionReadMode;
    using Coordinate = CharacterPositionResolver::Coordinate;

    REQUIRE(ResolvePositionReadMode(false) == PositionReadMode::Standard);
    REQUIRE(ResolvePositionReadMode(true) == PositionReadMode::Direct);
    REQUIRE(ResolveDecodedCharacterZ(1000.0f) == 910.0f);
    REQUIRE(ResolveDecodedCharacterZ(90.0f) == 0.0f);
    REQUIRE(ResolveDecodedCharacterZ(-10.0f) == -100.0f);

    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter("NC_BP_DFMCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter(
        "BP_RangeTargetCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter(
        "BP_RangeTargeCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter(
        "BP_DFMRangeTargetCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsDirectAiCharacter(
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
    PositionMemory memory;
    memory.Put(actor + 0x180, component);
    memory.Put(component + 0x168, Coordinate{10.0f, 20.0f, 30.0f});
    memory.Put(component + 0x220, Coordinate{40.0f, 50.0f, 60.0f});
    auto read = [&memory](std::uintptr_t address, void* output, std::size_t size) {
        return memory.Read(address, output, size);
    };

    CharacterPositionResolver resolver;
    Coordinate coordinate{};
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

    memory.Put(component + 0x168, Coordinate{});
    REQUIRE(resolver.ReadLocalWithRoot(
        actor, 0, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({40.0f, 50.0f, 60.0f}));
    REQUIRE(resolver.Read(
        actor, "", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({40.0f, 50.0f, 60.0f}));
    memory.Put(component + 0x168, Coordinate{10.0f, 20.0f, 30.0f});

    memory.Put(component + 0x220, Coordinate{
        std::numeric_limits<float>::quiet_NaN(), 50.0f, 60.0f});
    REQUIRE(resolver.Read(
        actor, "NC_BP_DFMCharacter_C", PositionReadMode::Standard,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));
    memory.Put(component + 0x220, Coordinate{40.0f, 50.0f, 60.0f});

    REQUIRE(!resolver.Read(
        actor, "NC_BP_DFMCharacter_C", PositionReadMode::Direct,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate{});

    REQUIRE(resolver.ReadWithRoot(
        actor,
        component,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({10.0f, 20.0f, 30.0f}));

    constexpr std::uintptr_t decodedRoot = 0x300000;
    memory.Put(decodedRoot + 0x168, Coordinate{});
    memory.Put(decodedRoot + 0x148, Coordinate{});
    memory.Put(decodedRoot + 0x220, Coordinate{0.0f, 0.0f, -90.0f});
    memory.Put(decodedRoot + 0x240, Coordinate{70.0f, 80.0f, 90.0f});
    REQUIRE(resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({0.0f, 0.0f, -90.0f}));

    memory.Put(decodedRoot + 0x220, Coordinate{});
    REQUIRE(resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({70.0f, 80.0f, 90.0f}));

    memory.Put(decodedRoot + 0x240, Coordinate{});
    REQUIRE(!resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate{});

    memory.Erase(decodedRoot + 0x240);
    memory.Erase(decodedRoot + 0x220);
    REQUIRE(!resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate{});

    memory.Put(decodedRoot + 0x168, Coordinate{1.0f, 2.0f, 3.0f});
    memory.Put(decodedRoot + 0x148, Coordinate{4.0f, 5.0f, 6.0f});
    memory.Put(decodedRoot + 0x220, Coordinate{7.0f, 8.0f, 9.0f});
    memory.Put(decodedRoot + 0x240, Coordinate{10.0f, 11.0f, 12.0f});
    REQUIRE(resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({1.0f, 2.0f, 3.0f}));

    REQUIRE(resolver.ReadLocalWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({1.0f, 2.0f, 3.0f}));

    memory.Put(decodedRoot + 0x168, Coordinate{});
    REQUIRE(resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate({4.0f, 5.0f, 6.0f}));

    REQUIRE(!resolver.ReadLocalWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate{});

    memory.Put(decodedRoot + 0x148, Coordinate{});
    memory.Erase(decodedRoot + 0x240);
    memory.Put(decodedRoot + 0x220, Coordinate{
        std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f});
    REQUIRE(!resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate{});

    memory.Put(decodedRoot + 0x220, Coordinate{
        std::numeric_limits<float>::infinity(), 0.0f, 0.0f});
    REQUIRE(!resolver.ReadWithRoot(
        actor,
        decodedRoot,
        "NC_BP_DFMCharacter_C",
        PositionReadMode::Direct,
        false,
        coordinate,
        read));
    REQUIRE(coordinate == Coordinate{});

    REQUIRE(!resolver.Read(
        actor, "Pickup_C", PositionReadMode::Direct,
        true, coordinate, read));
    REQUIRE(coordinate == Coordinate{});

    memory.Erase(component + 0x168);
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
