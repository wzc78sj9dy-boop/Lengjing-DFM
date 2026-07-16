#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace lengjing::game::native {

struct HudMapCache {
    std::uintptr_t controller = 0;
    std::uintptr_t bigMapWidget = 0;
    std::uintptr_t miniMapWidget = 0;
    bool bigMapVisible = false;

    float bigOriginX = 0.0f;
    float bigOriginY = 0.0f;
    float bigScaleX = 0.0f;
    float bigScaleY = 0.0f;
    std::int32_t bigWorldOffsetX = 0;
    std::int32_t bigWorldOffsetY = 0;

    float miniScale = 0.0f;
    float miniPositionX = 0.0f;
    float miniPositionY = 0.0f;
    float miniSize = 0.0f;
    std::chrono::steady_clock::time_point bigLastValidAt{};
    std::chrono::steady_clock::time_point miniLastValidAt{};

    void Clear() noexcept { *this = HudMapCache{}; }
};

class HudMapReader final {
public:
    template <typename ReadBytes, typename ResolveName>
    static bool Refresh(std::uintptr_t controller,
                        HudMapCache& cache,
                        ReadBytes&& readBytes,
                        ResolveName&& resolveName) {
        if (controller == 0) {
            cache.Clear();
            return false;
        }
        if (cache.controller != 0 && cache.controller != controller) {
            cache.Clear();
        }

        HudMapCache resolved{};
        resolved.controller = controller;
        const auto now = std::chrono::steady_clock::now();

        std::uintptr_t hud = 0;
        std::uintptr_t holder = 0;
        std::uintptr_t structure = 0;
        if (!ReadAt(readBytes, controller, 0x400, hud) || hud == 0 ||
            !ReadAt(readBytes, hud, 0x4A8, holder) || holder == 0 ||
            !ReadAt(readBytes, holder, 0xE0, structure) || structure == 0) {
            return RefreshRetained(controller, cache, readBytes);
        }

        std::uintptr_t fields = 0;
        std::uint32_t count = 0;
        if (!ReadAt(readBytes, structure, 0x28, fields) || fields == 0 ||
            !ReadAt(readBytes, structure, 0x30, count) ||
            count == 0 || count > 0x12B) {
            return RefreshRetained(controller, cache, readBytes);
        }

        for (std::uint32_t index = 0; index < count; ++index) {
            const std::uintptr_t indexValue = static_cast<std::uintptr_t>(index);
            if (indexValue >
                (std::numeric_limits<std::uintptr_t>::max() - fields) /
                    sizeof(FieldEntry)) {
                break;
            }
            FieldEntry entry{};
            if (!ReadValue(
                    readBytes,
                    fields + indexValue * sizeof(FieldEntry),
                    entry) ||
                entry.property == 0) {
                continue;
            }

            const auto resolvedName = resolveName(entry.nameIndex);
            const std::string_view name(resolvedName);
            std::uintptr_t widget = 0;
            if (name.find("MobileHUD_BigMap") != std::string_view::npos) {
                if (ReadAt(readBytes, entry.property, 0x38, widget)) {
                    resolved.bigMapWidget = widget;
                }
            } else if (name.find("MobileHUD_MiniMap") != std::string_view::npos) {
                if (ReadAt(readBytes, entry.property, 0x38, widget)) {
                    resolved.miniMapWidget = widget;
                }
            }
        }

        const HudMapCache* fallback =
            cache.controller == controller ? &cache : nullptr;
        if (fallback != nullptr) MergeMissingWidgets(resolved, *fallback);
        SyncLayouts(resolved, readBytes, fallback, now);
        if (resolved.bigMapWidget != 0 || resolved.miniMapWidget != 0) {
            cache = resolved;
            return true;
        }
        return RefreshRetained(controller, cache, readBytes);
    }

private:
    struct FieldEntry {
        std::uint32_t nameIndex = 0;
        std::uint32_t padding = 0;
        std::uintptr_t property = 0;
        std::uintptr_t fieldClass = 0;
    };
    static_assert(sizeof(FieldEntry) == 24, "HUD field entry layout mismatch");

    struct BigLayout {
        float originX = 0.0f;
        float originY = 0.0f;
        float scaleX = 0.0f;
        float scaleY = 0.0f;
        std::int32_t worldOffsetX = 0;
        std::int32_t worldOffsetY = 0;
    };
    static_assert(sizeof(BigLayout) == 24, "big map layout mismatch");

    struct MiniLayout {
        float positionX = 0.0f;
        float positionY = 0.0f;
        float size = 0.0f;
    };
    static_assert(sizeof(MiniLayout) == 12, "mini map layout mismatch");

    template <typename ReadBytes>
    static bool RefreshRetained(std::uintptr_t controller,
                                HudMapCache& cache,
                                ReadBytes& readBytes) {
        if (cache.controller != controller) {
            cache.Clear();
            return false;
        }
        const HudMapCache previous = cache;
        HudMapCache retained = cache;
        SyncLayouts(
            retained, readBytes, &previous, std::chrono::steady_clock::now());
        if (retained.bigMapWidget == 0 && retained.miniMapWidget == 0) {
            cache.Clear();
            return false;
        }
        cache = retained;
        return true;
    }

    static constexpr auto kRetainedLayoutLifetime =
        std::chrono::seconds(2);

