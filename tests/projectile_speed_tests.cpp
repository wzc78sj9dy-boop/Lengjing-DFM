#include "test_support.h"

#include "game/native/ProjectileSpeedReader.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class FakeMemory final {
public:
    template <typename T>
    void Put(std::uintptr_t address, const T& value) {
        std::vector<std::uint8_t> bytes(sizeof(T));
        std::memcpy(bytes.data(), &value, sizeof(T));
        values_[address] = std::move(bytes);
    }

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

void RunProjectileSpeedTests() {
    using lengjing::game::native::ProjectileSpeedReader;

    constexpr std::uintptr_t weapon = 0x100000;
    constexpr std::uintptr_t identity = 0x200000;
    constexpr std::uintptr_t entries = 0x300000;
    constexpr std::uintptr_t field = 0x400000;
    constexpr std::uint32_t fieldName = 77;

    FakeMemory memory;
    memory.Put(weapon + 0x840, identity);
    memory.Put(weapon + 0x838, std::uint64_t{830000001});
    memory.Put(weapon + 0x220, entries);
    memory.Put(weapon + 0x228, std::uint32_t{1});
    memory.Put(entries, field);
    memory.Put(field + 0x1C, fieldName);

    std::uintptr_t node = weapon;
    constexpr std::uintptr_t offsets[]{0x438, 0x538, 0x38, 0x170, 0xC8};
    for (std::size_t index = 0; index < std::size(offsets); ++index) {
        const std::uintptr_t next = 0x500000 + index * 0x1000;
        memory.Put(node + offsets[index], next);
        node = next;
    }
    memory.Put(node + 0x6C, 91234.0f);

    ProjectileSpeedReader reader;
    auto read = [&memory](std::uintptr_t address, void* output, std::size_t size) {
        return memory.Read(address, output, size);
    };
    auto resolve = [](std::uint32_t index) {
        return index == fieldName
            ? std::string(ProjectileSpeedReader::kRequiredField)
            : std::string("OtherField");
    };

    const std::optional<float> speed = reader.Read(weapon, read, resolve);
    REQUIRE(speed.has_value());
    REQUIRE(*speed == 91234.0f);
    REQUIRE(reader.CachedIdentity() == identity);
    REQUIRE(reader.CurrentWeaponId() == 830000001ULL);

    memory.Put(node + 0x6C, -1.0f);
    REQUIRE(!reader.Read(weapon, read, resolve).has_value());

    memory.Put(weapon + 0x840, std::uintptr_t{0});
    REQUIRE(!reader.Read(weapon, read, resolve).has_value());
    REQUIRE(reader.CachedIdentity() == 0);
}
