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
void RunActorFrameVisitSetTests();
void RunActorRecordResolverTests();
void RunAlgorithmPositionPolicyTests();
void RunAimModePolicyTests();
void RunAimPredictionTests();
void RunAuthConfigTests();
void RunAuthSessionTests();
void RunAuthTransportTests();
void RunCardInputPolicyTests();
void RunCloudLayoutTests();
void RunCpuMailboxPolicyTests();
void RunFrameProjectionTests();
void RunHudMapTests();
void RunKernelModuleCatalogTests();
void RunPlayerBoundsTests();
void RunPlayerTrackingTests();
void RunPositionResolverTests();
void RunPresentationRateTests();
void RunProjectileSpeedTests();
void RunRenderBackendSelectionTests();
void RunRuntimeLayoutOverrideTests();
void RunRuntimeTests();
void RunThreatCatalogTests();
void RunTrackingCalculatorTests();
void RunTouchTransformTests();
void RunWorldObjectRefreshPolicyTests();
