#include "test_support.h"

#include "game/native/KernelModuleCatalog.h"

#include <string_view>

void RunKernelModuleCatalogTests() {
    using lengjing::game::native::FindKernelModuleRelease;
    using lengjing::game::native::KernelModuleId;
    using lengjing::game::native::kKernelModuleReleases;

    REQUIRE(kKernelModuleReleases.size() == 7);
    REQUIRE(
        FindKernelModuleRelease("5.10.245-dirty")->module ==
        KernelModuleId::Kernel510245);
    REQUIRE(
        FindKernelModuleRelease(
            "5.15.202-android13-5.15.202_r00-dirty")->module ==
        KernelModuleId::Kernel515202Android13);
    REQUIRE(
        FindKernelModuleRelease("6.12.76-4k-gae4e2f4f997e-dirty")->module ==
        KernelModuleId::Kernel61276);

    REQUIRE(FindKernelModuleRelease("5.10.245") == nullptr);
    REQUIRE(FindKernelModuleRelease("5.10.245-dirty-extra") == nullptr);
    REQUIRE(FindKernelModuleRelease("6.6.127-4k") == nullptr);
    REQUIRE(FindKernelModuleRelease(std::string_view{}) == nullptr);
}
