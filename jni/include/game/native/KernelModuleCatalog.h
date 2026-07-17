#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lengjing::game::native {

enum class KernelModuleId : std::uint8_t {
    Kernel510245,
    Kernel510252,
    Kernel515202,
    Kernel515202Android13,
    Kernel61166,
    Kernel66127,
    Kernel61276,
};

struct KernelModuleVariant final {
    std::string_view kernelVersion;
    std::string_view device;
    KernelModuleId module;
};

inline constexpr std::array<KernelModuleVariant, 7> kKernelModuleVariants{{
    {"5.10", "Pixel", KernelModuleId::Kernel510245},
    {"5.10", {}, KernelModuleId::Kernel510252},
    {"5.15", "Pixel", KernelModuleId::Kernel515202},
    {"5.15", {}, KernelModuleId::Kernel515202Android13},
    {"6.1", {}, KernelModuleId::Kernel61166},
    {"6.6", {}, KernelModuleId::Kernel66127},
    {"6.12", {}, KernelModuleId::Kernel61276},
}};

namespace detail {

struct NumericKernelVersion final {
    int major = -1;
    int minor = -1;
    int patch = -1;

    constexpr bool IsValid() const noexcept {
        return major >= 0 && minor >= 0;
    }
};

constexpr bool ParseVersionPart(std::string_view value,
                                std::size_t& cursor,
                                int& result) noexcept {
    if (cursor >= value.size() || value[cursor] < '0' ||
        value[cursor] > '9') {
        return false;
    }
    int parsed = 0;
    while (cursor < value.size() && value[cursor] >= '0' &&
           value[cursor] <= '9') {
        parsed = parsed * 10 + static_cast<int>(value[cursor] - '0');
        if (parsed > 65535) return false;
        ++cursor;
    }
    result = parsed;
    return true;
}

constexpr NumericKernelVersion ParseKernelVersion(
    std::string_view value) noexcept {
    NumericKernelVersion version{};
    std::size_t cursor = 0;
    if (!ParseVersionPart(value, cursor, version.major) ||
        cursor >= value.size() || value[cursor] != '.') {
        return {};
    }
    ++cursor;
    if (!ParseVersionPart(value, cursor, version.minor)) return {};
    if (cursor < value.size() && value[cursor] == '.') {
        ++cursor;
        if (!ParseVersionPart(value, cursor, version.patch)) return {};
    }
    return version;
}

constexpr bool IsAutomaticCandidate(
    const KernelModuleVariant& candidate,
    bool hasGeneric) noexcept {
    return !hasGeneric || candidate.device.empty();
}

constexpr int AbsoluteDifference(int left, int right) noexcept {
    return left >= right ? left - right : right - left;
}

}  // namespace detail

constexpr const KernelModuleVariant* FindKernelModuleVariant(
    std::string_view kernelRelease) noexcept {
    bool hasGeneric = false;
    const KernelModuleVariant* lastAutomatic = nullptr;
    for (const KernelModuleVariant& candidate : kKernelModuleVariants) {
        if (candidate.device.empty()) hasGeneric = true;
    }
    for (const KernelModuleVariant& candidate : kKernelModuleVariants) {
        if (detail::IsAutomaticCandidate(candidate, hasGeneric)) {
            lastAutomatic = &candidate;
        }
    }
    if (lastAutomatic == nullptr) return nullptr;

    const detail::NumericKernelVersion kernel =
        detail::ParseKernelVersion(kernelRelease);
    if (!kernel.IsValid()) return lastAutomatic;

    const KernelModuleVariant* sameMinor = nullptr;
    for (const KernelModuleVariant& candidate : kKernelModuleVariants) {
        if (!detail::IsAutomaticCandidate(candidate, hasGeneric)) continue;
        const detail::NumericKernelVersion version =
            detail::ParseKernelVersion(candidate.kernelVersion);
        if (version.IsValid() && version.major == kernel.major &&
            version.minor == kernel.minor) {
            sameMinor = &candidate;
        }
    }
    if (sameMinor != nullptr) return sameMinor;

    const KernelModuleVariant* sameMajorBelow = nullptr;
    int greatestMinor = -1;
    for (const KernelModuleVariant& candidate : kKernelModuleVariants) {
        if (!detail::IsAutomaticCandidate(candidate, hasGeneric)) continue;
        const detail::NumericKernelVersion version =
            detail::ParseKernelVersion(candidate.kernelVersion);
        if (version.IsValid() && version.major == kernel.major &&
            version.minor <= kernel.minor && version.minor > greatestMinor) {
            greatestMinor = version.minor;
            sameMajorBelow = &candidate;
        }
    }
    if (sameMajorBelow != nullptr) return sameMajorBelow;

    const KernelModuleVariant* sameMajorAny = nullptr;
    int lowestMinor = 65536;
    for (const KernelModuleVariant& candidate : kKernelModuleVariants) {
        if (!detail::IsAutomaticCandidate(candidate, hasGeneric)) continue;
        const detail::NumericKernelVersion version =
            detail::ParseKernelVersion(candidate.kernelVersion);
        if (version.IsValid() && version.major == kernel.major &&
            version.minor < lowestMinor) {
            lowestMinor = version.minor;
            sameMajorAny = &candidate;
        }
    }
    if (sameMajorAny != nullptr) return sameMajorAny;

    const KernelModuleVariant* closest = nullptr;
    int closestScore = 2147483647;
    for (const KernelModuleVariant& candidate : kKernelModuleVariants) {
        if (!detail::IsAutomaticCandidate(candidate, hasGeneric)) continue;
        const detail::NumericKernelVersion version =
            detail::ParseKernelVersion(candidate.kernelVersion);
        if (!version.IsValid()) continue;
        const int score =
            detail::AbsoluteDifference(version.major, kernel.major) * 1000 +
            version.minor;
        if (score < closestScore) {
            closestScore = score;
            closest = &candidate;
        }
    }
    return closest;
}

}  // namespace lengjing::game::native
