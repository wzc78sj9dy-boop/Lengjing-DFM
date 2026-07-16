#include "test_support.h"

#include <exception>
#include <iostream>

int main() {
    try {
        RunConfigTests();
        RunCoordinateReaderTests();
        RunHudMapTests();
        RunKernelModuleCatalogTests();
        RunPlayerTrackingTests();
        RunPositionResolverTests();
        RunPresentationRateTests();
        RunProjectileSpeedTests();
        RunRuntimeTests();
        RunThreatCatalogTests();
        RunTouchTransformTests();
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
