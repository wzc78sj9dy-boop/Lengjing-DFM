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
void RunActorRecordResolverTests();
void RunCoordinateReaderTests();
void RunHudMapTests();
void RunKernelModuleCatalogTests();
void RunPlayerTrackingTests();
void RunPositionResolverTests();
void RunPresentationRateTests();
void RunProjectileSpeedTests();
void RunRuntimeTests();
void RunThreatCatalogTests();
void RunTrackingBoneSelectorTests();
void RunTrackingCalculatorTests();
void RunTouchTransformTests();
