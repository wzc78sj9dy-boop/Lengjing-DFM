#pragma once

#include "game/native/IndexedCoordinateReader.h"

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
    Indexed,
};

class CharacterPositionResolver final {
public:
    using Coordinate = IndexedCoordinateReader::Coordinate;

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
        coordinate = Coordinate{};
        const auto now = std::chrono::steady_clock::now();
        PruneIfDue(now);
        const bool primary = IsPrimaryCharacter(className);
        const bool directAi = IsDirectAiCharacter(className);

        if (mode == PositionReadMode::Direct) {
            return decodedRoot != 0 &&
                ReadDecodedRoot(decodedRoot, coordinate, readBytes);
        }

        bool resolved = false;
        if (mode == PositionReadMode::Indexed && primary) {
            resolved = IndexedCoordinateReader::Read(actor, coordinate, readBytes) &&
                IsValid(coordinate);
            if (resolved) indexedHistory_[actor] = HistoryEntry{coordinate, now};
            else resolved = Restore(indexedHistory_, actor, now, coordinate);
        } else if (mode != PositionReadMode::Standard && directAi) {
            resolved = ReadDirect(actor, coordinate, readBytes);
        } else {
            resolved = ReadStandard(actor, coordinate, readBytes);
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

    void Clear() {
        positionHistory_.clear();
        indexedHistory_.clear();
        lastPruneAt_ = {};
    }

    static bool IsPrimaryCharacter(std::string_view className) noexcept {
        return className == "NC_BP_DFMCharacter_C" ||
            className == "NC_BP_DFMCharacter_TutorialPlayerAi_C" ||
            className == "BP_RangeTargetCharacter_C";
    }

    static bool IsDirectAiCharacter(std::string_view className) noexcept {
        return className.rfind("NC_BP_DFMCharacter_AI", 0) == 0 ||
            className.rfind("NC_BP_DFMAICharacter", 0) == 0;
    }

private:
    static bool IsValid(const Coordinate& coordinate) noexcept {
        return std::isfinite(coordinate[0]) &&
            std::isfinite(coordinate[1]) &&
            std::isfinite(coordinate[2]) &&
            (coordinate[0] != 0.0f || coordinate[1] != 0.0f ||
             coordinate[2] != 0.0f);
    }

    static bool IsZero(const Coordinate& coordinate) noexcept {
        return coordinate[0] == 0.0f && coordinate[1] == 0.0f &&
            coordinate[2] == 0.0f;
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
    static bool ReadDirect(std::uintptr_t actor,
                           Coordinate& coordinate,
                           ReadBytes& readBytes) {
        std::uintptr_t component = 0;
        return ReadComponent(actor, component, readBytes) &&
            component <=
                std::numeric_limits<std::uintptr_t>::max() - 0x220 &&
            ReadValue(readBytes, component + 0x220, coordinate) &&
            IsValid(coordinate);
    }

    template <typename ReadBytes>
    static bool ReadDecodedRoot(std::uintptr_t root,
                                Coordinate& coordinate,
                                ReadBytes& readBytes) {
        if (root == 0) return false;
        constexpr std::array<std::uintptr_t, 4> offsets{
            0x168,
            0x148,
            0x220,
            0x240,
        };
        for (std::size_t index = 0; index < offsets.size(); ++index) {
            if (root > std::numeric_limits<std::uintptr_t>::max() -
                    offsets[index]) {
                coordinate = Coordinate{};
            } else {
                ReadValue(readBytes, root + offsets[index], coordinate);
            }
            if (index + 1 == offsets.size()) return true;
            if (!IsZero(coordinate)) return true;
        }
        return false;
    }

    template <typename ReadBytes>
    static bool ReadStandard(std::uintptr_t actor,
                             Coordinate& coordinate,
                             ReadBytes& readBytes) {
        std::uintptr_t component = 0;
        if (!ReadComponent(actor, component, readBytes)) return false;
        if (component <=
                std::numeric_limits<std::uintptr_t>::max() - 0x168 &&
            ReadValue(readBytes, component + 0x168, coordinate) &&
            IsValid(coordinate)) {
            return true;
        }
        return component <=
                std::numeric_limits<std::uintptr_t>::max() - 0x220 &&
            ReadValue(readBytes, component + 0x220, coordinate) &&
            IsValid(coordinate);
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
        prune(indexedHistory_);
        lastPruneAt_ = now;
    }

    std::unordered_map<std::uintptr_t, HistoryEntry> positionHistory_;
    std::unordered_map<std::uintptr_t, HistoryEntry> indexedHistory_;
    std::chrono::steady_clock::time_point lastPruneAt_{};
};

}  // namespace lengjing::game::native