    static bool CanRetain(
        std::chrono::steady_clock::time_point lastValidAt,
        std::chrono::steady_clock::time_point now) noexcept {
        return lastValidAt != std::chrono::steady_clock::time_point{} &&
            now - lastValidAt <= kRetainedLayoutLifetime;
    }

    static void CopyBigLayout(HudMapCache& destination,
                              const HudMapCache& source) noexcept {
        destination.bigMapWidget = source.bigMapWidget;
        destination.bigMapVisible = source.bigMapVisible;
        destination.bigOriginX = source.bigOriginX;
        destination.bigOriginY = source.bigOriginY;
        destination.bigScaleX = source.bigScaleX;
        destination.bigScaleY = source.bigScaleY;
        destination.bigWorldOffsetX = source.bigWorldOffsetX;
        destination.bigWorldOffsetY = source.bigWorldOffsetY;
        destination.bigLastValidAt = source.bigLastValidAt;
    }

    static void CopyMiniLayout(HudMapCache& destination,
                               const HudMapCache& source) noexcept {
        destination.miniMapWidget = source.miniMapWidget;
        destination.miniScale = source.miniScale;
        destination.miniPositionX = source.miniPositionX;
        destination.miniPositionY = source.miniPositionY;
        destination.miniSize = source.miniSize;
        destination.miniLastValidAt = source.miniLastValidAt;
    }

    static void MergeMissingWidgets(HudMapCache& destination,
                                    const HudMapCache& source) noexcept {
        if (destination.bigMapWidget == 0 && source.bigMapWidget != 0) {
            CopyBigLayout(destination, source);
        }
        if (destination.miniMapWidget == 0 && source.miniMapWidget != 0) {
            CopyMiniLayout(destination, source);
        }
    }

    template <typename ReadBytes>
    static void SyncLayouts(HudMapCache& cache,
                            ReadBytes& readBytes,
                            const HudMapCache* fallback,
                            std::chrono::steady_clock::time_point now) {
        if (cache.bigMapWidget != 0) {
            std::uint8_t visible = 0;
            BigLayout layout{};
            const bool visibilityRead = ReadAt(
                readBytes, cache.bigMapWidget, 0x9B4, visible);
            const bool layoutRead = ReadAt(
                readBytes, cache.bigMapWidget, 0x898, layout);
            if (!visibilityRead || !layoutRead) {
                if (fallback != nullptr &&
                    fallback->bigMapWidget == cache.bigMapWidget &&
                    CanRetain(fallback->bigLastValidAt, now)) {
                    CopyBigLayout(cache, *fallback);
                } else {
                    cache.bigMapWidget = 0;
                }
            } else if (!std::isfinite(layout.scaleX) ||
                !std::isfinite(layout.scaleY) ||
                layout.scaleX <= 10.0f || layout.scaleY <= 10.0f) {
                cache.bigMapWidget = 0;
            } else {
                cache.bigMapVisible = visible != 0;
                cache.bigOriginX = layout.originX;
                cache.bigOriginY = layout.originY;
                cache.bigScaleX = layout.scaleX;
                cache.bigScaleY = layout.scaleY;
                cache.bigWorldOffsetX = layout.worldOffsetX;
                cache.bigWorldOffsetY = layout.worldOffsetY;
                cache.bigLastValidAt = now;
            }
        }

        if (cache.miniMapWidget != 0) {
            float scale = 0.0f;
            MiniLayout layout{};
            const bool scaleRead = ReadAt(
                readBytes, cache.miniMapWidget, 0x598, scale);
            const bool layoutRead = ReadAt(
                readBytes, cache.miniMapWidget, 0xB74, layout);
            if (!scaleRead || !layoutRead) {
                if (fallback != nullptr &&
                    fallback->miniMapWidget == cache.miniMapWidget &&
                    CanRetain(fallback->miniLastValidAt, now)) {
                    CopyMiniLayout(cache, *fallback);
                } else {
                    cache.miniMapWidget = 0;
                }
            } else if (!std::isfinite(scale) ||
                !std::isfinite(layout.positionX) ||
                !std::isfinite(layout.positionY) ||
                !std::isfinite(layout.size) ||
                (layout.positionX == 0.0f && layout.positionY == 0.0f &&
                 layout.size == 0.0f)) {
                cache.miniMapWidget = 0;
            } else {
                cache.miniScale = scale;
                cache.miniPositionX = layout.positionX;
                cache.miniPositionY = layout.positionY;
                cache.miniSize = layout.size;
                cache.miniLastValidAt = now;
            }
        }
    }

    template <typename ReadBytes, typename T>
    static bool ReadValue(ReadBytes& readBytes,
                          std::uintptr_t address,
                          T& value) {
        value = T{};
        return address != 0 &&
            static_cast<bool>(readBytes(address, &value, sizeof(value)));
    }

    template <typename ReadBytes, typename T>
    static bool ReadAt(ReadBytes& readBytes,
                       std::uintptr_t base,
                       std::uintptr_t offset,
                       T& value) {
        if (base == 0 ||
            offset > std::numeric_limits<std::uintptr_t>::max() - base) {
            value = T{};
            return false;
        }
        return ReadValue(readBytes, base + offset, value);
    }
};

}  // namespace lengjing::game::native
