#include "test_support.h"

#include <exception>
#include <iostream>

int main() {
    try {
        RunActorFrameVisitSetTests();
        RunActorRecordRefreshPolicyTests();
        RunActorRecordSourceTests();
        RunActorRecordResolverTests();
        RunAlgorithmPositionPolicyTests();
        RunAimModePolicyTests();
        RunAimPredictionTests();
        RunAuthConfigTests();
        RunAuthSessionTests();
        RunAuthTransportTests();
        RunCardInputPolicyTests();
        RunCloudLayoutStartupPolicyTests();
        RunCloudLayoutTests();
        RunConfigTests();
        RunCpuMailboxPolicyTests();
        RunFrameProjectionTests();
        RunGeometrySceneBuildPolicyTests();
        RunHudMapTests();
        RunKernelModuleCatalogTests();
        RunOverlayContrastPolicyTests();
        RunPlayerBoundsTests();
        RunPlayerTracerPolicyTests();
        RunPlayerTrackingTests();
        RunPositionResolverTests();
        RunPresentationRateTests();
        RunProjectileSpeedTests();
        RunRenderBackendSelectionTests();
        RunRemoteElfIdentityTests();
        RunRuntimeLayoutOverrideTests();
        RunRuntimeExitPolicyTests();
        RunRuntimePresentationPolicyTests();
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
