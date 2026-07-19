#pragma once

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace lengjing::game::native {

enum class PositionReadMode : std::uint8_t {
    Standard,
    Direct,
};

class CharacterPositionResolver final {
public:
    using Coordinate = std::array<float, 3>;

    template <typename ReadBytes>
    bool Read(std::uintptr_t actor,
              std::string_view className,
              PositionReadMode mode,
              bool antiFlicker,
              Coordinate& coordinate,
              ReadBytes&& readBytes) {
        return ReadWithRoot(
            actor,
            0,
            className,
            mode,
            antiFlicker,
            coordinate,
            readBytes);
    }

    template <typename ReadBytes>
    bool ReadWithRoot(std::uintptr_t actor,
                      std::uintptr_t decodedRoot,
                      std::string_view className,
                      PositionReadMode mode,
                      bool antiFlicker,
                      Coordinate& coordinate,
                      ReadBytes&& readBytes) {
        return ReadWithPolicy(
            actor,
            decodedRoot,
            className,
            mode,
            antiFlicker,
            false,
            coordinate,
            readBytes);
    }

    template <typename ReadBytes>
    bool ReadLocalWithRoot(std::uintptr_t actor,
                           std::uintptr_t decodedRoot,
                           std::string_view className,
                           PositionReadMode mode,
                           bool antiFlicker,
                           Coordinate& coordinate,
                           ReadBytes&& readBytes) {
        return ReadWithPolicy(
            actor,
            decodedRoot,
            className,
            mode,
            antiFlicker,
            true,
            coordinate,
            readBytes);
    }

    static bool IsRangeTargetCharacter(std::string_view className) noexcept {
        return
            className.find("RangeTargetCharacter") !=
                std::string_view::npos ||
            className.find("RangeTargeCharacter") !=
                std::string_view::npos;
    }

    static bool IsPrimaryCharacter(std::string_view className) noexcept {
        return className == "NC_BP_DFMCharacter_C" ||
            className == "NC_BP_DFMCharacter_TutorialPlayerAi_C" ||
            IsRangeTargetCharacter(className);
    }

    static bool IsDirectAiCharacter(std::string_view className) noexcept {
        return className.rfind("NC_BP_DFMCharacter_AI", 0) == 0 ||
            className.rfind("NC_BP_DFMAICharacter", 0) == 0;
    }

    static bool IsTargetCharacter(std::string_view className) noexcept {
        return IsPrimaryCharacter(className) ||
            IsDirectAiCharacter(className) ||
            className.find("DFMCharacter") != std::string_view::npos;
    }

    void Clear() {
        positionHistory_.clear();
        lastPruneAt_ = {};
    }

private:
    template <typename ReadBytes>
    bool ReadWithPolicy(std::uintptr_t actor,
                        std::uintptr_t decodedRoot,
                        std::string_view className,
                        PositionReadMode mode,
                        bool antiFlicker,
                        bool localActor,
                        Coordinate& coordinate,
                        ReadBytes&& readBytes) {
        coordinate = Coordinate{};
        const auto now = std::chrono::steady_clock::now();
        PruneIfDue(now);
        const bool rangeTarget = IsRangeTargetCharacter(className);

        if (mode == PositionReadMode::Direct) {
            if (decodedRoot == 0) return false;
            return localActor
                ? ReadDecodedLocalRoot(decodedRoot, coordinate, readBytes)
                : ReadDecodedRoot(decodedRoot, coordinate, readBytes);
        }

        bool resolved = false;
        if (rangeTarget) {
            resolved = decodedRoot != 0 &&
                ReadDecodedStandardRoot(decodedRoot, coordinate, readBytes) &&
                IsValid(coordinate);
            if (!resolved) {
                resolved = ReadStandard(
                    actor,
                    coordinate,
                    readBytes);
            }
        } else {
            resolved = ReadStandard(
                actor,
                coordinate,
                readBytes);
        }

        if (resolved) {
            if (antiFlicker) {
                positionHistory_[actor] = HistoryEntry{coordinate, now};
            }
            return true;
        }
        return antiFlicker &&
            Restore(positionHistory_, actor, now, coordinate);
    }

    static bool IsValid(const Coordinate& coordinate) noexcept {
        return std::isfinite(coordinate[0]) &&
            std::isfinite(coordinate[1]) &&
            std::isfinite(coordinate[2]) &&
            (coordinate[0] != 0.0f || coordinate[1] != 0.0f ||
             coordinate[2] != 0.0f);
    }

    template <typename ReadBytes, typename T>
    static bool ReadValue(ReadBytes& readBytes,
                          std::uintptr_t address,
                          T& value) {
        value = T{};
        return address != 0 &&
            static_cast<bool>(readBytes(address, &value, sizeof(value)));
    }

    template <typename ReadBytes>
    static bool ReadComponent(std::uintptr_t actor,
                              std::uintptr_t& component,
                              ReadBytes& readBytes) {
        return actor <=
                std::numeric_limits<std::uintptr_t>::max() - 0x180 &&
            ReadValue(readBytes, actor + 0x180, component) && component != 0;
    }

    template <typename ReadBytes>
    static bool ReadDecodedRoot(std::uintptr_t root,
                                Coordinate& coordinate,
                                ReadBytes& readBytes) {
        constexpr std::array<std::uintptr_t, 3> fallbackOffsets{
            0x168, 0x148, 0x220};
        for (const std::uintptr_t offset : fallbackOffsets) {
            coordinate = Coordinate{};
            if (root <=
                std::numeric_limits<std::uintptr_t>::max() - offset) {
                static_cast<void>(
                    ReadValue(readBytes, root + offset, coordinate));
            }
            if (!IsZero(coordinate)) return true;
        }
        coordinate = Coordinate{};
        if (root <=
            std::numeric_limits<std::uintptr_t>::max() - 0x240) {
            static_cast<void>(
                ReadValue(readBytes, root + 0x240, coordinate));
        }
        return true;
    }

    template <typename ReadBytes>
    static bool ReadDecodedLocalRoot(std::uintptr_t root,
                                     Coordinate& coordinate,
                                     ReadBytes& readBytes) {
        coordinate = Coordinate{};
        if (root <=
            std::numeric_limits<std::uintptr_t>::max() - 0x168) {
            static_cast<void>(
                ReadValue(readBytes, root + 0x168, coordinate));
        }
        return true;
    }

    template <typename ReadBytes>
    static bool ReadDecodedStandardRoot(std::uintptr_t root,
                                        Coordinate& coordinate,
                                        ReadBytes& readBytes) {
        return root <=
                std::numeric_limits<std::uintptr_t>::max() - 0x220 &&
            ReadValue(readBytes, root + 0x220, coordinate);
    }

    static bool IsZero(const Coordinate& coordinate) noexcept {
        return coordinate[0] == 0.0f && coordinate[1] == 0.0f &&
            coordinate[2] == 0.0f;
    }

    template <typename ReadBytes>
    static bool ReadStandard(std::uintptr_t actor,
                             Coordinate& coordinate,
                             ReadBytes& readBytes) {
        std::uintptr_t component = 0;
        if (!ReadComponent(actor, component, readBytes)) return false;
        constexpr std::array<std::uintptr_t, 2> offsets{0x168, 0x220};
        for (const std::uintptr_t offset : offsets) {
            if (component <=
                    std::numeric_limits<std::uintptr_t>::max() - offset &&
                ReadValue(readBytes, component + offset, coordinate) &&
                IsValid(coordinate)) {
                return true;
            }
        }
        coordinate = Coordinate{};
        return false;
    }

    struct HistoryEntry {
        Coordinate coordinate{};
        std::chrono::steady_clock::time_point updatedAt{};
    };

    static constexpr auto kHistoryLifetime = std::chrono::milliseconds(300);

    static bool Restore(
        std::unordered_map<std::uintptr_t, HistoryEntry>& history,
        std::uintptr_t actor,
        std::chrono::steady_clock::time_point now,
        Coordinate& coordinate) {
        auto found = history.find(actor);
        if (found == history.end()) return false;
        if (now - found->second.updatedAt > kHistoryLifetime) {
            history.erase(found);
            return false;
        }
        coordinate = found->second.coordinate;
        return true;
    }

    void PruneIfDue(std::chrono::steady_clock::time_point now) {
        if (lastPruneAt_.time_since_epoch().count() != 0 &&
            now - lastPruneAt_ < kHistoryLifetime) {
            return;
        }
        const auto prune = [now](auto& history) {
            for (auto iterator = history.begin(); iterator != history.end();) {
                if (now - iterator->second.updatedAt > kHistoryLifetime) {
                    iterator = history.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        };
        prune(positionHistory_);
        lastPruneAt_ = now;
    }

    std::unordered_map<std::uintptr_t, HistoryEntry> positionHistory_;
    std::chrono::steady_clock::time_point lastPruneAt_{};
};

}  // namespace lengjing::game::native
