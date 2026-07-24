#include "test_support.h"

#include <exception>
#include <iostream>

#if 0
void RunExecutionVeneerLocatorTests();
#endif

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
        RunCoordinatePoolCloudLayoutStandaloneTests();
        RunConfigTests();
        RunCoordinatePoolPolicyTests();
        RunCoordinateOutputPolicyTests();
        RunCpuMailboxPolicyTests();
#if 0
        RunExecutionVeneerLocatorTests();
#endif
        RunFrameProjectionTests();
        RunGameVersionPolicyTests();
        RunGeometrySceneBuildPolicyTests();
#if 0
        RunHardwareBreakpointCoordinateRuntimeTests();
#endif
        RunHudMapTests();
        RunKernelModuleCatalogTests();
        RunOverlayContrastPolicyTests();
#if 0
        RunPerfExecutionBreakpointTests();
#endif
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
