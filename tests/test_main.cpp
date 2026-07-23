#include "test_support.h"

#include <exception>
#include <iostream>

void RunExecutionVeneerLocatorTests();

int main() {
    try {
        RunActorFrameVisitSetTests();
        RunActorRecordRefreshPolicyTests();
        RunActorRecordSourceTests();
        RunActorRecordResolverTests();
        RunAlgorithmPositionPolicyTests();
        RunAimGuidePolicyTests();
        RunAimModePolicyTests();
        RunAimPredictionTests();
        RunAuthConfigTests();
        RunAuthSessionTests();
        RunAuthTransportTests();
        RunCardInputPolicyTests();
        RunCharacterComponentTransformTests();
        RunCloudLayoutStartupPolicyTests();
        RunCloudLayoutTests();
        RunConfigTests();
        RunCoordinatePoolPolicyTests();
        RunCoordinateOutputPolicyTests();
        RunCpuMailboxPolicyTests();
        RunExecutionVeneerLocatorTests();
        RunFrameProjectionTests();
        RunGameVersionPolicyTests();
        RunGeometrySceneBuildPolicyTests();
        RunHardwareBreakpointCoordinateRuntimeTests();
        RunHudMapTests();
        RunKernelModuleCatalogTests();
        RunOverlayContrastPolicyTests();
        RunPerfExecutionBreakpointTests();
        RunPlayerBoundsTests();
        RunPlayerDetailReadPolicyTests();
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
        RunTrackingPageBindingTests();
        RunTouchTransformTests();
        RunWorldObjectRefreshPolicyTests();
        std::cout << "all tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
