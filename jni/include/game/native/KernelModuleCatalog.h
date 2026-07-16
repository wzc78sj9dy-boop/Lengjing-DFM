#pragma once

#include <array>
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

struct KernelModuleRelease final {
    std::string_view release;
    KernelModuleId module;
};

inline constexpr std::array<KernelModuleRelease, 7> kKernelModuleReleases{{
    {"5.10.245-dirty", KernelModuleId::Kernel510245},
    {"5.10.252-dirty", KernelModuleId::Kernel510252},
    {"5.15.202-dirty", KernelModuleId::Kernel515202},
    {"5.15.202-android13-5.15.202_r00-dirty",
     KernelModuleId::Kernel515202Android13},
    {"6.1.166-dirty", KernelModuleId::Kernel61166},
    {"6.6.127-4k-g46a034eca005-dirty", KernelModuleId::Kernel66127},
    {"6.12.76-4k-gae4e2f4f997e-dirty", KernelModuleId::Kernel61276},
}};

constexpr const KernelModuleRelease* FindKernelModuleRelease(
    std::string_view release) noexcept {
    for (const KernelModuleRelease& candidate : kKernelModuleReleases) {
        if (candidate.release == release) return &candidate;
    }
    return nullptr;
}

}  // namespace lengjing::game::native
