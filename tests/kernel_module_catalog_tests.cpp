#include "test_support.h"

#include "game/native/KernelModuleCatalog.h"

#include <string_view>

void RunKernelModuleCatalogTests() {
    using lengjing::game::native::FindKernelModuleVariant;
    using lengjing::game::native::KernelModuleId;
    using lengjing::game::native::kKernelModuleVariants;

    REQUIRE(kKernelModuleVariants.size() == 7);
    REQUIRE(
        FindKernelModuleVariant("5.10.157+")->module ==
        KernelModuleId::Kernel510252);
    REQUIRE(
        FindKernelModuleVariant("5.15.202-android13-vendor")->module ==
        KernelModuleId::Kernel515202Android13);
    REQUIRE(
        FindKernelModuleVariant(
            "6.1.118-android14-11-o-gdc1f87dd9a69")->module ==
        KernelModuleId::Kernel61166);
    REQUIRE(
        FindKernelModuleVariant("6.6.127-custom")->module ==
        KernelModuleId::Kernel66127);
    REQUIRE(
        FindKernelModuleVariant("6.12.76-custom")->module ==
        KernelModuleId::Kernel61276);

    REQUIRE(
        FindKernelModuleVariant("5.10.245-dirty")->module ==
        KernelModuleId::Kernel510252);
    REQUIRE(
        FindKernelModuleVariant("6.2.0")->module ==
        KernelModuleId::Kernel61166);
    REQUIRE(
        FindKernelModuleVariant("6.10.0")->module ==
        KernelModuleId::Kernel66127);
    REQUIRE(
        FindKernelModuleVariant("4.19.157")->module ==
        KernelModuleId::Kernel510252);
    REQUIRE(
        FindKernelModuleVariant(std::string_view{})->module ==
        KernelModuleId::Kernel61276);
}
