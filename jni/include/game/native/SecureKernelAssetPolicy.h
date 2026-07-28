#pragma once

#include <string_view>

namespace lengjing::game::native {

enum class SecureKernelAsset {
    None,
    Kernel510,
    Kernel515,
    Kernel61,
    Kernel66,
    Kernel612,
};

constexpr bool KernelReleaseStartsWith(
    std::string_view release,
    std::string_view prefix) noexcept {
    return release.size() >= prefix.size() &&
        release.compare(0, prefix.size(), prefix) == 0;
}

constexpr SecureKernelAsset SelectSecureKernelAsset(
    std::string_view release) noexcept {
    if (KernelReleaseStartsWith(release, "5.10")) {
        return SecureKernelAsset::Kernel510;
    }
    if (KernelReleaseStartsWith(release, "5.15")) {
        return SecureKernelAsset::Kernel515;
    }
    if (KernelReleaseStartsWith(release, "6.12")) {
        return SecureKernelAsset::Kernel612;
    }
    if (KernelReleaseStartsWith(release, "6.1")) {
        return SecureKernelAsset::Kernel61;
    }
    if (KernelReleaseStartsWith(release, "6.6")) {
        return SecureKernelAsset::Kernel66;
    }
    return SecureKernelAsset::None;
}

}  // namespace lengjing::game::native
