#include "test_support.h"

#include <exception>
#include <iostream>

int main() {
    try {
        RunActorFrameVisitSetTests();
        RunActorRecordResolverTests();
        RunAlgorithmPositionPolicyTests();
        RunAimModePolicyTests();
        RunAimPredictionTests();
        RunAuthConfigTests();
        RunAuthSessionTests();
        RunAuthTransportTests();
        RunCardInputPolicyTests();
        RunCloudLayoutTests();
        RunConfigTests();
        RunCpuMailboxPolicyTests();
        RunFrameProjectionTests();
        RunHudMapTests();
        RunKernelModuleCatalogTests();
        RunPlayerBoundsTests();
        RunPlayerTrackingTests();
        RunPositionResolverTests();
        RunPresentationRateTests();
        RunProjectileSpeedTests();
        RunRenderBackendSelectionTests();
        RunRemoteElfIdentityTests();
        RunRuntimeLayoutOverrideTests();
        RunRuntimeTests();
        RunThreatCatalogTests();
        RunTrackingCalculatorTests();
        RunTouchTransformTests();
        RunWorldObjectRefreshPolicyTests();
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
