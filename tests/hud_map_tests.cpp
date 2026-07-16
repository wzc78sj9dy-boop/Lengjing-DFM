#include "test_support.h"

#include "game/native/HudMapProjection.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

class HudMemory final {
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

struct HudFieldEntry {
    std::uint32_t nameIndex = 0;
    std::uint32_t padding = 0;
    std::uintptr_t property = 0;
    std::uintptr_t fieldClass = 0;
};

struct BigLayoutBlock {
    float originX = 0.0f;
    float originY = 0.0f;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    std::int32_t worldOffsetX = 0;
    std::int32_t worldOffsetY = 0;
};

struct MiniLayoutBlock {
    float positionX = 0.0f;
    float positionY = 0.0f;
    float size = 0.0f;
};

bool Near(float left, float right) {
    return std::fabs(left - right) < 0.001f;
}

}  // namespace

void RunHudMapTests() {
    using namespace lengjing::game::native;

    constexpr std::uintptr_t controller = 0x100000;
    constexpr std::uintptr_t hud = 0x200000;
    constexpr std::uintptr_t holder = 0x300000;
    constexpr std::uintptr_t structure = 0x400000;
    constexpr std::uintptr_t fields = 0x500000;
    constexpr std::uintptr_t bigProperty = 0x600000;
    constexpr std::uintptr_t miniProperty = 0x610000;
    constexpr std::uintptr_t bigWidget = 0x700000;
    constexpr std::uintptr_t miniWidget = 0x710000;

    HudMemory memory;
    memory.Put(controller + 0x400, hud);
    memory.Put(hud + 0x4A8, holder);
    memory.Put(holder + 0xE0, structure);
    memory.Put(structure + 0x28, fields);
    memory.Put(structure + 0x30, std::uint32_t{2});
    memory.Put(fields, HudFieldEntry{11, 0, bigProperty, 0});
    memory.Put(fields + sizeof(HudFieldEntry),
               HudFieldEntry{12, 0, miniProperty, 0});
    memory.Put(bigProperty + 0x38, bigWidget);
    memory.Put(miniProperty + 0x38, miniWidget);
    memory.Put(bigWidget + 0x9B4, std::uint8_t{1});
    memory.Put(bigWidget + 0x898,
               BigLayoutBlock{100.0f, 200.0f, 400.0f, 500.0f, 10, -20});
    memory.Put(miniWidget + 0x598, 2.0f);
    memory.Put(miniWidget + 0xB74, MiniLayoutBlock{30.0f, 40.0f, 200.0f});

    auto read = [&memory](std::uintptr_t address, void* output, std::size_t size) {
        return memory.Read(address, output, size);
    };
    auto resolve = [](std::uint32_t index) {
        if (index == 11) return std::string("MobileHUD_BigMap");
        if (index == 12) return std::string("MobileHUD_MiniMap");
        return std::string{};
    };

    HudMapCache cache{};
    REQUIRE(HudMapReader::Refresh(controller, cache, read, resolve));
    REQUIRE(cache.controller == controller);
    REQUIRE(cache.bigMapWidget == bigWidget);
    REQUIRE(cache.miniMapWidget == miniWidget);
    REQUIRE(cache.bigMapVisible);
    REQUIRE(cache.bigScaleX == 400.0f);
    REQUIRE(cache.miniScale == 2.0f);

    const HudMapProjection big = ProjectBigMap(
        cache, 490.0f, 720.0f, 0.0f, 2000.0f, 1000.0f,
        5.0f, -5.0f, 6.0f);
    REQUIRE(big.visible);
    REQUIRE(Near(big.marker.x, 2005.0f));
    REQUIRE(Near(big.marker.y, 995.0f));
    REQUIRE(Near(big.directionEnd.x, 2029.0f));

    const HudMapProjection self = ProjectMiniMap(
        cache, 1000.0f, 2000.0f, 50.0f,
        1000.0f, 2000.0f, 50.0f,
        0.0f, false, 5.0f, -5.0f, 6.0f);
    REQUIRE(self.visible);
    REQUIRE(Near(self.marker.x, 131.75f));
    REQUIRE(Near(self.marker.y, 105.0f));

    const HudMapProjection normal = ProjectMiniMap(
        cache, 0.0f, 0.0f, 0.0f,
        10000.0f, 0.0f, 0.0f,
        0.0f, false, 0.0f, 0.0f, 6.0f);
    REQUIRE(normal.visible);
    REQUIRE(normal.marker.x > 126.75f);

    const HudMapProjection rotated = ProjectMiniMap(
        cache, 0.0f, 0.0f, 0.0f,
        10000.0f, 0.0f, 0.0f,
        0.0f, true, 0.0f, 0.0f, 6.0f);
    REQUIRE(rotated.visible);
    REQUIRE(Near(rotated.marker.x, 126.75f));
    REQUIRE(rotated.marker.y > 110.0f);

    auto failReflection = [&memory, controller](
                              std::uintptr_t address,
                              void* output,
                              std::size_t size) {
        if (address == controller + 0x400) return false;
        return memory.Read(address, output, size);
    };
    REQUIRE(HudMapReader::Refresh(
        controller, cache, failReflection, resolve));
    REQUIRE(cache.controller == controller);
    REQUIRE(cache.bigMapWidget == bigWidget);
    REQUIRE(cache.miniMapWidget == miniWidget);

    auto failRetainedLayouts = [&memory, controller, bigWidget, miniWidget](
                                   std::uintptr_t address,
                                   void* output,
                                   std::size_t size) {
        if (address == controller + 0x400 ||
            address == bigWidget + 0x9B4 ||
            address == bigWidget + 0x898 ||
            address == miniWidget + 0x598 ||
            address == miniWidget + 0xB74) {
            return false;
        }
        return memory.Read(address, output, size);
    };
    REQUIRE(HudMapReader::Refresh(
        controller, cache, failRetainedLayouts, resolve));
    REQUIRE(cache.bigMapWidget == bigWidget);
    REQUIRE(cache.miniMapWidget == miniWidget);
    REQUIRE(cache.bigMapVisible);
    REQUIRE(cache.bigScaleX == 400.0f);
    REQUIRE(cache.miniScale == 2.0f);

    auto failResolvedBigLayout = [&memory, bigWidget](
                                     std::uintptr_t address,
                                     void* output,
                                     std::size_t size) {
        if (address == bigWidget + 0x898) return false;
        return memory.Read(address, output, size);
    };
    REQUIRE(HudMapReader::Refresh(
        controller, cache, failResolvedBigLayout, resolve));
    REQUIRE(cache.bigMapWidget == bigWidget);
    REQUIRE(cache.bigScaleX == 400.0f);

    auto resolveWithoutMini = [](std::uint32_t index) {
        return index == 11
            ? std::string("MobileHUD_BigMap")
            : std::string{};
    };
    REQUIRE(HudMapReader::Refresh(
        controller, cache, read, resolveWithoutMini));
    REQUIRE(cache.bigMapWidget == bigWidget);
    REQUIRE(cache.miniMapWidget == miniWidget);
    REQUIRE(cache.miniScale == 2.0f);

    cache.bigLastValidAt =
        std::chrono::steady_clock::now() - std::chrono::seconds(3);
    cache.miniLastValidAt =
        std::chrono::steady_clock::now() - std::chrono::seconds(3);
    REQUIRE(!HudMapReader::Refresh(
        controller, cache, failRetainedLayouts, resolve));
    REQUIRE(cache.controller == 0);
    REQUIRE(cache.bigMapWidget == 0);
    REQUIRE(cache.miniMapWidget == 0);

    constexpr std::uintptr_t replacementController = 0x120000;
    REQUIRE(!HudMapReader::Refresh(
        replacementController, cache, read, resolve));
    REQUIRE(cache.controller == 0);
    REQUIRE(cache.bigMapWidget == 0);
    REQUIRE(cache.miniMapWidget == 0);
}
