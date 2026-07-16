#include "test_support.h"

#include "game/native/IndexedCoordinateReader.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

class CoordinateMemory final {
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

void RunCoordinateReaderTests() {
    using Reader = lengjing::game::native::IndexedCoordinateReader;

    constexpr std::uintptr_t subject = 0x100000;
    constexpr std::uintptr_t links[]{
        0x200000, 0x300000, 0x400000, 0x500000, 0x600000};
    constexpr std::uintptr_t offsets[]{0x3D0, 0xF8, 0x8, 0x4C8, 0x118};
    constexpr std::uintptr_t coordinateAddress = 0x700000;

    CoordinateMemory memory;
    std::uintptr_t current = subject;
    for (std::size_t index = 0; index < std::size(offsets); ++index) {
        memory.Put(current + offsets[index], links[index]);
        current = links[index];
    }
    const std::uintptr_t matchingRecord = current + 3 * 0x70;
    memory.Put(matchingRecord, subject);
    memory.Put(matchingRecord + 0x30, coordinateAddress);
    memory.Put(coordinateAddress, Reader::Coordinate{125.0f, -42.0f, 900.0f});

    auto read = [&memory](std::uintptr_t address, void* output, std::size_t size) {
        return memory.Read(address, output, size);
    };
    Reader::Coordinate coordinate{};
    REQUIRE(Reader::Read(subject, coordinate, read));
    REQUIRE(coordinate[0] == 125.0f);
    REQUIRE(coordinate[1] == -42.0f);
    REQUIRE(coordinate[2] == 900.0f);

    Reader::Coordinate missing{1.0f, 1.0f, 1.0f};
    REQUIRE(!Reader::Read(subject + 8, missing, read));
    REQUIRE(missing == Reader::Coordinate{});
}
