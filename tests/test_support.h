#pragma once

#include <stdexcept>
#include <string>

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error(                                             \
                std::string(__FILE__) + ":" + std::to_string(__LINE__) +         \
                ": requirement failed: " #condition);                           \
        }                                                                         \
    } while (false)

void RunConfigTests();
void RunCoordinatePoolPolicyTests();
void RunCoordinateOutputPolicyTests();
void RunActorFrameVisitSetTests();
void RunActorRecordRefreshPolicyTests();
void RunActorRecordSourceTests();
void RunActorRecordResolverTests();
void RunAlgorithmPositionPolicyTests();
void RunAimGuidePolicyTests();
void RunAimModePolicyTests();
void RunAimPredictionTests();
void RunAuthConfigTests();
void RunAuthSessionTests();
void RunAuthTransportTests();
void RunCardInputPolicyTests();
void RunCharacterComponentTransformTests();
void RunCloudLayoutStartupPolicyTests();
void RunCloudLayoutTests();
void RunCoordinatePoolCloudLayoutStandaloneTests();
void RunCpuMailboxPolicyTests();
void RunFrameProjectionTests();
void RunGameVersionPolicyTests();
void RunGeometrySceneBuildPolicyTests();
#if 0
void RunHardwareBreakpointCoordinateRuntimeTests();
#endif
void RunHudMapTests();
void RunKernelModuleCatalogTests();
void RunOverlayContrastPolicyTests();
#if 0
void RunPerfExecutionBreakpointTests();
#endif
void RunPlayerBoundsTests();
void RunPlayerDetailReadPolicyTests();
void RunPlayerTracerPolicyTests();
void RunPlayerTrackingTests();
void RunPositionResolverTests();
void RunPresentationRateTests();
void RunProjectileSpeedTests();
void RunRenderBackendSelectionTests();
void RunRemoteElfIdentityTests();
void RunRuntimeLayoutOverrideTests();
void RunRuntimeExitPolicyTests();
void RunRuntimePresentationPolicyTests();
void RunRuntimeTests();
void RunThreatCatalogTests();
void RunTrackingCalculatorTests();
void RunTrackingPageBindingTests();
void RunTouchTransformTests();
void RunWorldObjectRefreshPolicyTests();
