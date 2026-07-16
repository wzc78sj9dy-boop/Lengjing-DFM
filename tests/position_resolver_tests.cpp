#include "test_support.h"

#include "game/native/CharacterPositionResolver.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
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
    using Coordinate = CharacterPositionResolver::Coordinate;

    REQUIRE(CharacterPositionResolver::IsPrimaryCharacter("NC_BP_DFMCharacter_C"));
    REQUIRE(CharacterPositionResolver::IsDirectAiCharacter(
        "NC_BP_DFMAICharacter_Soldier_C"));
    REQUIRE(!CharacterPositionResolver::IsPrimaryCharacter("Pickup_C"));

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

    REQUIRE(resolver.Read(
        actor, "NC_BP_DFMCharacter_C", PositionReadMode::Direct,
        false, coordinate, read));
    REQUIRE(coordinate == Coordinate({40.0f, 50.0f, 60.0f}));

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
