#ifndef LENGJING_ENABLE_ALGORITHM_COORDINATE
#define LENGJING_ENABLE_ALGORITHM_COORDINATE 0
#endif

#include "game/GameBackend.h"
#include "game/aim/AimController.h"
#include "game/aim/AimGuidePolicy.h"
#include "game/aim/AimModePolicy.h"
#include "game/aim/AimPrediction.h"
#include "game/aim/CoverSelectionPolicy.h"
#if LENGJING_ENABLE_PROJECTILE_TRACKING
#include "game/aim/TrackingCalculator.h"
#endif
#include "game/data/CustomItemCatalog.h"
#include "game/data/ItemCatalog.h"
#include "game/data/ThreatCatalog.h"
#include "game/native/ActorRecordRefreshPolicy.h"
#include "game/native/ActorRecordResolver.h"
#include "game/native/ActorRecordSource.h"
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
#include "game/native/AlgorithmCoordinateReader.h"
#endif
#include "game/native/AlgorithmPositionPolicy.h"
#include "game/native/AlgorithmReplayPolicy.h"
#include "game/native/BoneFrameSource.h"
#include "game/native/CharacterComponentTransform.h"
#include "game/native/CharacterPositionResolver.h"
#include "game/native/CoordinateDecrypt2Runtime.h"
#include "game/native/CoordinateOutputPolicy.h"
#include "game/native/CoordinatePoolRuntime.h"
#include "game/native/ExecutionVeneerLocator.h"
#include "game/native/FrameProjection.h"
#include "game/native/GeometryRuntime.h"
#include "game/native/GeometrySceneBuildPolicy.h"
#if 0
#include "game/native/HardwareBreakpointCoordinateRuntime.h"
#endif
#include "game/native/HudMapProjection.h"
#include "game/native/MemoryTransport.h"
#include "game/native/PlayerBounds.h"
#include "game/native/PlayerDetailReadPolicy.h"
#include "game/native/PlayerTrackingPolicy.h"
#include "game/native/PositionReadModePolicy.h"
#include "game/native/ProjectileSpeedReader.h"
#include "game/native/RemoteElfIdentity.h"
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
#include "game/native/RuntimeCoordinateCodec.h"
#endif
#include "game/native/RuntimeLayoutOverride.h"
#if LENGJING_ENABLE_PROJECTILE_TRACKING
#include "game/native/TrajectoryHook.h"
#endif
#include "game/native/WorldObjectRefreshPolicy.h"
#include "platform/PerformanceTrace.h"
#include "render/PlayerTracerPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <unistd.h>

#ifndef LENGJING_ENABLE_COORDINATE_DEBUG_LOG
#define LENGJING_ENABLE_COORDINATE_DEBUG_LOG 0
#endif

namespace lengjing::game {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr std::uintptr_t kMinimumRemoteAddress = 0x10000000ULL;
constexpr std::uintptr_t kMaximumRemoteAddress = 0x10000000000ULL;
constexpr std::int32_t kMaximumActorCount = 10000;
constexpr std::int32_t kMaximumWorldObjectCount = 65536;
constexpr std::size_t kMaximumNameLength = 249;
constexpr std::uint64_t kCoordinateTraceIntervalFrames = 30;

constexpr bool ShouldWriteCoordinateFrameTrace(
    std::uint64_t frame) noexcept {
    return frame < 5 || (frame % kCoordinateTraceIntervalFrames) == 0;
}

std::uintptr_t ForcedCoordinateProbeComponent() noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    const char* value = std::getenv(
        "LENGJING_COORDINATE_FORCE_COMPONENT");
    if (value == nullptr || value[0] == '\0') return 0;
    std::string_view text(value);
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    std::uintptr_t component = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), component, 16);
    return parsed.ec == std::errc{} &&
            parsed.ptr == text.data() + text.size() &&
            component >= kMinimumRemoteAddress &&
            component < kMaximumRemoteAddress
        ? component
        : 0;
#else
    return 0;
#endif
}

CoordinateDecryptError CoordinatePoolError(
    native::CoordinatePoolRuntimeError error,
    const CoordinateReadDiagnostic& read) noexcept {
    using native::CoordinatePoolRuntimeError;
    if (read.HasFailure() && read.stage == CoordinateReadStage::CodePage) {
        return CoordinateReadError(read.failure);
    }
    switch (error) {
        case CoordinatePoolRuntimeError::None:
            return CoordinateDecryptError::None;
        case CoordinatePoolRuntimeError::InvalidInput:
            return CoordinateDecryptError::InvalidConfiguration;
        case CoordinatePoolRuntimeError::RootReadFailed:
            return CoordinateDecryptError::RootReadFailed;
        case CoordinatePoolRuntimeError::EntryResolveFailed:
            return CoordinateDecryptError::EntryResolveFailed;
        case CoordinatePoolRuntimeError::EntryMappingMissing:
            return CoordinateDecryptError::EntryMappingMissing;
        case CoordinatePoolRuntimeError::EntryMappingFragmented:
            return CoordinateDecryptError::EntryMappingFragmented;
        case CoordinatePoolRuntimeError::EntryMappingChanged:
            return CoordinateDecryptError::EntryMappingChanged;
        case CoordinatePoolRuntimeError::EntryPageReadFailed:
            return CoordinateDecryptError::EntryCodePageReadFailed;
        case CoordinatePoolRuntimeError::EntryCodeReadFailed:
        case CoordinatePoolRuntimeError::CodeReadFailed:
            return read.HasFailure()
                ? CoordinateReadError(read.failure)
                : CoordinateDecryptError::EntryCodeReadFailed;
        case CoordinatePoolRuntimeError::AnalysisFailed:
            return CoordinateDecryptError::CodeAnalysisFailed;
        case CoordinatePoolRuntimeError::EngineSetupFailed:
            return CoordinateDecryptError::EngineSetupFailed;
        case CoordinatePoolRuntimeError::ContextMissing:
            return CoordinateDecryptError::ContextDataInvalid;
        case CoordinatePoolRuntimeError::ParameterExecutionFailed:
            return CoordinateDecryptError::ParameterExecutionFailed;
        case CoordinatePoolRuntimeError::ParameterReadFailed:
            return CoordinateDecryptError::ParameterReadFailed;
        case CoordinatePoolRuntimeError::PoolPointerReadFailed:
            return CoordinateDecryptError::PoolPointerReadFailed;
        case CoordinatePoolRuntimeError::RingSearchFailed:
            return CoordinateDecryptError::RingSearchFailed;
        case CoordinatePoolRuntimeError::RingPreparationFailed:
            return CoordinateDecryptError::RingPreparationFailed;
        case CoordinatePoolRuntimeError::RingExecutionFailed:
            return CoordinateDecryptError::RingExecutionFailed;
        case CoordinatePoolRuntimeError::RingRegisterReadFailed:
            return CoordinateDecryptError::RingRegisterReadFailed;
        case CoordinatePoolRuntimeError::RingValueInvalid:
            return CoordinateDecryptError::RingValueInvalid;
        case CoordinatePoolRuntimeError::PositionReadFailed:
            return CoordinateDecryptError::PositionReadFailed;
        case CoordinatePoolRuntimeError::PositionNotFinite:
            return CoordinateDecryptError::OutputNotFinite;
        case CoordinatePoolRuntimeError::PositionUnstable:
            return CoordinateDecryptError::OutputUnstable;
        case CoordinatePoolRuntimeError::SlotLayoutPending:
            return CoordinateDecryptError::PoolSlotLayoutPending;
        case CoordinatePoolRuntimeError::SlotLayoutConflict:
            return CoordinateDecryptError::PoolSlotLayoutConflict;
        case CoordinatePoolRuntimeError::SlotLayoutEvidenceMissing:
            return CoordinateDecryptError::PoolSlotLayoutEvidenceMissing;
    }
    return CoordinateDecryptError::UnhandledException;
}

CoordinateDecryptError AlgorithmPositionError(
    native::AlgorithmPositionRuntimeError error) noexcept {
    using native::AlgorithmPositionRuntimeError;
    switch (error) {
        case AlgorithmPositionRuntimeError::None:
            return CoordinateDecryptError::None;
        case AlgorithmPositionRuntimeError::InvalidInput:
            return CoordinateDecryptError::ReplayInvalidInput;
        case AlgorithmPositionRuntimeError::EngineSetupFailed:
            return CoordinateDecryptError::ReplayEngineSetupFailed;
        case AlgorithmPositionRuntimeError::PageRefreshFailed:
            return CoordinateDecryptError::ReplayPageRefreshFailed;
        case AlgorithmPositionRuntimeError::RegisterSetupFailed:
            return CoordinateDecryptError::ReplayRegisterSetupFailed;
        case AlgorithmPositionRuntimeError::EmulationFailed:
            return CoordinateDecryptError::ReplayEmulationFailed;
        case AlgorithmPositionRuntimeError::MemoryHookFailed:
            return CoordinateDecryptError::ReplayMemoryHookFailed;
        case AlgorithmPositionRuntimeError::Timeout:
            return CoordinateDecryptError::ReplayTimeout;
        case AlgorithmPositionRuntimeError::ReturnPcMismatch:
            return CoordinateDecryptError::ReplayReturnPcMismatch;
        case AlgorithmPositionRuntimeError::ResultReadFailed:
            return CoordinateDecryptError::ReplayResultReadFailed;
        case AlgorithmPositionRuntimeError::ResultInvalid:
            return CoordinateDecryptError::ReplayResultInvalid;
        case AlgorithmPositionRuntimeError::PacgaUnavailable:
            return CoordinateDecryptError::ReplayPacgaUnavailable;
        case AlgorithmPositionRuntimeError::UnsupportedSvc:
            return CoordinateDecryptError::ReplayUnsupportedSvc;
        case AlgorithmPositionRuntimeError::ContextStale:
            return CoordinateDecryptError::ReplayContextStale;
        case AlgorithmPositionRuntimeError::FaultAddressInvalid:
            return CoordinateDecryptError::ReplayFaultAddressInvalid;
        case AlgorithmPositionRuntimeError::GuestPageMapFailed:
            return CoordinateDecryptError::ReplayGuestPageMapFailed;
        case AlgorithmPositionRuntimeError::RemotePageReadFailed:
            return CoordinateDecryptError::ReplayRemotePageReadFailed;
        case AlgorithmPositionRuntimeError::GuestPageWriteFailed:
            return CoordinateDecryptError::ReplayGuestPageWriteFailed;
        case AlgorithmPositionRuntimeError::InstructionHookSetupFailed:
            return CoordinateDecryptError::ReplayInstructionHookSetupFailed;
    }
    return CoordinateDecryptError::ReplayExecutionFailed;
}

struct CoordinateFailure {
    CoordinateDecryptError error = CoordinateDecryptError::None;
    int systemError = 0;
    CoordinateReadDiagnostic read{};
};

void SetRuntimeFailure(
    RuntimeProbe& probe,
    RuntimeError error,
    int systemError = 0) noexcept {
    probe.runtimeError = error;
    probe.runtimeSystemError = systemError;
}

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

native::AlgorithmPosition ToAlgorithmPosition(const Vec3& value) noexcept {
    return {value.x, value.y, value.z};
}

Vec3 AdjustDecodedPosition(Vec3 value) noexcept {
    value.z = native::ResolveDecodedCharacterZ(value.z);
    return value;
}

enum class CoordinateTraceSource : std::uint8_t {
    None,
    Algorithm,
    Pending,
    Pool,
    PoolRetry,
    Replay,
    Cache,
    StabilityHistory,
#if 0
    HardwareBreakpoint,
#endif
    Standard,
    Failure,
};

const char* CoordinateTraceSourceName(CoordinateTraceSource source) noexcept {
    switch (source) {
        case CoordinateTraceSource::Algorithm:
            return "algorithm";
        case CoordinateTraceSource::Pending:
            return "pending";
        case CoordinateTraceSource::Pool:
            return "pool";
        case CoordinateTraceSource::PoolRetry:
            return "pool_retry";
        case CoordinateTraceSource::Replay:
            return "replay";
        case CoordinateTraceSource::Cache:
            return "cache";
        case CoordinateTraceSource::StabilityHistory:
            return "history";
#if 0
        case CoordinateTraceSource::HardwareBreakpoint:
            return "hardware_breakpoint";
#endif
        case CoordinateTraceSource::Standard:
            return "standard";
        case CoordinateTraceSource::Failure:
            return "failure";
        case CoordinateTraceSource::None:
            break;
    }
    return "none";
}

enum class CoordinateStabilityDecision : std::uint8_t {
    None,
    SingleRead,
    FirstNoHistory,
    FirstNearHistory,
    SecondNearHistory,
    SecondNearFirst,
    SecondPending,
    SecondConfirmed,
    HistorySecondFailed,
    HistorySecondInconsistent,
};

const char* CoordinateStabilityDecisionName(
    CoordinateStabilityDecision decision) noexcept {
    switch (decision) {
        case CoordinateStabilityDecision::SingleRead:
            return "single_read";
        case CoordinateStabilityDecision::FirstNoHistory:
            return "first_no_history";
        case CoordinateStabilityDecision::FirstNearHistory:
            return "first_near_history";
        case CoordinateStabilityDecision::SecondNearHistory:
            return "second_near_history";
        case CoordinateStabilityDecision::SecondNearFirst:
            return "second_near_first";
        case CoordinateStabilityDecision::SecondPending:
            return "second_pending";
        case CoordinateStabilityDecision::SecondConfirmed:
            return "second_confirmed";
        case CoordinateStabilityDecision::HistorySecondFailed:
            return "history_second_failed";
        case CoordinateStabilityDecision::HistorySecondInconsistent:
            return "history_second_inconsistent";
        case CoordinateStabilityDecision::None:
            break;
    }
    return "none";
}

bool IsCoordinateTraceEnabled() noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    static const bool enabled = [] {
        const char* value = std::getenv("LENGJING_COORDINATE_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
bool IsCoordinateTableProbeEnabled() noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    static const bool enabled = [] {
        const char* value = std::getenv("LENGJING_COORDINATE_TABLE_PROBE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}
#endif

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
bool IsAlgorithmCoordinateValidationRequested() noexcept {
    static const bool requested = [] {
        const char* value = std::getenv(
            "LENGJING_ALGORITHM_COORDINATE_VALIDATION");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return requested;
}
#endif

struct Rotation {
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
};

struct CameraView {
    Vec3 location{};
    float padding = 0.0f;
    Rotation rotation{};
    float fieldOfView = 90.0f;
};
static_assert(sizeof(CameraView) == 32, "Camera view layout mismatch");

struct Quaternion {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Transform {
    Quaternion rotation{};
    Vec3 translation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(Transform) == 40, "Transform payload layout mismatch");
static_assert(offsetof(Transform, rotation) == 0,
              "Transform rotation offset mismatch");
static_assert(offsetof(Transform, translation) == 16,
              "Transform translation offset mismatch");
static_assert(offsetof(Transform, scale) == 28,
              "Transform scale offset mismatch");

constexpr std::uintptr_t kBoneTransformStride = 48;
constexpr std::size_t kPlayerBoneTransformCount = 65;

struct Matrix4 {
    float value[4][4]{};
};

struct ActorArrayHeader {
    std::uintptr_t data = 0;
    std::int32_t count = 0;
    std::int32_t capacity = 0;
};
static_assert(sizeof(ActorArrayHeader) == 16, "Actor array layout mismatch");

struct VersionLayout {
    const char* processName = nullptr;
    std::uintptr_t namePoolOffset = 0;
    std::uintptr_t worldOffset = 0;
    std::array<std::uintptr_t, 2> geometryInstancePointerOffsets{};
    native::ActorRecordLayout actorRecordLayout{};
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    std::uintptr_t trackingMatrixRootOffset = 0;
#endif
    std::uintptr_t componentPositionFlagOffset = 0;
};

constexpr std::array<VersionLayout, 3> kVersionLayouts{{
    {"com.tencent.tmgp.dfm",
     0x1CDCB8C0ULL,
     0x1D0E8668ULL,
     {0x1C3C5368ULL, 0x1AF01C68ULL},
     {0x0EEBDB14ULL,
      0x1D0A6908ULL,
      0x180,
      0x3D0,
      1000,
       24,
       10000,
       3000},
#if LENGJING_ENABLE_PROJECTILE_TRACKING
       0x1D0AD4C0ULL,
#endif
       0x1DCFB4FULL},
    {"com.proxima.dfm",
     0x1D0F4800ULL,
     0x1D4115A8ULL,
     {0x1B1C0D68ULL, 0},
     {0, 0, 0, 0, 0, 0, 0, 0},
#if LENGJING_ENABLE_PROJECTILE_TRACKING
      0,
#endif
      0},
    {"com.garena.game.df",
     0x1CF7A440ULL,
     0x1D2971F8ULL,
     {0x1B0669A8ULL, 0},
     {0, 0, 0, 0, 0, 0, 0, 0},
#if LENGJING_ENABLE_PROJECTILE_TRACKING
      0,
#endif
      0},
}};

constexpr std::array<int, 15> kBoneIndices{
    31, 30, 1, 34, 6, 35, 7, 36, 8, 58, 62, 59, 63, 60, 64};

#if LENGJING_ENABLE_PROJECTILE_TRACKING
constexpr std::array<std::uintptr_t, 4> kTrackingClassOffsets{
    0x1A4E2548ULL,
    0x191F50D0ULL,
    0x1A4E6110ULL,
    0x1A3D6A20ULL,
};

constexpr std::array<std::uintptr_t, 3> kTrackingPlayerClassOffsets{
    0x191F50D0ULL,
    0x1A4E6110ULL,
    0x1A3D6A20ULL,
};
#endif

constexpr std::array<BoneLink, 14> kBoneLinks{{
    {0, 1},
    {1, 2},
    {1, 3},
    {3, 5},
    {5, 7},
    {1, 4},
    {4, 6},
    {6, 8},
    {2, 9},
    {9, 11},
    {11, 13},
    {2, 10},
    {10, 12},
    {12, 14},
}};

constexpr std::array<std::uint64_t, 8> kUnsupportedAimWeapons{
    900130826ULL,
    900130823ULL,
    900130827ULL,
    890130846ULL,
    950130817ULL,
    950130818ULL,
    900130825ULL,
    860130824ULL,
};

bool IsFinite(const Vec2& value) {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool IsFinite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool IsValidTransform(const Transform& transform) {
    const Quaternion& rotation = transform.rotation;
    const float normSquared =
        rotation.x * rotation.x + rotation.y * rotation.y +
        rotation.z * rotation.z + rotation.w * rotation.w;
    return std::isfinite(normSquared) && normSquared > 0.25f &&
        normSquared < 2.25f && IsFinite(transform.translation) &&
        IsFinite(transform.scale);
}

native::GeometryPoint ToGeometryPoint(const Vec3& value) {
    return native::GeometryPoint{value.x, value.y, value.z};
}

SemanticTone ToneForVisibility(bool bot,
                               bool visibilityColor,
                               native::GeometryVisibility visibility) {
    if (!visibilityColor) {
        return bot ? SemanticTone::Caution : SemanticTone::Danger;
    }
    switch (visibility) {
        case native::GeometryVisibility::Visible:
            return SemanticTone::Danger;
        case native::GeometryVisibility::Occluded:
            return SemanticTone::Accent;
        case native::GeometryVisibility::Unavailable:
        default:
            return SemanticTone::Muted;
    }
}

bool IsFinite(const CameraView& view) {
    return IsFinite(view.location) && std::isfinite(view.rotation.pitch) &&
           std::isfinite(view.rotation.yaw) && std::isfinite(view.rotation.roll) &&
           std::isfinite(view.fieldOfView) && view.fieldOfView >= 5.0f &&
           view.fieldOfView <= 170.0f;
}

bool IsNonzero(const Vec3& value) {
    return value.x != 0.0f || value.y != 0.0f || value.z != 0.0f;
}

bool ReadElfBytes(void* context,
                  std::uintptr_t address,
                  void* destination,
                  std::size_t size) {
    auto* memory = static_cast<native::MemoryTransport*>(context);
    return memory != nullptr && memory->Read(address, destination, size);
}

bool IsMappedExecutableAddress(pid_t processId, std::uintptr_t address) {
    if (processId <= 0 || address == 0) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", processId);
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char permissions[5]{};
        if (std::sscanf(
                line.c_str(),
                "%llx-%llx %4s",
                &start,
                &end,
                permissions) != 3) {
            continue;
        }
        if (address >= start && address < end) {
            return permissions[2] == 'x';
        }
    }
    return false;
}

bool IsMappedNamedExecutableAddress(pid_t processId,
                                    std::uintptr_t address,
                                    std::string_view expectedName) {
    if (processId <= 0 || address == 0 || expectedName.empty()) return false;
    char path[64]{};
    std::snprintf(path, sizeof(path), "/proc/%d/maps", processId);
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        char permissions[5]{};
        char name[256]{};
        const int fields = std::sscanf(
            line.c_str(),
            "%llx-%llx %4s %*s %*s %*s %255[^\n]",
            &start,
            &end,
            permissions,
            name);
        if (fields < 3 || address < start || address >= end) continue;
        if (fields != 4 || permissions[0] != 'r' ||
            permissions[2] != 'x') {
            return false;
        }
        std::string_view mappedName(name);
        while (!mappedName.empty() &&
               (mappedName.back() == ' ' || mappedName.back() == '\t')) {
            mappedName.remove_suffix(1);
        }
        if (mappedName == expectedName) return true;
        const std::size_t separator = mappedName.find_last_of('/');
        return separator != std::string_view::npos &&
            mappedName.substr(separator + 1) == expectedName;
    }
    return false;
}

bool IsValidPointer(std::uintptr_t address) {
    return address >= kMinimumRemoteAddress && address < kMaximumRemoteAddress &&
           (address & 0x3ULL) == 0;
}

bool IsValidReadAddress(std::uintptr_t address) {
    return address >= kMinimumRemoteAddress && address < kMaximumRemoteAddress;
}

float Dot(const Vec3& left, const Vec3& right) {
    return left.x * right.x + left.y * right.y + left.z * right.z;
}

Vec3 Subtract(const Vec3& left, const Vec3& right) {
    return Vec3{left.x - right.x, left.y - right.y, left.z - right.z};
}

float Length(const Vec3& value) {
    return std::sqrt(Dot(value, value));
}

float HorizontalLength(const Vec3& value) {
    return std::hypot(value.x, value.y);
}

bool IsAimingAtPosition(const Vec3& actorPosition,
                        const Vec3& actorForward,
                        const Vec3& targetPosition) {
    constexpr float kHorizontalGateDegrees = 10.0f;
    constexpr float kVerticalGateDegrees = 40.0f;
    constexpr float kEpsilon = 0.0001f;
    if (!IsFinite(actorPosition) || !IsFinite(actorForward) ||
        !IsFinite(targetPosition)) {
        return false;
    }

    const Vec3 delta = Subtract(targetPosition, actorPosition);
    const float forwardLength = HorizontalLength(actorForward);
    const float horizontalDistance = HorizontalLength(delta);
    if (forwardLength <= kEpsilon || horizontalDistance <= kEpsilon) return false;

    const float cosine =
        (actorForward.x * delta.x + actorForward.y * delta.y) /
        (forwardLength * horizontalDistance);
    const float gateCosine = std::cos(kHorizontalGateDegrees * kPi / 180.0f);
    if (!std::isfinite(cosine) || cosine < gateCosine) return false;

    const float verticalAngle =
        std::atan2(std::fabs(delta.z), horizontalDistance) * 180.0f / kPi;
    return std::isfinite(verticalAngle) && verticalAngle <= kVerticalGateDegrees;
}

void AppendDetail(std::string& destination, std::string_view value) {
    if (value.empty()) return;
    if (!destination.empty()) destination += " | ";
    destination.append(value.data(), value.size());
}

bool WeaponAllowsAim(std::uint64_t weaponId) {
    return weaponId != 0 &&
        std::find(kUnsupportedAimWeapons.begin(), kUnsupportedAimWeapons.end(), weaponId) ==
            kUnsupportedAimWeapons.end();
}

std::size_t WeaponProfileFor(std::uint64_t weaponId) {
    const auto inRange = [weaponId](std::uint64_t first, std::uint64_t last) {
        return weaponId >= first && weaponId <= last;
    };
    if (inRange(18010000001ULL, 18010000100ULL) ||
        inRange(830000000ULL, 839999999ULL)) return 0;
    if (inRange(18020000001ULL, 18020000100ULL) ||
        inRange(840000000ULL, 849999999ULL)) return 1;
    if (inRange(18030000001ULL, 18030000100ULL) ||
        inRange(850000000ULL, 859999999ULL)) return 2;
    if (inRange(18040000001ULL, 18040000100ULL) ||
        inRange(860000000ULL, 869999999ULL)) return 3;
    if (inRange(18050000001ULL, 18050000100ULL) ||
        inRange(870000000ULL, 879999999ULL)) return 4;
    if (inRange(18060000001ULL, 18060000100ULL) ||
        inRange(880000000ULL, 889999999ULL)) return 5;
    if (inRange(18070000001ULL, 18070000100ULL) ||
        inRange(890000000ULL, 899999999ULL)) return 6;
    if (inRange(18150000001ULL, 18150000100ULL) ||
        inRange(970000000ULL, 979999999ULL)) return 7;
    return ui::kWeaponProfileCount - 1;
}

float ProjectileSpeedFor(std::uint64_t weaponId) {
    switch (WeaponProfileFor(weaponId)) {
        case 0: return 88000.0f;
        case 1: return 45000.0f;
        case 2: return 43000.0f;
        case 3: return 85000.0f;
        case 4: return 83500.0f;
        case 5: return 88000.0f;
        case 6: return 37500.0f;
        case 7: return 65000.0f;
        default: return 88000.0f;
    }
}

int EquipmentLevel(std::int32_t definitionId) {
    const std::string digits = std::to_string(
        definitionId < 0 ? -static_cast<std::int64_t>(definitionId) : definitionId);
    if (digits.size() < 7) return 0;
    const std::string_view code(digits.data() + 4, 3);
    if (code == "900") return 1;
    if (code == "899") return 2;
    if (code == "898") return 3;
    if (code == "897") return 4;
    if (code == "896") return 5;
    if (code == "895") return 6;
    return 0;
}

const char* OperatorName(std::int32_t id) {
    switch (id) {
        case 2100654110: return "红狼";
        case 2100654105: return "威龙";
        case 2100654106: return "骇爪";
        case 2100654107: return "蜂医";
        case 2100654109: return "牧羊人";
        case 2100654108: return "露娜";
        case 2100654115: return "乌鲁鲁";
        case 2100654116: return "蛊";
        case 2100654117: return "深蓝";
        case 2100654118: return "无名";
        case 2100654119: return "疾风";
        case 2100654120: return "银翼";
        case 2100654121: return "比特";
        case 2100654122: return "赤枭";
        case 2100654123:
        case 2100654124: return "赤枭小弟";
        case 2100654125: return "蝶";
        case 2100654126: return "回响";
        case 2100654127: return "液氮";
        default: return nullptr;
    }
}

Matrix4 TransformToMatrix(const Transform& transform) {
    Matrix4 matrix{};
    const float x2 = transform.rotation.x + transform.rotation.x;
    const float y2 = transform.rotation.y + transform.rotation.y;
    const float z2 = transform.rotation.z + transform.rotation.z;
    const float xx2 = transform.rotation.x * x2;
    const float yy2 = transform.rotation.y * y2;
    const float zz2 = transform.rotation.z * z2;
    const float yz2 = transform.rotation.y * z2;
    const float wx2 = transform.rotation.w * x2;
    const float xy2 = transform.rotation.x * y2;
    const float wz2 = transform.rotation.w * z2;
    const float xz2 = transform.rotation.x * z2;
    const float wy2 = transform.rotation.w * y2;

    matrix.value[0][0] = (1.0f - (yy2 + zz2)) * transform.scale.x;
    matrix.value[0][1] = (xy2 + wz2) * transform.scale.x;
    matrix.value[0][2] = (xz2 - wy2) * transform.scale.x;
    matrix.value[1][0] = (xy2 - wz2) * transform.scale.y;
    matrix.value[1][1] = (1.0f - (xx2 + zz2)) * transform.scale.y;
    matrix.value[1][2] = (yz2 + wx2) * transform.scale.y;
    matrix.value[2][0] = (xz2 + wy2) * transform.scale.z;
    matrix.value[2][1] = (yz2 - wx2) * transform.scale.z;
    matrix.value[2][2] = (1.0f - (xx2 + yy2)) * transform.scale.z;
    matrix.value[3][0] = transform.translation.x;
    matrix.value[3][1] = transform.translation.y;
    matrix.value[3][2] = transform.translation.z;
    matrix.value[3][3] = 1.0f;
    return matrix;
}

Matrix4 Multiply(const Matrix4& left, const Matrix4& right) {
    Matrix4 result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float value = left.value[row][0] * right.value[0][column];
            for (int index = 1; index < 4; ++index) {
                value = std::fma(
                    left.value[row][index],
                    right.value[index][column],
                    value);
            }
            result.value[row][column] = value;
        }
    }
    return result;
}

Vec3 MatrixTranslation(const Matrix4& matrix) {
    return Vec3{
        matrix.value[3][0],
        matrix.value[3][1],
        matrix.value[3][2],
    };
}

void CameraAxes(const CameraView& view, Vec3& forward, Vec3& right, Vec3& up) {
    const float pitch = view.rotation.pitch * kPi / 180.0f;
    const float yaw = view.rotation.yaw * kPi / 180.0f;
    const float roll = view.rotation.roll * kPi / 180.0f;
    const float sinPitch = std::sin(pitch);
    const float cosPitch = std::cos(pitch);
    const float sinYaw = std::sin(yaw);
    const float cosYaw = std::cos(yaw);
    const float sinRoll = std::sin(roll);
    const float cosRoll = std::cos(roll);

    forward = Vec3{cosPitch * cosYaw, cosPitch * sinYaw, sinPitch};
    right = Vec3{
        sinRoll * sinPitch * cosYaw - cosRoll * sinYaw,
        sinRoll * sinPitch * sinYaw + cosRoll * cosYaw,
        -sinRoll * cosPitch,
    };
    up = Vec3{
        -(cosRoll * sinPitch * cosYaw + sinRoll * sinYaw),
        cosYaw * sinRoll - cosRoll * sinPitch * sinYaw,
        cosRoll * cosPitch,
    };
}

struct CameraPoint {
    float side = 0.0f;
    float vertical = 0.0f;
    float forward = 0.0f;
};

native::ProjectionView ToProjectionView(const CameraView& view) {
    return native::ProjectionView{
        native::ProjectionPoint{
            view.location.x, view.location.y, view.location.z},
        native::ProjectionRotation{
            view.rotation.pitch,
            view.rotation.yaw,
            view.rotation.roll,
        },
        view.fieldOfView,
    };
}

native::PreparedProjection PrepareProjection(
    const CameraView& view,
    int screenWidth,
    int screenHeight) {
    return native::PrepareProjection(
        ToProjectionView(view), screenWidth, screenHeight);
}

CameraPoint ToCameraSpace(
    const Vec3& world,
    const native::PreparedProjection& prepared) {
    const native::CameraSpacePoint point = native::ToCameraSpace(
        native::ProjectionPoint{world.x, world.y, world.z},
        prepared);
    return CameraPoint{point.side, point.vertical, point.forward};
}

bool ProjectToScreen(const Vec3& world,
                     const native::PreparedProjection& prepared,
                     Vec2& screen,
                     CameraPoint* cameraPoint = nullptr) {
    platform::PerformanceTraceScope trace(
        platform::PerformancePhase::Projection, 64);
    const native::ScreenProjection projected = native::ProjectWorldPoint(
        native::ProjectionPoint{world.x, world.y, world.z},
        prepared);
    if (cameraPoint != nullptr) {
        *cameraPoint = CameraPoint{
            projected.camera.side,
            projected.camera.vertical,
            projected.camera.forward,
        };
    }
    if (!projected.valid) return false;
    screen = Vec2{projected.x, projected.y};
    return true;
}

bool ProjectToScreenLoose(const Vec3& world,
                          const native::PreparedProjection& prepared,
                          Vec2& screen,
                          CameraPoint* cameraPoint = nullptr) {
    platform::PerformanceTraceScope trace(
        platform::PerformancePhase::Projection, 64);
    const native::ScreenProjection projected =
        native::ProjectWorldPointLoose(
            native::ProjectionPoint{world.x, world.y, world.z},
            prepared);
    if (cameraPoint != nullptr) {
        *cameraPoint = CameraPoint{
            projected.camera.side,
            projected.camera.vertical,
            projected.camera.forward,
        };
    }
    if (!projected.valid) return false;
    screen = Vec2{projected.x, projected.y};
    return true;
}

bool IsInsideScreen(const Vec2& point, int width, int height, float margin = 0.0f) {
    return IsFinite(point) && point.x >= -margin && point.y >= -margin &&
           point.x <= static_cast<float>(width) + margin &&
           point.y <= static_cast<float>(height) + margin;
}

std::string FormatDistance(float distanceMeters) {
    if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) return {};
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0fm", distanceMeters);
    return buffer;
}

bool IsRangeTargetClass(std::string_view name) {
    return name.find("RangeTargetCharacter") != std::string_view::npos ||
           name.find("RangeTargeCharacter") != std::string_view::npos;
}

bool IsCharacterClass(std::string_view name) {
    if (name == "NC_BP_DFMCharacter_C" ||
        name == "NC_BP_DFMCharacter_TutorialPlayerAi_C" ||
        IsRangeTargetClass(name)) {
        return true;
    }
    return name.rfind("NC_BP_DFMCharacter_AI", 0) == 0 ||
           name.rfind("NC_BP_DFMAICharacter", 0) == 0 ||
           name.find("DFMCharacter") != std::string_view::npos;
}

bool IsBotClass(std::string_view name) {
    return name == "NC_BP_DFMCharacter_TutorialPlayerAi_C" ||
           name.rfind("NC_BP_DFMCharacter_AI", 0) == 0 ||
           name.rfind("NC_BP_DFMAICharacter", 0) == 0 ||
           IsRangeTargetClass(name);
}

struct FNameClassTraits {
    bool character = false;
    bool rangeTarget = false;
    bool bot = false;
};

FNameClassTraits ClassifyFName(std::string_view name) {
    return FNameClassTraits{
        IsCharacterClass(name),
        IsRangeTargetClass(name),
        IsBotClass(name),
    };
}

bool IsPickupClass(std::string_view name) {
    return name.find("Pickup_C") != std::string_view::npos ||
        name.find("PickUp_") != std::string_view::npos ||
        name.find("InventoryPickup") != std::string_view::npos ||
        name.find("InvntoryPickup") != std::string_view::npos;
}

bool IsDeadBodyClass(std::string_view name) {
    return name.find("BP_Inventory_DeadBody_C") != std::string_view::npos;
}

bool IsExtractionClass(std::string_view name) {
    return name.find("BP_FXProxy_Exit_C") != std::string_view::npos;
}

SemanticTone ToneForRarity(int rarity) {
    if (rarity >= 6) return SemanticTone::Danger;
    if (rarity >= 5) return SemanticTone::Caution;
    if (rarity >= 4) return SemanticTone::Accent;
    if (rarity >= 3) return SemanticTone::Ally;
    return rarity >= 2 ? SemanticTone::Accent : SemanticTone::Neutral;
}

bool IsKnownContainerId(std::int32_t id) {
    switch (id) {
        case 902: case 905: case 906: case 909: case 913: case 914:
        case 916: case 920: case 926: case 927: case 930: case 933:
        case 934: case 936: case 937: case 938: case 940: case 943:
        case 945: case 946: case 947: case 948: case 950: case 956:
        case 957: case 962:
            return true;
        default:
            return false;
    }
}

const char* ContainerName(std::int32_t id) {
    switch (id) {
        case 902: case 945: return "快递箱";
        case 905: return "医疗包";
        case 909: return "电脑机箱";
        case 913: return "垃圾箱";
        case 914: return "抽屉柜";
        case 916: case 947: return "衣物";
        case 920: return "保险箱";
        case 926: return "大型武器箱";
        case 927: return "旅行包";
        case 930: return "鸟窝";
        case 934: return "高级旅行箱";
        case 936: return "工具柜";
        case 937: return "服务器";
        case 938: return "手提箱";
        case 940: return "医疗物资堆";
        case 943: return "航空储物箱";
        case 946: return "藏匿物";
        case 948: return "野外物资箱";
        case 950: return "小保险箱";
        case 957: return "高级储物箱";
        default: return "容器";
    }
}

std::optional<ui::ContainerKind> ContainerKindForId(std::int32_t id) {
    using Kind = ui::ContainerKind;
    switch (id) {
        case 902: case 945: return Kind::ParcelBox;
        case 905: return Kind::MedicalBag;
        case 909: return Kind::ComputerCase;
        case 913: return Kind::TrashCan;
        case 914: return Kind::Drawer;
        case 916: case 947: return Kind::Clothing;
        case 920: return Kind::Safe;
        case 926: return Kind::LargeWeaponCrate;
        case 927: return Kind::TravelBag;
        case 930: return Kind::BirdNest;
        case 934: return Kind::PremiumSuitcase;
        case 936: return Kind::ToolCabinet;
        case 937: return Kind::Server;
        case 938: return Kind::Suitcase;
        case 940: return Kind::MedicalSupplyPile;
        case 943: return Kind::AviationStorage;
        case 946: return Kind::Stash;
        case 948: return Kind::FieldSupplyCrate;
        case 950: return Kind::SmallSafe;
        case 957: return Kind::AdvancedStorage;
        default: return std::nullopt;
    }
}

std::string Utf16ToUtf8(const std::u16string& value) {
    std::string output;
    output.reserve(value.size() * 3);
    for (std::size_t index = 0; index < value.size(); ++index) {
        std::uint32_t codePoint = value[index];
        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU && index + 1 < value.size()) {
            const std::uint32_t low = value[index + 1];
            if (low >= 0xDC00U && low <= 0xDFFFU) {
                codePoint = 0x10000U + ((codePoint - 0xD800U) << 10U) + (low - 0xDC00U);
                ++index;
            }
        }
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else if (codePoint <= 0x10FFFFU) {
            output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }
    return output;
}

}  // namespace

class NativeGameBackend final : public GameBackend {
public:
    ~NativeGameBackend() override {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (Close()) break;
        }
    }

    bool Open(const RuntimeOptions& options,
              RuntimeProbe& probe,
        std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!CloseLocked()) {
            probe = RuntimeProbe{};
            SetRuntimeFailure(
                probe, RuntimeError::BackendCloseFailed, -EBUSY);
            error.clear();
            return false;
        }
        probe = RuntimeProbe{};
        error.clear();

        const int inputMode = static_cast<int>(options.inputMode);
        if (options.gameVersionIndex < 0 ||
            options.gameVersionIndex >= static_cast<int>(kVersionLayouts.size()) ||
            !native::IsValidMemoryTransportMode(options.driverIndex) ||
            inputMode < static_cast<int>(ui::AimInputMode::ReadOnly) ||
            inputMode > static_cast<int>(ui::AimInputMode::KernelGyroscope) ||
            options.screenWidth <= 1 || options.screenHeight <= 1) {
            SetRuntimeFailure(
                probe, RuntimeError::InvalidConfiguration, -EINVAL);
            error = "运行参数无效";
            return false;
        }
        options_ = options;
        algorithmPositionConfig_ = options.algorithmPosition;
        customItemPath_ = options.programDirectory.empty()
            ? std::string("自定义物资.txt")
            : options.programDirectory + "/自定义物资.txt";
        customItems_.Load(customItemPath_);
        layout_ = kVersionLayouts[static_cast<std::size_t>(options.gameVersionIndex)];
        processId_ = native::FindProcessId(layout_.processName);
        if (processId_ <= 0) {
            SetRuntimeFailure(
                probe, RuntimeError::TargetProcessNotFound, -ESRCH);
            error = "未找到目标游戏进程";
            CloseLocked();
            return false;
        }

        memory_ = std::make_unique<native::MemoryTransport>();
        RuntimeDiagnostic memoryDiagnostic{};
        if (!memory_->Open(
                options.driverIndex,
                processId_,
                layout_.processName,
                memoryDiagnostic,
                error)) {
            SetRuntimeFailure(
                probe,
                memoryDiagnostic.error != RuntimeError::None
                    ? memoryDiagnostic.error
                    : RuntimeError::BackendOpenFailed,
                memoryDiagnostic.systemError);
            CloseLocked();
            return false;
        }

        moduleBase_ = memory_->ModuleBase("libUE4.so");
        if (!IsValidPointer(moduleBase_)) {
            moduleBase_ = native::FindMappedModuleBase(processId_, "libUE4.so");
        }
        if (!IsValidPointer(moduleBase_)) {
            SetRuntimeFailure(
                probe, RuntimeError::ModuleBaseUnavailable, -ENOENT);
            error = "无法定位游戏模块";
            CloseLocked();
            return false;
        }

        std::array<std::uint8_t, 4> signature{};
        if (!memory_->Read(moduleBase_, signature.data(), signature.size()) ||
            signature[0] != 0x7FU || signature[1] != 'E' ||
            signature[2] != 'L' || signature[3] != 'F') {
            SetRuntimeFailure(
                probe, RuntimeError::ModuleReadFailed, -EIO);
            error = "所选内存通道无法读取游戏模块";
            CloseLocked();
            return false;
        }

        moduleBuildId_.clear();
        const bool moduleBuildIdReady =
            native::ReadRemoteElfBuildId(
                moduleBase_,
                &ReadElfBytes,
                memory_.get(),
                moduleBuildId_);
        if (options.cloudLayout != nullptr ||
            options.coordinateDecrypt2Layout != nullptr) {
            if (!moduleBuildIdReady) {
                probe.failureKind =
                    RuntimeFailureKind::CloudLayoutRejected;
                SetRuntimeFailure(
                    probe, RuntimeError::CloudBuildIdMismatch);
                error = "cloud layout build id mismatch";
                CloseLocked();
                return false;
            }
        }
        if (options.cloudLayout != nullptr) {
            const auto cloudLayout = native::BuildRuntimeLayoutOverride(
                options.cloudLayout.get(), layout_.processName,
                "libUE4.so", moduleBuildId_);
            if (!cloudLayout.has_value() ||
                !memory_->ConfigureCoordinateReplay(
                    cloudLayout->coordinateTransport) ||
                !coordinatePoolRuntime_.Configure(
                    cloudLayout->coordinatePool)) {
                probe.failureKind =
                    RuntimeFailureKind::CloudLayoutRejected;
                SetRuntimeFailure(
                    probe, RuntimeError::CloudLayoutInvalid, -EINVAL);
                error = "cloud layout validation failed";
                CloseLocked();
                return false;
            }
            layout_.namePoolOffset = cloudLayout->namePoolOffset;
            layout_.worldOffset = cloudLayout->worldOffset;
            layout_.geometryInstancePointerOffsets =
                cloudLayout->geometryInstancePointerOffsets;
            layout_.actorRecordLayout = cloudLayout->actorRecords;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            layout_.trackingMatrixRootOffset =
                cloudLayout->trackingMatrixRootOffset;
#endif
            layout_.componentPositionFlagOffset =
                cloudLayout->componentPositionFlagOffset;
            algorithmPositionConfig_ =
                cloudLayout->coordinateReplayEntryOffset != 0
                ? native::AlgorithmPositionRuntimeConfig{
                      cloudLayout->coordinateReplayEntryOffset,
                      0,
                  }
                : native::AlgorithmPositionRuntimeConfig{};
        }
        if (options.coordinateDecrypt2Layout != nullptr) {
            const auth::CoordinatePoolCloudLayoutDocument& decrypt2 =
                *options.coordinateDecrypt2Layout;
            if (decrypt2.identity.packageName != layout_.processName ||
                decrypt2.identity.moduleName != "libUE4.so" ||
                decrypt2.identity.buildId != moduleBuildId_ ||
                !decrypt2.coordinatePool.IsValid() ||
                !coordinateDecrypt2Runtime_.Configure(
                    decrypt2.coordinatePool)) {
                probe.failureKind =
                    RuntimeFailureKind::CloudLayoutRejected;
                SetRuntimeFailure(
                    probe, RuntimeError::CloudLayoutInvalid, -EINVAL);
                error = "decrypt2 cloud layout validation failed";
                CloseLocked();
                return false;
            }
        }

        RefreshAlgorithmEntry(true);

        const auto algorithmContextReadAt =
            std::chrono::steady_clock::now();
        algorithmExecutionContextReady_ =
            memory_->ReadProcessExecutionContext(algorithmExecutionContext_);
        algorithmExecutionContextReady_ =
            algorithmExecutionContextReady_ &&
            algorithmExecutionContext_.IsUsable();
        if (algorithmExecutionContextReady_) {
            algorithmExecutionContextRefreshPolicy_.MarkSucceeded(
                CurrentAlgorithmExecutionContextRefreshKey(false),
                algorithmContextReadAt);
        } else {
            algorithmExecutionContextRefreshPolicy_.MarkFailed();
        }

        if (!aimController_.Start(options.inputMode)) {
            SetRuntimeFailure(
                probe, RuntimeError::InputChannelStartFailed);
            error = "输入通道初始化失败";
            CloseLocked();
            return false;
        }
        opened_ = true;
        aimController_.SetEnabled(aimEnabled_.load(std::memory_order_acquire));
        probe.processId = processId_;
        probe.baseReady = true;
        probe.customItemCount = customItems_.Size();
        UpdateCoordinateProbe(probe);
        return true;
    }

    bool Close() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        return CloseLocked();
    }

    bool ReadFrame(const FeatureSettings& settings,
                   GameFrame& frame,
                   RuntimeProbe& probe,
                   std::string& error) override {
        std::lock_guard<std::mutex> lock(mutex_);
        platform::PerformanceTraceScope backendFrameTrace(
            platform::PerformancePhase::BackendFrame);
        const std::uint64_t sequence = frame.sequence;
        frame = GameFrame{};
        frame.sequence = sequence;
        coordinateTraceFrame_ = sequence;
        if (IsCoordinateTraceEnabled()) {
            coordinateTraceRecords_.clear();
        }
        error.clear();
        probe.runtimeError = RuntimeError::None;
        probe.runtimeSystemError = 0;

        if (!opened_ || memory_ == nullptr || !memory_->IsOpen()) {
            SetRuntimeFailure(
                probe, RuntimeError::BackendUnavailable, -ENODEV);
            error = "游戏后端尚未打开";
            return false;
        }
        probe.processId = processId_;
        probe.baseReady = IsValidPointer(moduleBase_);
        probe.customItemCount = customItems_.Size();
        if (!native::IsProcessAlive(processId_)) {
            probe = RuntimeProbe{};
            SetRuntimeFailure(
                probe, RuntimeError::TargetProcessExited, -ESRCH);
            error = "目标游戏进程已结束";
            return false;
        }
        {
            platform::PerformanceTraceScope geometryUpdateTrace(
                platform::PerformancePhase::GeometryUpdate);
            UpdateGeometryRuntime(settings);
        }

        const bool requestedHardwareBreakpoint =
            settings.visual.hardwareBreakpointDecrypt;
        const bool requestedCoordinateReplay =
            settings.visual.coordinateDecrypt &&
            !requestedHardwareBreakpoint;
        const bool hardwareBreakpointRequestChanged =
            hardwareBreakpointRequested_ != requestedHardwareBreakpoint;
#if 0
        if (hardwareBreakpointRequestChanged) {
            hardwareBreakpointRetryAfter_ = {};
            hardwareBreakpointFailure_ = {};
        }
        const auto hardwareBreakpointNow =
            std::chrono::steady_clock::now();
        if (requestedHardwareBreakpoint &&
            !hardwareBreakpointRuntime_.IsActive() &&
            (hardwareBreakpointRetryAfter_ ==
                 std::chrono::steady_clock::time_point{} ||
             hardwareBreakpointNow >= hardwareBreakpointRetryAfter_)) {
            if (StartHardwareBreakpointRuntime()) {
                hardwareBreakpointRetryAfter_ = {};
            } else {
                hardwareBreakpointRetryAfter_ =
                    hardwareBreakpointNow +
                    std::chrono::seconds(1);
            }
        } else if (!requestedHardwareBreakpoint &&
                   hardwareBreakpointRuntime_.IsActive()) {
            static_cast<void>(StopHardwareBreakpointRuntime());
        }
#endif
        hardwareBreakpointRequested_ = requestedHardwareBreakpoint;

        const bool coordinateRequestChanged =
            algorithmPositionRequested_ != requestedCoordinateReplay;
        const std::uint32_t requestedCoordinateDecrypt2Index =
            static_cast<std::uint32_t>(std::clamp(
                settings.visual.coordinateDecrypt2Index,
                0,
                static_cast<int>(
                    native::kCoordinatePoolMaximumDecryptIndexOffset)));
        const bool coordinateDecrypt2IndexChanged =
            requestedHardwareBreakpoint &&
            coordinateDecrypt2Index_ != requestedCoordinateDecrypt2Index;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        const bool requestedAlgorithmCoordinate =
            settings.visual.algorithmDecrypt ||
            IsAlgorithmCoordinateValidationRequested();
        const bool algorithmCoordinateRequestChanged =
            algorithmDecryptRequested_ != requestedAlgorithmCoordinate;
        algorithmDecryptRequested_ = requestedAlgorithmCoordinate;
#endif
        algorithmPositionRequested_ = requestedCoordinateReplay;
        coordinateDecrypt2Index_ = requestedCoordinateDecrypt2Index;
        if (coordinateRequestChanged ||
            hardwareBreakpointRequestChanged) {
            algorithmReplayBackoffPolicy_.Reset();
            algorithmReplayPagePolicy_.Invalidate();
            algorithmFailureSince_ = {};
            coordinatePoolRuntime_.Reset();
            coordinateDecrypt2Runtime_.Reset();
            coordinatePoolReady_ = false;
            coordinatePoolFrame_ = 0;
            coordinatePoolBridge_ = 0;
            coordinatePoolContext_ = 0;
            coordinatePoolEntry_ = 0;
        }
        bool coordinateSourceChanged = coordinateRequestChanged ||
            hardwareBreakpointRequestChanged ||
            coordinateDecrypt2IndexChanged;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        coordinateSourceChanged =
            coordinateSourceChanged || algorithmCoordinateRequestChanged;
#endif
        if (coordinateSourceChanged) {
            characterPositions_.Clear();
            positionCache_.clear();
            decodedPositionCache_.clear();
            decodedPositionPending_.clear();
            boneCache_.clear();
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
            algorithmCoordinateSnapshot_.clear();
            algorithmCoordinateTableReady_ = false;
            algorithmCoordinateTableDiagnostic_ = {};
            runtimeCoordinateCodec_.Reset();
            algorithmCoordinateRuntimeReady_ = false;
            algorithmCoordinateRuntimeDiagnostic_ = {};
            algorithmCoordinateRefreshCount_ = 0;
            algorithmCoordinateResolveAttemptCount_ = 0;
            algorithmCoordinateResolveSuccessCount_ = 0;
            algorithmCoordinateAttemptCount_ = 0;
            algorithmCoordinateSuccessCount_ = 0;
            algorithmCoordinateObjectAttemptCount_ = 0;
            algorithmCoordinateObjectSuccessCount_ = 0;
            algorithmCoordinateTableAttemptCount_ = 0;
            algorithmCoordinateTableSuccessCount_ = 0;
            algorithmCoordinateFallbackCount_ = 0;
#endif
        }
        algorithmFrameAttemptCount_ = 0;
        algorithmFrameSuccessCount_ = 0;
        algorithmFrameOutputError_ = CoordinateDecryptError::None;
        algorithmFrameFailure_ = {};
        algorithmFrameAgedDecodedFailure_ = false;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        algorithmCoordinateFrameAttemptCount_ = 0;
        algorithmCoordinateFrameSuccessCount_ = 0;
        algorithmCoordinateFrameFailure_ = {};
        algorithmCoordinateFrameSuccess_ = {};
        algorithmCoordinateFrameRuntimeFailure_ = {};
        algorithmCoordinateFrameRuntimeSuccess_ = {};
        algorithmCoordinateFrameSource_ =
            native::AlgorithmCoordinateSource::None;
        algorithmCoordinateObjectCache_.clear();
#endif
        const bool coordinatePoolSelected =
            UsesAnyCoordinatePoolRuntime();
        if (!coordinatePoolSelected ||
            ForcedCoordinateProbeComponent() != 0) {
            platform::PerformanceTraceScope entryRefreshTrace(
                platform::PerformancePhase::CoordinateEntryRefresh);
            RefreshAlgorithmEntry(false);
        }
        const auto algorithmFrameNow = std::chrono::steady_clock::now();
        algorithmReplayAllowedThisFrame_ =
            !algorithmPositionRequested_ ||
            coordinatePoolSelected ||
            algorithmReplayBackoffPolicy_.BeginFrame(algorithmFrameNow);
        if (algorithmPositionRequested_ &&
            algorithmReplayAllowedThisFrame_ && !coordinatePoolSelected) {
            algorithmReplayPagePolicy_.BeginFrame();
        }
        if (!algorithmPositionRequested_ ||
            algorithmReplayAllowedThisFrame_) {
            platform::PerformanceTraceScope contextRefreshTrace(
                platform::PerformancePhase::CoordinateContextRefresh);
            RefreshAlgorithmExecutionContext();
        }
        const bool infrastructureProbeFailed =
            coordinatePoolSelected &&
            algorithmReplayAllowedThisFrame_ &&
            (!algorithmExecutionContextReady_ || !coordinatePoolReady_);
        if (infrastructureProbeFailed) {
            algorithmReplayBackoffPolicy_.ObserveFrame(
                1, 0, algorithmFrameNow);
        }
        RunForcedCoordinateProbe();
        UpdateCoordinateProbe(probe);
        const native::PositionReadMode positionMode =
            native::ResolvePositionReadMode(
                algorithmPositionRequested_);
        if (positionMode != positionReadMode_) {
            characterPositions_.Clear();
            decodedPositionCache_.clear();
            decodedPositionPending_.clear();
            algorithmPositionRuntime_.Invalidate();
            algorithmReplayPagePolicy_.Invalidate();
            algorithmExecutionContextRefreshPolicy_.Invalidate();
            boneCache_.clear();
            positionReadMode_ = positionMode;
        }
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        RefreshAlgorithmCoordinateSources();
#endif

        FrameContext context{};
        RuntimeDiagnostic frameDiagnostic{};
        bool frameContextReady = false;
        {
            platform::PerformanceTraceScope frameContextTrace(
                platform::PerformancePhase::FrameContext);
            frameContextReady = BuildFrameContext(
                context,
                frame.sequence,
                settings.visual.battlefieldMode,
                positionMode,
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                IsProjectileTrackingRequested(
                    settings.aim.trajectoryTracking),
#endif
                settings.visual.antiFlicker,
                frameDiagnostic,
                error);
        }
        if (!frameContextReady) {
            if (geometryRuntime_.IsRunning()) {
                geometryValidationPending_ = true;
            }
            SetRuntimeFailure(
                probe,
                frameDiagnostic.error != RuntimeError::None
                    ? frameDiagnostic.error
                    : RuntimeError::FrameDataUnavailable,
                frameDiagnostic.systemError);
            InvalidateActorRecordSnapshot();
            ResetWorldState();
            UpdateCoordinateProbe(probe);
            ApplyCoordinateDiagnostic(error, probe);
            return true;
        }

        if (world_ != context.world) {
            ResetWorldState();
            world_ = context.world;
        }
        if (geometryWorld_ != context.world) {
            const bool refreshExistingWorld =
                native::ShouldRequestGeometryRefresh(
                    geometryWorld_, context.world);
            geometryWorld_ = context.world;
            if (refreshExistingWorld) {
                RequestGeometryRefresh();
            }
            geometryValidationPending_ = false;
        } else if (geometryValidationPending_) {
            geometryValidationPending_ = false;
            if (geometryRuntime_.IsRunning()) {
                geometrySnapshotReady_ = false;
                geometryValidationEpoch_ =
                    geometryRuntime_.RequestValidation();
            }
        }
#if 0
        if (hardwareBreakpointRequested_ &&
            hardwareBreakpointRuntime_.IsActive()) {
            if (hardwareBreakpointRuntime_.Poll(context.world) &&
                hardwareBreakpointRuntime_.PublishedCoordinateCount() !=
                    0) {
                hardwareBreakpointFailure_ = {};
            } else if (
                hardwareBreakpointRuntime_.AcceptedSampleCount() == 0) {
                hardwareBreakpointFailure_.error =
                    CoordinateDecryptError::PoolPointerStateInvalid;
            } else if (
                hardwareBreakpointRuntime_.RecordsBase() == 0) {
                hardwareBreakpointFailure_.error =
                    CoordinateDecryptError::PoolPointerValueInvalid;
            } else {
                hardwareBreakpointFailure_.error =
                    CoordinateDecryptError::PositionReadFailed;
            }
        }
#endif
        RefreshWorldObjectCache(context, settings);
        if (settings.visual.debugInfo) {
            const std::shared_ptr<const native::GeometrySnapshot> snapshot =
                geometryRuntime_.GetSnapshot();
            frame.geometryAvailable = geometrySnapshotReady_ &&
                snapshot != nullptr && snapshot->available;
            if (frame.geometryAvailable) {
                frame.geometryMeshCount =
                    snapshot->staticMeshes.size() +
                    snapshot->dynamicMeshes.size();
                frame.geometryTriangleCount = snapshot->triangleCount;
                frame.geometryGeneration = snapshot->generation;
            }
        }

        if (!settings.visual.antiFlicker) boneCache_.clear();

        const bool aimWarningEnabled = settings.visual.enabled &&
            (settings.visual.aimWarning || settings.visual.aimWarningRay) &&
            !(context.warfare || settings.visual.battlefieldMode);
        if (!aimWarningEnabled) aimWarningStates_.clear();
        const auto warningTime = std::chrono::steady_clock::now();

        if (settings.visual.crosshair && settings.visual.enabled) {
            CrosshairVisual crosshair{};
            crosshair.center = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                static_cast<float>(options_.screenHeight) * 0.5f);
            crosshair.gap = std::max(2.0f, settings.visual.crosshairSize * 0.25f);
            crosshair.armLength = settings.visual.crosshairSize;
            crosshair.thickness = settings.visual.crosshairThickness;
            crosshair.tone = SemanticTone::Accent;
            frame.crosshair = crosshair;
        }

        float radarForwardX = 0.0f;
        float radarForwardY = 0.0f;
        float radarRightX = 0.0f;
        float radarRightY = 0.0f;
        if (settings.radar.overlay) {
            RadarVisual radar{};
            const float size = std::clamp(settings.radar.overlaySize, 90.0f, 800.0f);
            radar.center = ImVec2(
                settings.radar.overlayX + size * 0.5f,
                settings.radar.overlayY + size * 0.5f);
            radar.radius = size * 0.5f;
            radar.maxDistanceMeters = std::max(1.0f, settings.radar.overlayRangeMeters);
            radar.viewHeadingRadians = context.view.rotation.yaw * kPi / 180.0f;
            radarForwardX = std::cos(radar.viewHeadingRadians);
            radarForwardY = std::sin(radar.viewHeadingRadians);
            radarRightX = -radarForwardY;
            radarRightY = radarForwardX;
            radar.showSelf = settings.radar.showSelf;
            frame.radar = std::move(radar);
        }

        bool hudBigMapActive = false;
        bool hudMiniMapActive = false;
        if (settings.radar.miniMap || settings.radar.bigMap) {
            auto readBytes = [this](std::uintptr_t address,
                                    void* destination,
                                    std::size_t size) {
                return memory_ != nullptr && IsValidReadAddress(address) &&
                    size <= kMaximumRemoteAddress - address &&
                    memory_->Read(address, destination, size);
            };
            auto resolveName = [this, &context](std::uint32_t index) {
                return DecodeName(static_cast<std::int32_t>(index), context.namePool);
            };
            native::HudMapReader::Refresh(
                context.localController, hudMapCache_, readBytes, resolveName);
            hudBigMapActive = settings.radar.bigMap &&
                hudMapCache_.bigMapWidget != 0 && hudMapCache_.bigMapVisible;
            hudMiniMapActive = settings.radar.miniMap &&
                hudMapCache_.miniMapWidget != 0 && !hudMapCache_.bigMapVisible;
            if (hudBigMapActive || hudMiniMapActive) {
                HudMapVisual map{};
                map.markerSize = std::clamp(settings.radar.mapDotSize, 0.0f, 80.0f);
                map.fontSize = std::clamp(settings.radar.mapFontSize, 0.0f, 80.0f);
                if (settings.radar.showSelf) {
                    const float heading = context.view.rotation.yaw * kPi / 180.0f;
                    native::HudMapProjection projected{};
                    if (hudBigMapActive) {
                        projected = native::ProjectBigMap(
                            hudMapCache_,
                            context.localPosition.x,
                            context.localPosition.y,
                            heading,
                            static_cast<float>(options_.screenWidth),
                            static_cast<float>(options_.screenHeight),
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            map.markerSize);
                    } else if (hudMiniMapActive) {
                        projected = native::ProjectMiniMap(
                            hudMapCache_,
                            context.localPosition.x,
                            context.localPosition.y,
                            context.localPosition.z,
                            context.localPosition.x,
                            context.localPosition.y,
                            context.localPosition.z,
                            heading,
                            context.mapBuildId >= 1900 && context.mapBuildId <= 2000,
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            map.markerSize);
                    }
                    if (projected.visible) {
                        HudMapMarker marker{};
                        marker.position = ImVec2(projected.marker.x, projected.marker.y);
                        marker.directionEnd = ImVec2(
                            projected.directionEnd.x, projected.directionEnd.y);
                        marker.label = "自";
                        marker.kind = RadarBlipKind::Self;
                        marker.tone = SemanticTone::Accent;
                        marker.drawDirection = true;
                        map.markers.push_back(std::move(marker));
                    }
                }
                frame.hudMap = std::move(map);
            }
        } else {
            hudMapCache_.Clear();
        }

        const aim::AimModeActivation aimModes =
            aim::ResolveAimModeActivation(
                aimEnabled_.load(std::memory_order_acquire),
                WeaponAllowsAim(context.weaponId),
                settings.aim.enabled,
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                settings.aim.trajectoryTracking);
#else
                false);
#endif
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        const bool recoveredTrackingActive = aimModes.tracking;
#endif
        const bool aimActive = aimModes.Any();
        const ui::AimTuning aimTuning = ResolveAimTuning(
            settings.aim, context.weaponId);
        std::vector<AimCandidate> aimCandidates;
        if (aimActive) aimCandidates.reserve(64);
        if (settings.visual.enabled && settings.visual.modelGeometry) {
            BuildModelGeometry(
                context.view, context.preparedProjection, frame);
        }

        std::vector<RuntimeActorRecord>& actorRecords =
            actorRecordSnapshot_.records;
        platform::RecordPerformanceCount(
            platform::PerformanceCounter::ActorRecords,
            actorRecords.size());
        std::unordered_set<std::uintptr_t> seenThreats;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        std::unordered_set<std::uintptr_t> seenAimActors;
#endif
        if (actorRecords.empty()) {
            const RuntimeError actorFailure =
                UsesAnyCoordinatePoolRuntime()
                ? (actorRecordSnapshot_.decodedRecordArrayLocated
                    ? RuntimeError::DecodedActorRecordsUnavailable
                    : RuntimeError::DecodedActorSourceUnavailable)
                : RuntimeError::ActorListUnavailable;
            SetRuntimeFailure(
                probe, actorFailure);
            error = "数据链等待：人物列表暂不可读";
            RefreshCameraView(context, frame.sequence);
            AppendWorldObjectCache(settings, context, frame);
            FinalizeThreatSignals(settings, frame);
            PublishAimFrame(settings.aim, aimTuning, context, aimCandidates, frame);
            PruneThreatState(seenThreats);
            const auto pruneTime = std::chrono::steady_clock::now();
            PruneBoneCache(pruneTime);
            PrunePositionCache(pruneTime);
            return true;
        }

        frame.players.reserve(std::min<std::size_t>(actorRecords.size(), 256));
        std::vector<PlayerProjectionSource> playerProjectionSources;
        playerProjectionSources.reserve(
            std::min<std::size_t>(actorRecords.size(), 256));
        std::size_t validActorCount = 0;
        std::size_t decodedNameCount = 0;
        std::size_t characterActorCount = 0;
        std::array<
            std::size_t,
            static_cast<std::size_t>(BoneFrameReadStatus::Count)>
            botBoneStatusCounts{};
        std::array<
            std::size_t,
            static_cast<std::size_t>(BoneFrameReadStatus::Count)>
            playerBoneStatusCounts{};
        std::string boneFailureClass;
        float boneFailureDistance = 0.0f;
        float boneFailureHealth = 0.0f;
        bool boneFailureBot = false;
        bool boneFailureResolver = false;
        bool boneFailureEncrypted = false;
        const auto recordBoneStatus =
            [&botBoneStatusCounts, &playerBoneStatusCounts](
                bool bot, BoneFrameReadStatus status) {
                const std::size_t index = static_cast<std::size_t>(status);
                if (index >= botBoneStatusCounts.size()) return;
                (bot ? botBoneStatusCounts : playerBoneStatusCounts)[index]++;
            };
        platform::PerformanceTraceScope actorLoopTrace(
            platform::PerformancePhase::ActorLoop);
        for (RuntimeActorRecord& actorRecord : actorRecords) {
            const std::uintptr_t actor = actorRecord.actor;
            if (!IsValidPointer(actor) || actor == context.localPawn) continue;
            ++validActorCount;

#if LENGJING_ENABLE_PROJECTILE_TRACKING
            std::uintptr_t actorClass = 0;
            std::uintptr_t trackingClassOffset = 0;
            const bool trackingClassReadable = recoveredTrackingActive &&
                actorRecord.resolverRecord &&
                ReadValue(actor, actorClass) && actorClass >= moduleBase_;
            if (trackingClassReadable) {
                trackingClassOffset = actorClass - moduleBase_;
            }
            const bool trackingPlayerClass = trackingClassReadable &&
                std::find(
                    kTrackingPlayerClassOffsets.begin(),
                    kTrackingPlayerClassOffsets.end(),
                    trackingClassOffset) != kTrackingPlayerClassOffsets.end();
            const bool trackingRangeTarget = trackingClassOffset ==
                0x1A3D6A20ULL;
#endif

            std::int32_t nameIndex = -1;
            const bool nameIndexAvailable =
                ReadValue(actor + 0x1C, nameIndex) && nameIndex >= 0;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            if (!nameIndexAvailable && !trackingPlayerClass) continue;
#else
            if (!nameIndexAvailable) continue;
#endif
            const std::string className = nameIndexAvailable
                ? DecodeName(nameIndex, context.namePool)
                : std::string{};
            if (!className.empty()) {
                ++decodedNameCount;
            }
            FNameClassTraits classTraits{};
            {
                platform::PerformanceTraceScope classificationTrace(
                    platform::PerformancePhase::ActorClassification, 64);
                classTraits = ResolveClassTraits(nameIndex, className);
            }
            bool character = classTraits.character;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            character = character || trackingPlayerClass;
#endif
            if (!character) {
                float compatibilityMarker = 0.0f;
                character = ReadValue(actor + 0x47C, compatibilityMarker) &&
                            compatibilityMarker == 40.0f;
            }
            if (!character) {
                if (settings.visual.enabled &&
                    (settings.visual.throwableWarning ||
                     settings.visual.throwableTrajectory) &&
                    data::FindThreatObject(className) != nullptr) {
                    ProcessWorldActor(
                        actor,
                        className,
                        context,
                        settings,
                        frame,
                        seenThreats,
                        false,
                        true);
                }
                continue;
            }
            ++characterActorCount;
            const bool rangeTargetClass =
                classTraits.rangeTarget
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                || trackingRangeTarget
#endif
                ;
            const bool namedBotClass = classTraits.bot;
            const std::uintptr_t playerState = ReadPointer(actor + 0x390);
            const bool playerStateAvailable = IsValidPointer(playerState);
            const bool botClass = namedBotClass
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                || (trackingPlayerClass && !playerStateAvailable)
#endif
                ;
            if (!native::HasUsablePlayerState(playerStateAvailable, botClass)) {
                continue;
            }
            const bool useSecondaryTeam =
                context.warfare || settings.visual.battlefieldMode;
            std::int32_t primaryTeam = native::kUnknownPlayerTeam;
            std::int32_t secondaryTeam = native::kUnknownPlayerTeam;
            if (botClass) {
                primaryTeam = 0;
                secondaryTeam = 0;
            } else {
                const bool primaryTeamRead =
                    ReadValue(playerState + 0x658, primaryTeam);
                primaryTeam = native::ResolvePlayerTeam(
                    primaryTeamRead, primaryTeam, false);
                if (useSecondaryTeam) {
                    const bool secondaryTeamRead =
                        ReadValue(playerState + 0x65C, secondaryTeam);
                    secondaryTeam = native::ResolvePlayerTeam(
                        secondaryTeamRead, secondaryTeam, false);
                }
            }
            const std::int32_t targetTeam =
                useSecondaryTeam ? secondaryTeam : primaryTeam;
            const bool bot = botClass || targetTeam == 0;
            const bool threatTeamsValid =
                native::HasComparablePlayerTeams(
                    context.localTeam, targetTeam);
            if (native::IsPlayerTeammate(
                    context.localTeam,
                    targetTeam,
                    useSecondaryTeam,
                    context.localPrimaryTeam,
                    primaryTeam,
                    context.localSecondaryTeam,
                    secondaryTeam)) {
                continue;
            }
            const bool enemyEligible = native::IsEnemyEligible(
                context.localTeam, targetTeam, bot);

            native::FillOrdinaryActorPointers(
                actorRecord,
                [&](std::uintptr_t address) { return ReadPointer(address); });
            Vec3 position{};
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            const native::PositionReadMode actorPositionMode =
                trackingPlayerClass && actorRecord.resolverRecord
                ? native::PositionReadMode::Direct
                : positionMode;
#else
            const native::PositionReadMode actorPositionMode = positionMode;
#endif
            native::CharacterPositionSource positionSource =
                native::CharacterPositionSource::None;
            const bool coordinateAvailable = ReadCharacterPosition(
                actorRecord,
                className,
                actorPositionMode,
                settings.visual.antiFlicker,
                position,
                &positionSource);
            if (IsCoordinateTraceEnabled()) {
                auto& trace = coordinateTraceRecords_[actor];
                trace.recordRoot = actorRecord.root;
                trace.ordinaryRoot = actorRecord.ordinaryRoot;
                trace.resolverRecord = actorRecord.resolverRecord;
                trace.encryptedRecord = actorRecord.encryptedRecord;
                trace.ordinarySource = actorRecord.ordinarySource;
                trace.output = position;
            }
            if (!coordinateAvailable) {
                continue;
            }
            HealthState health{};
            std::uintptr_t healthSet = 0;
            const bool healthAvailable =
                ReadCoreHealth(actor, health, &healthSet);
            if (healthAvailable &&
                native::HasPlayerDetailReadField(
                    native::ResolvePlayerDownedReadMask(
                        true, health.health),
                    native::PlayerDetailReadField::Downed)) {
                ReadDownedState(actor, healthSet, health);
            }
            const native::PlayerLifecycleDisposition lifecycle =
                native::ResolvePlayerLifecycleDisposition(
                    true,
                    healthAvailable,
                    health.health,
                    health.downed,
                    settings.visual.downedPlayer,
                    settings.aim.enabled,
                    settings.aim.ignoreDowned);
            if (lifecycle ==
                native::PlayerLifecycleDisposition::Excluded) {
                continue;
            }
            const bool visualEligible = lifecycle ==
                native::PlayerLifecycleDisposition::Visual;
            const bool aimOnly = lifecycle ==
                native::PlayerLifecycleDisposition::AimOnly;
            const Vec3* resolvedBonePosition =
                native::ShouldAlignBoneFrameToCharacterPosition(positionSource)
                ? &position
                : nullptr;
            const Vec3 delta = Subtract(position, context.localPosition);
            const float distanceMeters = Length(delta) * 0.01f;
            const float horizontalDistanceMeters = HorizontalLength(delta) * 0.01f;
            if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) continue;
            if (!std::isfinite(horizontalDistanceMeters) ||
                horizontalDistanceMeters < 0.0f) {
                continue;
            }

            const bool combatModeActive = settings.visual.combatMode &&
                (context.firing || context.zooming);
            const bool selfAimActorEligible = aimModes.selfAim &&
                enemyEligible &&
                !(!rangeTargetClass && bot && settings.aim.ignoreBots) &&
                !(health.downed && settings.aim.ignoreDowned);
            const bool aimEligible = selfAimActorEligible;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            bool trackingActorEligible = recoveredTrackingActive &&
                trackingPlayerClass && actorRecord.resolverRecord &&
                (rangeTargetClass || enemyEligible);
            std::uint8_t trackingTargetState = rangeTargetClass ? 1 : 0;
            if (trackingActorEligible) {
                std::uint8_t excludedState = 0;
                ReadValue(actor + 0xDD8, excludedState);
                if (trackingTargetState == 0) {
                    const std::uintptr_t targetStateRoot =
                        ReadPointer(actor + 0x1030);
                    if (IsValidPointer(targetStateRoot)) {
                        ReadValue(
                            targetStateRoot + 0x2A1,
                            trackingTargetState);
                    }
                }
                std::uint8_t playerStateGate = 0;
                if (playerStateAvailable) {
                    ReadValue(playerState + 0x4C0, playerStateGate);
                }
                std::int32_t meshType = 0;
                const bool healthAlive = health.health > 0.0f;
                const bool trackingHealthEligible = healthAlive ||
                    (health.downed && !settings.aim.ignoreDowned);
                trackingActorEligible = excludedState == 0 &&
                    trackingHealthEligible &&
                    (playerStateGate == 0 || trackingTargetState != 0) &&
                    ReadTrackingMeshType(actorRecord, meshType) &&
                    (rangeTargetClass ||
                     PassTrackingCategory(actor, settings.aim)) &&
                    (!settings.aim.rejectTargetState ||
                     trackingTargetState == 0) &&
                    (!settings.aim.rejectDeadTarget || healthAlive);
            }
#endif

            native::GeometryVisibility actorVisibility =
                native::GeometryVisibility::Unavailable;
            if (geometrySnapshotReady_ && settings.visual.enabled &&
                (settings.visual.visibilityColor ||
                 aimWarningEnabled)) {
                Vec3 visibilityTarget = position;
                visibilityTarget.z += 90.0f;
                actorVisibility = TraceGeometry(
                    context.view.location, visibilityTarget);
            }
            const SemanticTone actorTone = ToneForVisibility(
                bot, settings.visual.visibilityColor, actorVisibility);

            const bool drawingInRange = native::IsWithinPlayerDrawRange(
                horizontalDistanceMeters,
                settings.visual.drawDistanceMeters);
            const bool warningInRange = visualEligible &&
                settings.visual.offscreenWarning &&
                settings.visual.warningSize > 0.0f &&
                native::IsWithinOffscreenWarningRange(
                    horizontalDistanceMeters,
                    settings.visual.drawDistanceMeters,
                    settings.visual.warningDistanceMeters);
            const bool radarInRange = health.health > 0.0f && enemyEligible &&
                frame.radar.has_value() &&
                distanceMeters <= frame.radar->maxDistanceMeters;
            const bool hudMapEligible = enemyEligible &&
                frame.hudMap.has_value();
            if (!HasReadableMesh(actorRecord)) {
                continue;
            }
            const bool facingNeeded = radarInRange || hudMapEligible ||
                (visualEligible && settings.visual.enabled && !bot &&
                 threatTeamsValid &&
                 (settings.visual.playerViewRay || aimWarningEnabled));
            Vec3 actorForward{};
            float actorHeadingRadians = 0.0f;
            const bool actorFacingValid = facingNeeded &&
                ReadActorFacing(
                    actorRecord,
                    actorForward,
                    actorHeadingRadians);
            if (hudMapEligible && (bot || actorFacingValid)) {
                const bool botAllowed = !bot ||
                    (hudBigMapActive
                        ? settings.radar.bigMapBots
                        : settings.radar.miniMapBots);
                if (botAllowed) {
                    const float heading = actorFacingValid
                        ? actorHeadingRadians
                        : 0.0f;
                    native::HudMapProjection projected{};
                    if (hudBigMapActive) {
                        projected = native::ProjectBigMap(
                            hudMapCache_,
                            position.x,
                            position.y,
                            heading,
                            static_cast<float>(options_.screenWidth),
                            static_cast<float>(options_.screenHeight),
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            frame.hudMap->markerSize);
                    } else if (hudMiniMapActive) {
                        projected = native::ProjectMiniMap(
                            hudMapCache_,
                            context.localPosition.x,
                            context.localPosition.y,
                            context.localPosition.z,
                            position.x,
                            position.y,
                            position.z,
                            heading,
                            context.mapBuildId >= 1900 && context.mapBuildId <= 2000,
                            settings.radar.mapOffsetX,
                            settings.radar.mapOffsetY,
                            frame.hudMap->markerSize);
                    }
                    if (projected.visible) {
                        HudMapMarker marker{};
                        marker.position = ImVec2(projected.marker.x, projected.marker.y);
                        marker.directionEnd = ImVec2(
                            projected.directionEnd.x, projected.directionEnd.y);
                        marker.kind = bot
                            ? RadarBlipKind::Bot
                            : RadarBlipKind::Player;
                        marker.tone = actorTone;
                        if (frame.hudMap->fontSize > 0.0f) {
                            marker.label = bot ? "AI" : ReadPlayerName(playerState);
                        }
                        marker.drawDirection = !bot && actorFacingValid;
                        frame.hudMap->markers.push_back(std::move(marker));
                    }
                }
            }

            if (!drawingInRange) {
                continue;
            }

            Vec2 bodyBottom{};
            Vec2 bodyTop{};
            CameraPoint cameraPoint{};
            const bool bottomProjected = ProjectToScreenLoose(
                Vec3{position.x, position.y, position.z - 5.0f},
                context.preparedProjection,
                bodyBottom,
                &cameraPoint);
            const bool topProjected = ProjectToScreenLoose(
                Vec3{position.x, position.y, position.z + 205.0f},
                context.preparedProjection,
                bodyTop);
            if (!bottomProjected || !topProjected ||
                !HasReadableBoneArray(actorRecord)) {
                continue;
            }
            const bool inFrontOfCamera = cameraPoint.forward > 0.01f;
            native::PlayerScreenBounds anchorBounds{};
            const bool anchorBoundsReady =
                native::CalculatePlayerAnchorBounds(
                    native::PlayerBoneScreenPoint{
                        bodyBottom.x, bodyBottom.y, bottomProjected},
                    native::PlayerBoneScreenPoint{
                        bodyTop.x, bodyTop.y, topProjected},
                    anchorBounds);
            bool onScreen = inFrontOfCamera && anchorBoundsReady &&
                native::IsReliablePlayerScreenBounds(
                    anchorBounds,
                    static_cast<float>(options_.screenWidth),
                    static_cast<float>(options_.screenHeight));

            BoneFrame boneFrame{};
            bool boneFrameReady = false;
            BoneFrameReadStatus boneReadStatus =
                BoneFrameReadStatus::NotRequested;
            ScreenRect boneBounds{};
            bool boneBoundsReady = false;
            if (aimEligible
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                || trackingActorEligible
#endif
                ||
                (settings.visual.enabled &&
                  (settings.visual.box ||
                   settings.visual.skeleton ||
                   settings.visual.visibilityColor ||
                   settings.visual.playerViewRay) &&
                  visualEligible && drawingInRange)) {
                boneFrameReady = ReadBoneFrame(
                    actorRecord,
                    context.preparedProjection,
                    settings.visual.antiFlicker,
                    resolvedBonePosition,
                    boneFrame,
                    &boneReadStatus);
                if (boneReadStatus == BoneFrameReadStatus::NoBoneArray &&
                    health.health <= 0.0f) {
                    boneReadStatus = BoneFrameReadStatus::Inactive;
                }
                recordBoneStatus(bot, boneReadStatus);
                if (boneReadStatus == BoneFrameReadStatus::NoBoneArray &&
                    boneFailureClass.empty()) {
                    boneFailureClass = className;
                    boneFailureDistance = distanceMeters;
                    boneFailureHealth = health.health;
                    boneFailureBot = bot;
                    boneFailureResolver = actorRecord.resolverRecord;
                    boneFailureEncrypted = actorRecord.encryptedRecord;
                }
                boneBoundsReady = boneFrameReady &&
                    TryBuildBoneBounds(boneFrame, boneBounds);
                const native::PlayerScreenBounds projectedBoneBounds{
                    boneBounds.left,
                    boneBounds.top,
                    boneBounds.right,
                    boneBounds.bottom,
                };
                native::PlayerScreenBounds visibleBounds{};
                onScreen = inFrontOfCamera &&
                    native::SelectPlayerScreenBounds(
                        boneBoundsReady,
                        projectedBoneBounds,
                        anchorBoundsReady,
                        anchorBounds,
                        static_cast<float>(options_.screenWidth),
                        static_cast<float>(options_.screenHeight),
                        visibleBounds);
            }

            if (bot) ++frame.botCount;
            else ++frame.playerCount;
            if (!bot && horizontalDistanceMeters <= 50.0f) {
                ++frame.nearbyEnemyCount;
            }

            if (IsCoordinateTraceEnabled()) {
                const auto trace = coordinateTraceRecords_.find(actor);
                if (trace != coordinateTraceRecords_.end()) {
                    trace->second.projectedBottom = bodyBottom;
                    trace->second.projectedTop = bodyTop;
                    trace->second.bottomProjected = bottomProjected;
                    trace->second.topProjected = topProjected;
                    trace->second.onScreen = onScreen;
                    std::fprintf(
                        stderr,
                        "[coordinate-trace] frame=%llu actor=%llx "
                        "resolver=%d encrypted=%d ordinary=%d "
                         "record_root=%llx input_root=%llx ordinary_root=%llx "
                        "component=%llx available=%d source=%s "
                        "error=%u sys=%d guest_pc=%llx "
                        "raw=(%.3f,%.3f,%.3f) xyz=(%.3f,%.3f,%.3f) "
                         "refresh=(%.3f,%.3f,%.3f) "
                         "refresh_attempted=%d refresh_ok=%d "
                         "second=(%.3f,%.3f,%.3f) history=(%.3f,%.3f,%.3f) "
                        "stability=%s delta=%.3f pending=%zu "
                        "second_attempted=%d "
                        "bottom=(%.2f,%.2f) top=(%.2f,%.2f) "
                        "projected=%d/%d on_screen=%d\n",
                        static_cast<unsigned long long>(coordinateTraceFrame_),
                        static_cast<unsigned long long>(actor),
                        trace->second.resolverRecord ? 1 : 0,
                        trace->second.encryptedRecord ? 1 : 0,
                        trace->second.ordinarySource ? 1 : 0,
                        static_cast<unsigned long long>(trace->second.recordRoot),
                        static_cast<unsigned long long>(trace->second.root),
                        static_cast<unsigned long long>(trace->second.ordinaryRoot),
                        static_cast<unsigned long long>(trace->second.component),
                        coordinateAvailable ? 1 : 0,
                        CoordinateTraceSourceName(trace->second.source),
                        static_cast<unsigned int>(trace->second.error),
                        trace->second.systemError,
                        static_cast<unsigned long long>(trace->second.guestPc),
                        trace->second.raw.x,
                        trace->second.raw.y,
                        trace->second.raw.z,
                         position.x,
                         position.y,
                         position.z,
                         trace->second.refresh.x,
                         trace->second.refresh.y,
                         trace->second.refresh.z,
                         trace->second.refreshAttempted ? 1 : 0,
                         trace->second.refreshSucceeded ? 1 : 0,
                         trace->second.second.x,
                        trace->second.second.y,
                        trace->second.second.z,
                        trace->second.history.x,
                        trace->second.history.y,
                        trace->second.history.z,
                        CoordinateStabilityDecisionName(
                            trace->second.stabilityDecision),
                        trace->second.stabilityDelta,
                        trace->second.pendingCount,
                        trace->second.secondAttempted ? 1 : 0,
                        bodyBottom.x,
                        bodyBottom.y,
                        bodyTop.x,
                        bodyTop.y,
                        bottomProjected ? 1 : 0,
                        topProjected ? 1 : 0,
                        onScreen ? 1 : 0);
                    std::fflush(stderr);
                }
            }

            bool aimWarningActive = false;
            if (visualEligible && aimWarningEnabled && threatTeamsValid && !bot &&
                health.health > 0.0f &&
                drawingInRange && onScreen) {
                aimWarningActive = ObserveAimWarning(
                    actor,
                    actorFacingValid && IsAimingAtPosition(
                        position, actorForward, context.localPosition) &&
                        actorVisibility != native::GeometryVisibility::Occluded,
                    frame.sequence,
                    warningTime);
                if (aimWarningActive && settings.visual.aimWarning) {
                    frame.aimWarningPlayers.push_back(ReadPlayerName(playerState));
                }
                if (aimWarningActive && settings.visual.aimWarningRay && bottomProjected) {
                    PlayerSignalVisual signal{};
                    signal.start = ImVec2(
                        static_cast<float>(options_.screenWidth) * 0.5f,
                        static_cast<float>(options_.screenHeight) - 3.0f);
                    signal.end = ImVec2(bodyBottom.x, bodyBottom.y);
                    signal.kind = PlayerSignalKind::AimWarning;
                    signal.tone = SemanticTone::Danger;
                    frame.playerSignals.push_back(std::move(signal));
                }
            }
            native::GeometryVisibility playerVisibility = actorVisibility;
            if (boneFrameReady &&
                (settings.visual.visibilityColor || settings.aim.missMode ||
                 ((aimEligible
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                   || trackingActorEligible
#endif
                   ) &&
                  settings.aim.requireVisibility))) {
                playerVisibility = EvaluateBoneVisibility(
                    context.view.location, boneFrame);
            }
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            if (trackingActorEligible) {
                CollectTrackingCandidateFromSample(
                    actorRecord,
                    context,
                    settings.aim,
                    aimTuning,
                    rangeTargetClass,
                    playerState,
                    position,
                    distanceMeters,
                    boneFrame,
                    boneFrameReady,
                    frame.sequence,
                    seenAimActors,
                    aimCandidates);
            }
#endif
            if (visualEligible && settings.visual.enabled &&
                settings.visual.playerViewRay &&
                threatTeamsValid && !bot && health.health > 0.0f &&
                drawingInRange && onScreen && actorFacingValid) {
                const float forwardLength = Length(actorForward);
                if (std::isfinite(forwardLength) && forwardLength > 0.0001f) {
                    const Vec3 sightStart = boneFrameReady && boneFrame.valid[0]
                        ? boneFrame.world[0]
                        : Vec3{position.x, position.y, position.z + 165.0f};
                    const Vec3 direction{
                        actorForward.x / forwardLength,
                        actorForward.y / forwardLength,
                        actorForward.z / forwardLength,
                    };
                    Vec2 sightStartScreen{};
                    Vec2 sightEndScreen{};
                    bool endProjected = false;
                    for (const float distance : {3000.0f, 1000.0f, 300.0f}) {
                        const Vec3 sightEnd{
                            sightStart.x + direction.x * distance,
                            sightStart.y + direction.y * distance,
                            sightStart.z + direction.z * distance,
                        };
                        if (ProjectToScreen(
                                sightEnd,
                                context.preparedProjection,
                                sightEndScreen)) {
                            endProjected = true;
                            break;
                        }
                    }
                    if (endProjected &&
                        ProjectToScreen(
                            sightStart,
                            context.preparedProjection,
                            sightStartScreen) &&
                        IsInsideScreen(
                            sightStartScreen,
                            options_.screenWidth,
                            options_.screenHeight,
                            80.0f)) {
                        const float maximumX =
                            static_cast<float>(options_.screenWidth) * 3.0f;
                        const float maximumY =
                            static_cast<float>(options_.screenHeight) * 3.0f;
                        sightEndScreen.x = std::clamp(
                            sightEndScreen.x, -maximumX, maximumX);
                        sightEndScreen.y = std::clamp(
                            sightEndScreen.y, -maximumY, maximumY);
                        PlayerSignalVisual signal{};
                        signal.start = ImVec2(
                            sightStartScreen.x, sightStartScreen.y);
                        signal.end = ImVec2(
                            sightEndScreen.x, sightEndScreen.y);
                        signal.kind = PlayerSignalKind::ViewDirection;
                        signal.tone = actorTone;
                        frame.playerSignals.push_back(std::move(signal));
                    }
                }
            }
            if (aimEligible) {
                const std::uint64_t identity =
                    native::ResolvePlayerIdentity(actor, playerState);
                Vec3 aimWorld{};
                Vec2 aimScreen{};
                int aimBone = -1;
                const int preferredBone = settings.aim.persistentLock &&
                        identity == lockedAimIdentity_
                    ? lockedAimBone_
                    : -1;
                const int aimMode = context.zooming
                    ? aimTuning.adsBone
                    : aimTuning.hipBone;
                bool aimPointReady = boneFrameReady && SelectAimPoint(
                    boneFrame,
                    aimMode,
                    settings.aim,
                    context.view.location,
                    identity,
                    frame.sequence,
                    preferredBone,
                    aimWorld,
                    aimScreen,
                    aimBone);
                if (!aimPointReady && rangeTargetClass) {
                    aimPointReady = SelectRangeTargetAimPoint(
                        position,
                        aimMode,
                        settings.aim,
                        context.view.location,
                        context.preparedProjection,
                        aimWorld,
                        aimScreen);
                }
                if (aimPointReady) {
                    Vec3 velocity{};
                    const std::uintptr_t movement =
                        ReadPointer(actor + 0x3D8);
                    if (IsValidPointer(movement)) {
                        ReadValue(movement + 0x2B0, velocity);
                    }
                    if (!IsFinite(velocity)) velocity = Vec3{};
                    const float centerX = static_cast<float>(options_.screenWidth) * 0.5f;
                    const float centerY = static_cast<float>(options_.screenHeight) * 0.5f;
                    const float screenDistance = std::hypot(
                        aimScreen.x - centerX, aimScreen.y - centerY);
                    const float distanceLimit = context.zooming
                        ? aimTuning.adsDistanceMeters
                        : aimTuning.hipDistanceMeters;
                    const bool distanceAllowed =
                        !settings.aim.enforceDistance ||
                        distanceMeters <= distanceLimit;
                    const bool rangeAllowed =
                        !settings.aim.enforceFov ||
                        screenDistance <= aimTuning.rangePixels;
                    if (std::isfinite(screenDistance) &&
                        rangeAllowed && distanceAllowed) {
                        aimCandidates.push_back(AimCandidate{
                            identity,
                            actor,
                            actorRecord.root,
                            actorRecord.mesh,
                            aimWorld,
                            velocity,
                            aimScreen,
                            screenDistance,
                            distanceMeters,
                            screenDistance,
                            aimBone,
                            actorRecord.encryptedRecord,
                            false,
                            rangeTargetClass,
                            selfAimActorEligible,
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                            false,
#endif
                        });
                    }
                }
            }

            if (aimOnly) {
                continue;
            }
            if ((combatModeActive && bot) ||
                (bot && settings.visual.filterBots)) {
                continue;
            }
            if (radarInRange && !bot) {
                AddRadarBlip(
                    context,
                    position,
                    false,
                    className,
                    actorHeadingRadians,
                    actorFacingValid,
                    ToneForVisibility(
                        false,
                        settings.visual.visibilityColor,
                        playerVisibility),
                    radarForwardX,
                    radarForwardY,
                    radarRightX,
                    radarRightY,
                    *frame.radar);
            }
            if (!onScreen && warningInRange &&
                settings.visual.enabled && !bot) {
                OffscreenMarker marker{};
                CameraPoint directionPoint = ToCameraSpace(
                    position, context.preparedProjection);
                if (directionPoint.forward >= 0.0f) {
                    marker.direction =
                        ImVec2(directionPoint.side, -directionPoint.vertical);
                } else {
                    marker.direction =
                        ImVec2(-directionPoint.side, directionPoint.vertical);
                }
                marker.radiusPixels = settings.visual.warningSize;
                marker.markerScale = std::clamp(
                    settings.visual.warningSize / 300.0f, 0.35f, 2.5f);
                marker.distanceMeters = distanceMeters;
                marker.tone = SemanticTone::Danger;
                frame.offscreenMarkers.push_back(std::move(marker));
            }
            if (!visualEligible || !settings.visual.enabled || !onScreen) {
                continue;
            }

            const native::PlayerScreenBounds resolvedBoneBounds{
                boneBounds.left,
                boneBounds.top,
                boneBounds.right,
                boneBounds.bottom,
            };
            native::PlayerScreenBounds resolvedBounds{};
            if (!native::SelectPlayerScreenBounds(
                    boneBoundsReady,
                    resolvedBoneBounds,
                    anchorBoundsReady,
                    anchorBounds,
                    static_cast<float>(options_.screenWidth),
                    static_cast<float>(options_.screenHeight),
                    resolvedBounds)) {
                continue;
            }
            const ScreenRect playerBounds{
                resolvedBounds.left,
                resolvedBounds.top,
                resolvedBounds.right,
                resolvedBounds.bottom,
            };
            const float height = playerBounds.bottom - playerBounds.top;
            if (!std::isfinite(height) || height < 8.0f) {
                continue;
            }
            native::PlayerEquipmentReadRequest equipmentRequest{};
            equipmentRequest.finalDrawable = true;
            equipmentRequest.bot = bot;
            equipmentRequest.healthBar = settings.visual.health;
            equipmentRequest.armorLevel = settings.visual.armorLevel;
            equipmentRequest.armorDurability =
                settings.visual.armorDurability;
            ReadEquipment(
                actor,
                native::ResolvePlayerEquipmentReadMask(equipmentRequest),
                health);
            PlayerVisual visual{};
            visual.identity = native::ResolvePlayerIdentity(actor, playerState);
            visual.bounds = playerBounds;
            visual.tone = ToneForVisibility(
                bot, settings.visual.visibilityColor, playerVisibility);
            visual.visible =
                playerVisibility != native::GeometryVisibility::Occluded;
            if (settings.aim.missMode && settings.aim.coverMode == 1 &&
                boneFrameReady) {
                const int coverAimMode = context.zooming
                    ? aimTuning.adsBone
                    : aimTuning.hipBone;
                if (coverAimMode == 0 || coverAimMode == 1) {
                    const aim::CoverPointSelection coverSelection =
                        aim::SelectCoverPoint(
                            settings.aim.coverMode,
                            coverAimMode,
                            boneFrame.valid,
                            boneFrame.visibility);
                    visual.coverHighlighted = coverSelection.selected;
                }
            }
            visual.drawCornerBox = settings.visual.box;
            visual.drawTracer = settings.visual.snapline;
            visual.drawVitals = settings.visual.health;
            visual.drawPlate = settings.visual.playerName || settings.visual.distance ||
                settings.visual.operatorName || settings.visual.heldWeapon ||
                settings.visual.armorLevel || settings.visual.armorDurability ||
                settings.visual.health;
            visual.drawSkeleton = settings.visual.skeleton &&
                horizontalDistanceMeters <=
                    settings.visual.skeletonDistanceMeters;
            visual.tracerOrigin = render::TopTracerOrigin(
                static_cast<float>(options_.screenWidth));
            visual.vitals.health = health.health;
            visual.vitals.maxHealth = health.maxHealth;
            visual.vitals.armor = health.armor;
            visual.vitals.maxArmor = health.maxArmor;
            visual.vitals.downed = health.downed;

            if (settings.visual.playerName) {
                visual.name = ReadPlayerName(playerState);
                if (visual.name.empty() && bot) visual.name = "AI";
            }
            if (settings.visual.operatorName && !bot) {
                std::int32_t operatorId = 0;
                if (ReadValue(playerState + 0x9E8, operatorId)) {
                    if (const char* operatorName = OperatorName(operatorId)) {
                        if (!visual.name.empty()) visual.name += " | ";
                        visual.name += operatorName;
                    }
                }
            }
            if (settings.visual.distance) {
                AppendDetail(visual.detail, FormatDistance(distanceMeters));
            }
            if (settings.visual.heldWeapon && !bot) {
                const std::uintptr_t weapon = ReadPointer(actor + 0x1790);
                std::uint32_t weaponId = 0;
                if (IsValidPointer(weapon)) ReadValue(weapon + 0x838, weaponId);
                if (const char* weaponName = data::FindHandheldName(weaponId)) {
                    AppendDetail(visual.detail, weaponName);
                }
            }
            if ((settings.visual.armorLevel || settings.visual.armorDurability) && !bot) {
                std::string helmet;
                std::string armor;
                if (settings.visual.armorLevel) {
                    helmet = health.helmetLevel > 0
                        ? "头" + std::to_string(health.helmetLevel)
                        : "无头";
                    armor = health.armorLevel > 0
                        ? "甲" + std::to_string(health.armorLevel)
                        : "无甲";
                }
                if (settings.visual.armorDurability && health.maxHelmet > 0.0f) {
                    if (helmet.empty()) helmet = "头";
                    helmet += " " + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.helmet)))) +
                        "/" + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.maxHelmet))));
                }
                if (settings.visual.armorDurability && health.maxArmor > 0.0f) {
                    if (armor.empty()) armor = "甲";
                    armor += " " + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.armor)))) +
                        "/" + std::to_string(
                        std::max(0, static_cast<int>(std::lround(health.maxArmor))));
                }
                AppendDetail(helmet, armor);
                AppendDetail(visual.detail, helmet);
            }
            if (settings.visual.classNameDebug && !className.empty()) {
                AppendDetail(visual.detail, className);
            }

            if (visual.drawSkeleton) {
                if (!boneFrameReady) {
                    const BoneFrameReadStatus previousBoneReadStatus =
                        boneReadStatus;
                    boneFrameReady = ReadBoneFrame(
                        actorRecord,
                        context.preparedProjection,
                        settings.visual.antiFlicker,
                        resolvedBonePosition,
                        boneFrame,
                        &boneReadStatus);
                    if (boneReadStatus == BoneFrameReadStatus::NoBoneArray &&
                        health.health <= 0.0f) {
                        boneReadStatus = BoneFrameReadStatus::Inactive;
                    }
                    if (previousBoneReadStatus !=
                        BoneFrameReadStatus::NotRequested) {
                        const std::size_t previousIndex =
                            static_cast<std::size_t>(previousBoneReadStatus);
                        auto& counts = bot
                            ? botBoneStatusCounts
                            : playerBoneStatusCounts;
                        if (previousIndex < counts.size() &&
                            counts[previousIndex] != 0) {
                            --counts[previousIndex];
                        }
                    }
                    recordBoneStatus(bot, boneReadStatus);
                }
                visual.skeleton.colorByVisibility =
                    !visual.coverHighlighted &&
                    (settings.visual.visibilityColor ||
                     settings.aim.missMode);
                if (!boneFrameReady) visual.drawSkeleton = false;
            }
            playerProjectionSources.push_back(PlayerProjectionSource{
                frame.players.size(),
                Vec3{position.x, position.y, position.z - 5.0f},
                Vec3{position.x, position.y, position.z + 205.0f},
                std::move(boneFrame),
                boneFrameReady,
            });
            frame.players.push_back(std::move(visual));
        }
        actorLoopTrace.Finish();
        platform::RecordPerformanceCount(
            platform::PerformanceCounter::CharacterActors,
            characterActorCount);
        platform::RecordPerformanceCount(
            platform::PerformanceCounter::CoordinateAttempts,
            algorithmFrameAttemptCount_);
        platform::RecordPerformanceCount(
            platform::PerformanceCounter::CoordinateSuccesses,
            algorithmFrameSuccessCount_);

#if LENGJING_ENABLE_PROJECTILE_TRACKING
        const bool requireDecodedNames = !recoveredTrackingActive;
#else
        constexpr bool requireDecodedNames = true;
#endif
        if (requireDecodedNames && validActorCount > 0 &&
            decodedNameCount == 0) {
            SetRuntimeFailure(
                probe, RuntimeError::NamePoolUnavailable);
            error = "数据链等待：名称池暂不可读";
            aimController_.ClearTarget();
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
            lockedTrackingIdentity_ = 0;
            lockedTrackingBone_ = -1;
#endif
            FinalizeCoordinateExecutionHealth(infrastructureProbeFailed);
            UpdateCoordinateProbe(probe);
            return true;
        }

        RefreshCameraView(context, frame.sequence);
        ReprojectPlayers(
            context.preparedProjection, playerProjectionSources, frame);
        ReprojectAimCandidates(
            context.preparedProjection,
            settings.aim,
            aimTuning,
            aimCandidates);
        AppendWorldObjectCache(settings, context, frame);
        FinalizeThreatSignals(settings, frame);
        PublishAimFrame(settings.aim, aimTuning, context, aimCandidates, frame);

        PruneThreatState(seenThreats);
        const auto pruneTime = std::chrono::steady_clock::now();
        PruneBoneCache(pruneTime);
        PrunePositionCache(pruneTime);
        if (frame.highValueList.has_value()) {
            auto& entries = frame.highValueList->entries;
            std::stable_sort(entries.begin(), entries.end(),
                [](const HighValueEntry& left, const HighValueEntry& right) {
                    return left.value != right.value
                        ? left.value > right.value
                        : left.distanceMeters < right.distanceMeters;
                });
            if (entries.size() > static_cast<std::size_t>(frame.highValueList->maxRows)) {
                entries.resize(static_cast<std::size_t>(frame.highValueList->maxRows));
            }
        }
        FinalizeCoordinateExecutionHealth(infrastructureProbeFailed);
        UpdateCoordinateProbe(probe);
        ApplyCoordinateDiagnostic(error, probe);
        const auto boneAuditNow = std::chrono::steady_clock::now();
        if (boneAuditNow - lastBoneAuditLogAt_ >= std::chrono::seconds(1)) {
            const auto requested = [](const auto& counts) {
                std::size_t total = 0;
                for (std::size_t index = 1; index < counts.size(); ++index) {
                    if (index == static_cast<std::size_t>(
                            BoneFrameReadStatus::Inactive)) {
                        continue;
                    }
                    total += counts[index];
                }
                return total;
            };
            std::fprintf(
                stderr,
                "[bone-audit] records=%zu valid=%zu characters=%zu "
                "targets=%zu bots=%zu players=%zu visuals=%zu "
                "bot_bones=%zu/%zu bot_offscreen=%zu bot_no_source=%zu "
                "bot_no_component=%zu bot_no_array=%zu "
                "bot_no_transforms=%zu bot_inactive=%zu bot_invalid=%zu "
                "player_bones=%zu/%zu player_offscreen=%zu "
                "player_no_source=%zu player_no_component=%zu "
                "player_no_array=%zu player_no_transforms=%zu "
                "player_inactive=%zu player_invalid=%zu "
                "failure_class=%s failure_bot=%d "
                "failure_distance=%.1f failure_health=%.1f "
                "failure_resolver=%d failure_encrypted=%d\n",
                actorRecords.size(),
                validActorCount,
                characterActorCount,
                static_cast<std::size_t>(frame.botCount + frame.playerCount),
                static_cast<std::size_t>(frame.botCount),
                static_cast<std::size_t>(frame.playerCount),
                frame.players.size(),
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::Ready)],
                requested(botBoneStatusCounts),
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::Offscreen)],
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoSource)],
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoComponent)],
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoBoneArray)],
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoTransforms)],
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::Inactive)],
                botBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::InvalidActor)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::Ready)],
                requested(playerBoneStatusCounts),
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::Offscreen)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoSource)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoComponent)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoBoneArray)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::NoTransforms)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::Inactive)],
                playerBoneStatusCounts[
                    static_cast<std::size_t>(BoneFrameReadStatus::InvalidActor)],
                boneFailureClass.empty() ? "-" : boneFailureClass.c_str(),
                boneFailureBot ? 1 : 0,
                boneFailureDistance,
                boneFailureHealth,
                boneFailureResolver ? 1 : 0,
                boneFailureEncrypted ? 1 : 0);
            std::fflush(stderr);
            lastBoneAuditLogAt_ = boneAuditNow;
        }
        frame.ready = true;
        return true;
    }

    void SetAimEnabled(bool enabled) override {
        aimEnabled_.store(enabled, std::memory_order_release);
        aimController_.SetEnabled(enabled);
    }

    void UpdateDisplayGeometry(
        int width, int height, int orientation) override {
        if (width <= 1 || height <= 1) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        options_.screenWidth = width;
        options_.screenHeight = height;
        options_.orientation = ((orientation % 4) + 4) % 4;
    }

    void ReloadCustomItems() override {
        std::lock_guard<std::mutex> lock(mutex_);
        customItems_.Load(customItemPath_);
        worldObjectDiscoveryPolicy_.Invalidate();
        worldObjectContentPolicy_.Invalidate();
    }

private:
    using RuntimeActorRecord = native::ActorRecordSource;

    struct FrameContext {
        std::uintptr_t world = 0;
        std::uintptr_t level = 0;
        std::uintptr_t actorArray = 0;
        std::int32_t actorCount = 0;
        std::uintptr_t localController = 0;
        std::uintptr_t localPawn = 0;
        std::uintptr_t cameraManager = 0;
        std::uintptr_t namePool = 0;
        Vec3 localPosition{};
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        Vec3 firingOrigin{};
#endif
        CameraView view{};
        native::PreparedProjection preparedProjection{};
        std::int32_t localTeam = -1;
        std::int32_t localPrimaryTeam = -1;
        std::int32_t localSecondaryTeam = -1;
        std::int32_t mapBuildId = 0;
        bool warfare = false;
        bool firing = false;
        bool zooming = false;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        bool firingOriginValid = false;
#endif
        bool decodedRecordsEncrypted = false;
        bool decodedRecordSourceReady = false;
        std::uint64_t weaponId = 0;
        std::uintptr_t weaponRoot = 0;
    };

    struct ActorRecordSnapshot {
        native::ActorRecordSnapshotKey key{};
        std::vector<RuntimeActorRecord> records;
        std::vector<RuntimeActorRecord> decodedRecords;
        std::chrono::steady_clock::time_point decodedUpdatedAt{};
        bool decodedRecordsEncrypted = false;
        bool decodedRecordArrayLocated = false;
        bool decodedRecordSourceReady = false;
        bool decodedRecordSourceRetained = false;
        bool hasKey = false;
    };

    struct HealthState {
        float health = 0.0f;
        float maxHealth = 100.0f;
        float armor = 0.0f;
        float maxArmor = 100.0f;
        float helmet = 0.0f;
        float maxHelmet = 0.0f;
        int helmetLevel = 0;
        int armorLevel = 0;
        bool downed = false;
    };

    struct BoneFrame {
        std::array<Vec3, kBoneIndices.size()> world{};
        std::array<Vec2, kBoneIndices.size()> screen{};
        std::array<bool, kBoneIndices.size()> worldValid{};
        std::array<bool, kBoneIndices.size()> valid{};
        std::array<native::GeometryVisibility, kBoneIndices.size()> visibility{};
    };

    enum class BoneFrameReadStatus {
        NotRequested,
        InvalidActor,
        NoSource,
        NoComponent,
        NoBoneArray,
        NoTransforms,
        Inactive,
        Offscreen,
        Ready,
        Count,
    };

    struct PlayerProjectionSource {
        std::size_t playerIndex = 0;
        Vec3 bottom{};
        Vec3 top{};
        BoneFrame bones{};
        bool bonesReady = false;
    };

    struct WorldLabelProjectionSource {
        std::size_t labelIndex = 0;
        Vec3 world{};
    };

    struct WorldObjectFrameCache {
        std::vector<WorldLabel> labels;
        std::vector<WorldLabelProjectionSource> labelSources;
        std::optional<HighValueList> highValueList;

        void Clear() {
            labels.clear();
            labelSources.clear();
            highValueList.reset();
        }
    };

    struct CachedWorldActor {
        std::uintptr_t actor = 0;
        std::string className;
    };

    struct PositionCacheEntry {
        Vec3 position{};
        std::chrono::steady_clock::time_point updatedAt{};
        std::uintptr_t world = 0;
    };

    struct DecodedPositionCacheEntry {
        Vec3 position{};
        std::chrono::steady_clock::time_point updatedAt{};
        std::chrono::steady_clock::time_point observedAt{};
        native::DecodedPositionCacheIdentity identity{};
    };

    struct DecodedPositionPendingEntry {
        native::AlgorithmPositionPendingSample sample{};
        native::DecodedPositionCacheIdentity identity{};
    };

    struct CoordinateTraceRecord {
        std::uintptr_t root = 0;
        std::uintptr_t component = 0;
        std::uintptr_t recordRoot = 0;
        std::uintptr_t ordinaryRoot = 0;
        Vec3 raw{};
        Vec3 refresh{};
        Vec3 second{};
        Vec3 history{};
        Vec3 output{};
        Vec2 projectedBottom{};
        Vec2 projectedTop{};
        CoordinateTraceSource source = CoordinateTraceSource::None;
        CoordinateStabilityDecision stabilityDecision =
            CoordinateStabilityDecision::None;
        double stabilityDelta = 0.0;
        std::size_t pendingCount = 0;
        CoordinateDecryptError error = CoordinateDecryptError::None;
        int systemError = 0;
        std::uintptr_t guestPc = 0;
        bool attempted = false;
        bool refreshAttempted = false;
        bool refreshSucceeded = false;
        bool secondAttempted = false;
        bool resolverRecord = false;
        bool encryptedRecord = false;
        bool ordinarySource = false;
        bool bottomProjected = false;
        bool topProjected = false;
        bool onScreen = false;
    };

    struct BoneCacheEntry {
        std::uintptr_t root = 0;
        std::uintptr_t mesh = 0;
        std::uintptr_t boneArray = 0;
        bool encryptedRecord = false;
        bool resolvedTranslation = false;
        std::array<Vec3, kBoneIndices.size()> world{};
        std::array<bool, kBoneIndices.size()> valid{};
        std::array<
            std::chrono::steady_clock::time_point,
            kBoneIndices.size()> boneUpdatedAt{};
        std::chrono::steady_clock::time_point lastUpdatedAt{};
    };

    struct AimCandidate {
        std::uint64_t identity = 0;
        std::uintptr_t actor = 0;
        std::uintptr_t root = 0;
        std::uintptr_t mesh = 0;
        Vec3 world{};
        Vec3 velocity{};
        Vec2 screen{};
        float screenDistancePixels = 0.0f;
        float worldDistanceMeters = 0.0f;
        float selectionDistancePixels = 0.0f;
        int boneIndex = -1;
        bool encryptedRecord = false;
        bool alignBones = false;
        bool rangeTarget = false;
        bool selfAimEligible = false;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        bool trackingEligible = false;
#endif
    };

    struct AimWarningState {
        bool tracking = false;
        bool active = false;
        std::uint64_t lastSeenSequence = 0;
        std::chrono::steady_clock::time_point alignedSince{};
    };

    struct GeometryReadRoute {
        native::MemoryTransport* shared = nullptr;
        native::MemoryTransport* dedicated = nullptr;
        std::atomic<bool> useDedicated{false};
        std::atomic<std::uint32_t> consecutiveFallbacks{0};
        std::atomic<std::int64_t> nextProbeNanoseconds{0};
    };

    void ReleaseGeometryTransport() noexcept {
        geometryReadRoute_.reset();
        if (geometryMemory_ != nullptr) {
            geometryMemory_->Close();
            geometryMemory_.reset();
        }
        geometryTransportDedicated_ = false;
    }

    void UpdateGeometryRuntime(const FeatureSettings& settings) {
        const bool needed =
            (settings.visual.enabled &&
             (settings.visual.modelGeometry ||
              settings.visual.visibilityColor ||
              (settings.visual.skeleton && settings.aim.missMode))) ||
            (aim::IsAimOutputRequested(
                 settings.aim.enabled,
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                 settings.aim.trajectoryTracking) &&
#else
                 false) &&
#endif
             aimEnabled_.load(std::memory_order_acquire) &&
             (settings.aim.missMode ||
              settings.aim.requireVisibility));
        if (!needed) {
            if (geometryRuntime_.IsRunning()) geometryRuntime_.Stop();
            ReleaseGeometryTransport();
            geometrySnapshotReady_ = false;
            geometryRefreshEpoch_ = 0;
            geometryValidationEpoch_ = 0;
            geometryValidationPending_ = false;
            geometryRetryAfter_ = {};
            return;
        }

        if (!geometryRuntime_.IsRunning()) {
            auto now = std::chrono::steady_clock::now();
            if (now < geometryRetryAfter_) return;
            native::GeometryRuntimeConfig config{};
            for (const std::uintptr_t offset :
                 layout_.geometryInstancePointerOffsets) {
                if (offset == 0 ||
                    offset > kMaximumRemoteAddress - moduleBase_) {
                    continue;
                }
                const std::uintptr_t slot = moduleBase_ + offset;
                if (IsValidReadAddress(slot)) {
                    config.instancePointerSlots.push_back(slot);
                }
            }
            if (geometryMemory_ == nullptr || !geometryMemory_->IsOpen()) {
                geometryMemory_.reset();
                auto candidate =
                    std::make_unique<native::MemoryTransport>();
                RuntimeDiagnostic geometryDiagnostic{};
                std::string geometryError;
                bool candidateReady = candidate->OpenReadOnly(
                        options_.driverIndex,
                        processId_,
                        layout_.processName,
                        geometryDiagnostic,
                        geometryError);
                std::array<std::uint8_t, 4> candidateSignature{};
                candidateReady = candidateReady &&
                    candidate->ReadGeometry(
                        moduleBase_, candidateSignature.data(),
                        candidateSignature.size()) &&
                    candidateSignature[0] == 0x7FU &&
                    candidateSignature[1] == 'E' &&
                    candidateSignature[2] == 'L' &&
                    candidateSignature[3] == 'F';
                bool instanceSlotReadable = false;
                for (const std::uintptr_t slot :
                     config.instancePointerSlots) {
                    std::uintptr_t ignoredInstance = 0;
                    if (candidateReady && candidate->ReadGeometry(
                            slot, &ignoredInstance,
                            sizeof(ignoredInstance))) {
                        instanceSlotReadable = true;
                        break;
                    }
                }
                if (candidateReady && instanceSlotReadable) {
                    geometryMemory_ = std::move(candidate);
                    geometryTransportDedicated_ = true;
                } else {
                    geometryTransportDedicated_ = false;
                }
            }
            auto readRoute = std::make_shared<GeometryReadRoute>();
            readRoute->shared = memory_.get();
            readRoute->dedicated =
                geometryTransportDedicated_ && geometryMemory_ != nullptr
                ? geometryMemory_.get()
                : nullptr;
            readRoute->useDedicated.store(
                readRoute->dedicated != nullptr,
                std::memory_order_release);
            geometryReadRoute_ = readRoute;
            auto readBytes = [readRoute](
                                 std::uintptr_t address,
                                 void* destination,
                                 std::size_t size) {
                if (!IsValidReadAddress(address) || size == 0 ||
                    size > kMaximumRemoteAddress - address) {
                    return false;
                }
                bool dedicatedActive = readRoute->useDedicated.load(
                    std::memory_order_acquire);
                bool dedicatedProbe = false;
                std::int64_t nowNanoseconds = 0;
                if (!dedicatedActive && readRoute->dedicated != nullptr) {
                    nowNanoseconds =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now()
                                .time_since_epoch())
                            .count();
                    std::int64_t nextProbe =
                        readRoute->nextProbeNanoseconds.load(
                            std::memory_order_acquire);
                    constexpr std::int64_t kProbeIntervalNanoseconds =
                        5'000'000'000LL;
                    if (nowNanoseconds >= nextProbe &&
                        readRoute->nextProbeNanoseconds
                            .compare_exchange_strong(
                                nextProbe,
                                nowNanoseconds + kProbeIntervalNanoseconds,
                                std::memory_order_acq_rel,
                                std::memory_order_acquire)) {
                        dedicatedProbe = true;
                    }
                }
                if ((dedicatedActive || dedicatedProbe) &&
                    readRoute->dedicated != nullptr) {
                    platform::RecordPerformanceCount(
                        platform::PerformanceCounter::GeometryDedicatedReads);
                    if (readRoute->dedicated->ReadGeometry(
                            address, destination, size)) {
                        readRoute->consecutiveFallbacks.store(
                            0, std::memory_order_relaxed);
                        if (!dedicatedActive) {
                            readRoute->useDedicated.store(
                                true, std::memory_order_release);
                            readRoute->nextProbeNanoseconds.store(
                                0, std::memory_order_release);
                            platform::RecordPerformanceCount(
                                platform::PerformanceCounter::
                                    GeometryTransportRecoveries);
                        }
                        return true;
                    }
                    if (readRoute->shared == nullptr) return false;
                    platform::RecordPerformanceCount(
                        platform::PerformanceCounter::GeometrySharedReads);
                    const bool fallback = readRoute->shared->ReadGeometry(
                        address, destination, size);
                    if (fallback && dedicatedActive) {
                        constexpr std::uint32_t kFallbackLimit = 3;
                        const std::uint32_t fallbackCount =
                            readRoute->consecutiveFallbacks.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                        if (fallbackCount >= kFallbackLimit &&
                            readRoute->useDedicated.exchange(
                                false, std::memory_order_acq_rel)) {
                            constexpr std::int64_t
                                kProbeIntervalNanoseconds =
                                    5'000'000'000LL;
                            if (nowNanoseconds == 0) {
                                nowNanoseconds =
                                    std::chrono::duration_cast<
                                        std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now()
                                            .time_since_epoch())
                                        .count();
                            }
                            readRoute->nextProbeNanoseconds.store(
                                nowNanoseconds +
                                    kProbeIntervalNanoseconds,
                                std::memory_order_release);
                            platform::RecordPerformanceCount(
                                platform::PerformanceCounter::
                                    GeometryTransportDemotions);
                        }
                    }
                    return fallback;
                }
                if (readRoute->shared == nullptr) return false;
                platform::RecordPerformanceCount(
                    platform::PerformanceCounter::GeometrySharedReads);
                return readRoute->shared->ReadGeometry(
                    address, destination, size);
            };
            if (config.instancePointerSlots.empty() ||
                !geometryRuntime_.Start(std::move(readBytes), std::move(config))) {
                ReleaseGeometryTransport();
                geometrySnapshotReady_ = false;
                geometryRefreshEpoch_ = 0;
                geometryValidationEpoch_ = 0;
                geometryValidationPending_ = false;
                geometryRetryAfter_ = now + std::chrono::seconds(2);
                return;
            }
            geometryRetryAfter_ = {};
            geometrySnapshotReady_ = false;
            geometryValidationPending_ = false;
            geometryRefreshEpoch_ = geometryRuntime_.CurrentRefreshEpoch();
            geometryValidationEpoch_ = 0;
        }

        const std::shared_ptr<const native::GeometrySnapshot> current =
            geometryRuntime_.GetSnapshot();
        if (!geometrySnapshotReady_ && current != nullptr &&
            current->available &&
            current->refreshEpoch >= geometryRefreshEpoch_ &&
            current->validationEpoch >= geometryValidationEpoch_) {
            geometrySnapshotReady_ = true;
        }
    }

    void RequestGeometryRefresh() {
        if (!geometryRuntime_.IsRunning()) return;
        geometrySnapshotReady_ = false;
        geometryValidationEpoch_ = 0;
        geometryRefreshEpoch_ = geometryRuntime_.RequestRefresh();
    }

    native::GeometryVisibility TraceGeometry(const Vec3& origin,
                                             const Vec3& target) const {
        if (!geometrySnapshotReady_) {
            return native::GeometryVisibility::Unavailable;
        }
        return geometryRuntime_.Trace(
            ToGeometryPoint(origin), ToGeometryPoint(target));
    }

    template <typename T>
    bool ReadValue(std::uintptr_t address, T& output) {
        if (!IsValidReadAddress(address) ||
            sizeof(T) > kMaximumRemoteAddress - address ||
            memory_ == nullptr) {
            return false;
        }
        T value{};
        if (!memory_->Read(address, &value, sizeof(T))) {
            return false;
        }
        output = value;
        return true;
    }

    std::uintptr_t ReadPointer(std::uintptr_t address) {
        std::uintptr_t value = 0;
        return ReadValue(address, value) && IsValidPointer(value) ? value : 0;
    }

    std::vector<RuntimeActorRecord> CollectDecodedActorRecords(
        bool& sourceReady,
        bool& encrypted) {
        std::vector<RuntimeActorRecord> result;
        sourceReady = false;
        encrypted = false;
        if (!native::HasConfiguredActorRecordSource(
                layout_.actorRecordLayout) ||
            memory_ == nullptr) {
            return result;
        }

        auto readBytes = [this](std::uintptr_t address,
                                void* destination,
                                std::size_t size) {
            return memory_ != nullptr && IsValidReadAddress(address) &&
                size != 0 && size <= kMaximumRemoteAddress - address &&
                memory_->Read(address, destination, size);
        };
        const auto validatePointer = [](std::uintptr_t address) {
            return IsValidPointer(address);
        };
        const native::ActorRecordResolver resolver(
            layout_.actorRecordLayout);
        const std::optional<native::ActorArrayDescriptor> array =
            resolver.Locate(moduleBase_, readBytes, validatePointer);
        if (!array.has_value()) return result;
        sourceReady = true;
        encrypted = array->encrypted;

        result.reserve(std::min<std::uint32_t>(array->count, 10000));
        for (std::uint32_t index = 0;
             index < array->count && result.size() < 10000;
             ++index) {
            const std::optional<native::ActorAddressRecord> record =
                resolver.ReadRecord(*array, index, readBytes);
            if (!record.has_value() ||
                !IsValidPointer(record->actor) ||
                !IsValidPointer(record->root)) {
                continue;
            }
            result.push_back(native::MakeResolvedActorRecord(
                record->actor,
                record->root,
                record->mesh,
                array->encrypted));
        }
        return result;
    }

    std::vector<RuntimeActorRecord> CollectActorRecords(
        const FrameContext& context,
        const std::vector<RuntimeActorRecord>& decodedActors) {
        return native::MergeCurrentLevelActorRecordSources(
            CollectActorAddresses(context), decodedActors);
    }

    void RefreshActorRecordSnapshot(FrameContext& context,
                                    bool decodedRequired) {
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::ActorSnapshot);
        const native::ActorRecordSnapshotKey key{
            moduleBase_,
            context.world,
            context.level,
            context.actorArray,
            context.actorCount,
            context.localPawn,
            decodedRequired,
        };
        const auto now = std::chrono::steady_clock::now();
        const bool metadataRefreshed =
            actorRecordRefreshPolicy_.ShouldRefresh(key, now);
        if (metadataRefreshed) {
            ActorRecordSnapshot candidate{};
            candidate.key = key;
            candidate.hasKey = true;
            if (decodedRequired) {
                candidate.decodedRecords = CollectDecodedActorRecords(
                    candidate.decodedRecordSourceReady,
                    candidate.decodedRecordsEncrypted);
                candidate.decodedRecordArrayLocated =
                    candidate.decodedRecordSourceReady;
                if (!candidate.decodedRecords.empty()) {
                    candidate.decodedUpdatedAt = now;
                } else if (actorRecordSnapshot_.hasKey &&
                           native::CanRetainDecodedActorSnapshot(
                               native::ActorRecordIdentity(
                                   actorRecordSnapshot_.key),
                               native::ActorRecordIdentity(key),
                               actorRecordSnapshot_.decodedRecordSourceReady,
                               false,
                               actorRecordSnapshot_.decodedUpdatedAt,
                               now)) {
                    candidate.decodedRecords =
                        actorRecordSnapshot_.decodedRecords;
                    candidate.decodedRecordSourceReady =
                        !candidate.decodedRecords.empty();
                    candidate.decodedRecordsEncrypted =
                        actorRecordSnapshot_.decodedRecordsEncrypted;
                    candidate.decodedUpdatedAt =
                        actorRecordSnapshot_.decodedUpdatedAt;
                    candidate.decodedRecordSourceRetained =
                        candidate.decodedRecordSourceReady;
                }
            }
            actorRecordSnapshot_ = std::move(candidate);
        }

        actorRecordSnapshot_.records = CollectActorRecords(
            context, actorRecordSnapshot_.decodedRecords);
        const bool currentDecodedRecordsReady = std::any_of(
            actorRecordSnapshot_.records.begin(),
            actorRecordSnapshot_.records.end(),
            [](const RuntimeActorRecord& record) {
                return record.resolverRecord;
            });
        context.decodedRecordsEncrypted =
            actorRecordSnapshot_.decodedRecordsEncrypted;
        context.decodedRecordSourceReady =
            actorRecordSnapshot_.decodedRecordSourceReady &&
            (!decodedRequired || currentDecodedRecordsReady);

        if (IsCoordinateTraceEnabled() &&
            ShouldWriteCoordinateFrameTrace(coordinateTraceFrame_)) {
            std::fprintf(
                stderr,
                "[coordinate-trace-snapshot] frame=%llu decoded_required=%d "
                "decoded_ready=%d encrypted=%d retained=%d records=%zu "
                "ordinary_array=%llx "
                "ordinary_count=%d\n",
                static_cast<unsigned long long>(coordinateTraceFrame_),
                decodedRequired ? 1 : 0,
                context.decodedRecordSourceReady ? 1 : 0,
                actorRecordSnapshot_.decodedRecordsEncrypted ? 1 : 0,
                actorRecordSnapshot_.decodedRecordSourceRetained ? 1 : 0,
                actorRecordSnapshot_.records.size(),
                static_cast<unsigned long long>(context.actorArray),
                context.actorCount);
            std::fflush(stderr);
        }

        const bool cacheable = !actorRecordSnapshot_.records.empty() &&
            (!decodedRequired || context.decodedRecordSourceReady);
        if (cacheable && metadataRefreshed) {
            actorRecordRefreshPolicy_.MarkRefreshed(key, now);
        } else if (!cacheable) {
            actorRecordRefreshPolicy_.Invalidate();
        }
    }

    void InvalidateActorRecordSnapshot() {
        actorRecordRefreshPolicy_.Invalidate();
        actorRecordSnapshot_ = ActorRecordSnapshot{};
    }

    std::vector<std::uintptr_t> CollectActorAddresses(
        const FrameContext& context) {
        constexpr std::size_t kMaximumCollectedActors = 100000;
        std::vector<std::uintptr_t> result;
        std::unordered_set<std::uintptr_t> seen;
        result.reserve(static_cast<std::size_t>(std::max(context.actorCount, 0)));
        seen.reserve(static_cast<std::size_t>(std::max(context.actorCount, 0)));

        const auto appendArray = [&](const ActorArrayHeader& header,
                                     std::int32_t maximumCount) {
            if (!IsValidPointer(header.data) || header.count <= 0 ||
                header.count > maximumCount || header.capacity < header.count ||
                result.size() >= kMaximumCollectedActors) {
                return;
            }
            std::vector<std::uintptr_t> addresses(
                static_cast<std::size_t>(header.count));
            const bool bulkRead = memory_ != nullptr && memory_->Read(
                    header.data,
                    addresses.data(),
                    addresses.size() * sizeof(std::uintptr_t));
            if (!bulkRead) {
                for (std::size_t index = 0; index < addresses.size(); ++index) {
                    const std::uintptr_t offset =
                        index * sizeof(std::uintptr_t);
                    if (offset > kMaximumRemoteAddress - header.data) {
                        break;
                    }
                    std::uintptr_t address = 0;
                    if (ReadValue(header.data + offset, address)) {
                        addresses[index] = address;
                    } else {
                        addresses[index] = 0;
                    }
                }
            }
            for (const std::uintptr_t address : addresses) {
                if (IsValidPointer(address) && seen.insert(address).second) {
                    result.push_back(address);
                    if (result.size() >= kMaximumCollectedActors) break;
                }
            }
        };

        appendArray(
            ActorArrayHeader{
                context.actorArray,
                context.actorCount,
                context.actorCount,
            },
            kMaximumActorCount);
        if (result.size() >= kMaximumCollectedActors) {
            return result;
        }

        return result;
    }

    std::vector<std::uintptr_t> CollectWorldObjectAddresses(
        const FrameContext& context) {
        constexpr std::size_t kMaximumCollectedActors = 100000;
        std::vector<std::uintptr_t> result;
        std::unordered_set<std::uintptr_t> seen;
        result.reserve(4096);
        seen.reserve(4096);

        const auto appendArray = [&](const ActorArrayHeader& header,
                                     std::int32_t maximumCount) {
            if (!IsValidPointer(header.data) || header.count <= 0 ||
                header.count > maximumCount || header.capacity < header.count ||
                result.size() >= kMaximumCollectedActors) {
                return;
            }
            std::vector<std::uintptr_t> addresses(
                static_cast<std::size_t>(header.count));
            if (!memory_->Read(
                    header.data,
                    addresses.data(),
                    addresses.size() * sizeof(std::uintptr_t))) {
                for (std::size_t index = 0; index < addresses.size(); ++index) {
                    const std::uintptr_t offset =
                        index * sizeof(std::uintptr_t);
                    if (offset > kMaximumRemoteAddress - header.data) break;
                    ReadValue(header.data + offset, addresses[index]);
                }
            }
            for (const std::uintptr_t address : addresses) {
                if (IsValidPointer(address) && seen.insert(address).second) {
                    result.push_back(address);
                    if (result.size() >= kMaximumCollectedActors) break;
                }
            }
        };

        appendArray(
            ActorArrayHeader{
                context.actorArray,
                context.actorCount,
                context.actorCount,
            },
            kMaximumActorCount);
        ActorArrayHeader persistentObjects{};
        if (ReadValue(context.level + 0x98, persistentObjects)) {
            appendArray(persistentObjects, kMaximumWorldObjectCount);
        }
        if (result.size() >= kMaximumCollectedActors) return result;

        const std::uintptr_t levels = ReadPointer(context.world + 0x158);
        std::int32_t levelCount = 0;
        if (!IsValidPointer(levels) ||
            !ReadValue(context.world + 0x160, levelCount) ||
            levelCount <= 0 || levelCount > 1024) {
            return result;
        }
        std::vector<std::uintptr_t> levelAddresses(
            static_cast<std::size_t>(levelCount));
        if (!memory_->Read(
                levels,
                levelAddresses.data(),
                levelAddresses.size() * sizeof(std::uintptr_t))) {
            for (std::size_t index = 0;
                 index < levelAddresses.size();
                 ++index) {
                const std::uintptr_t offset =
                    index * sizeof(std::uintptr_t);
                if (offset > kMaximumRemoteAddress - levels) break;
                ReadValue(levels + offset, levelAddresses[index]);
            }
        }
        for (const std::uintptr_t level : levelAddresses) {
            if (!IsValidPointer(level)) continue;
            ActorArrayHeader actors{};
            if (ReadValue(level + 0x1F0, actors)) {
                appendArray(actors, kMaximumWorldObjectCount);
            }
            ActorArrayHeader objects{};
            if (ReadValue(level + 0x98, objects)) {
                appendArray(objects, kMaximumWorldObjectCount);
            }
            if (result.size() >= kMaximumCollectedActors) break;
        }
        return result;
    }

    std::string ReadUtf16String(std::uintptr_t address, std::size_t maximum = 96) {
        if (!IsValidPointer(address) || maximum == 0 || maximum > 512) return {};
        std::vector<char16_t> buffer(maximum, 0);
        if (!memory_->Read(address, buffer.data(), buffer.size() * sizeof(char16_t))) {
            return {};
        }
        const auto end = std::find(buffer.begin(), buffer.end(), u'\0');
        return Utf16ToUtf8(std::u16string(buffer.begin(), end));
    }

    std::uint64_t ReadStructuredItemId(std::uintptr_t address) {
        std::uint64_t raw = 0;
        if (!ReadValue(address, raw)) return 0;
        const std::uint32_t category = static_cast<std::uint32_t>(raw);
        const std::uint32_t sequence = static_cast<std::uint32_t>(raw >> 32U);
        return category == 0
            ? 0
            : static_cast<std::uint64_t>(category) * 10000ULL + sequence;
    }

    struct ItemInfo {
        std::uint64_t id = 0;
        std::string name;
        int rarity = 0;
        int value = 0;
    };

    std::string ReadDefinitionName(std::uintptr_t definition) {
        if (!IsValidPointer(definition)) return {};
        const std::uintptr_t first = ReadPointer(definition + 0x18);
        const std::uintptr_t second = ReadPointer(first + 0x18);
        const std::uintptr_t third = ReadPointer(second + 0x28);
        return ReadUtf16String(ReadPointer(third + 0x10));
    }

    ItemInfo ReadPickupItem(std::uintptr_t actor) {
        ItemInfo item{};
        const std::uintptr_t definition = ReadPointer(actor + 0x11A8);
        if (IsValidPointer(definition)) {
            ReadValue(definition + 0x68, item.rarity);
            ReadValue(definition + 0xDC, item.value);
            item.name = ReadDefinitionName(definition);
        }
        item.id = ReadStructuredItemId(actor + 0x11F8);
        if (item.name.empty()) {
            if (const char* mapped = data::FindItemName(item.id)) {
                item.name = mapped;
            }
        }
        if (item.id != 0 && item.rarity >= 1 && item.rarity <= 6 &&
            item.value > 0 && item.value < 100000000) {
            itemMetadata_[item.id] = std::pair<int, int>{item.rarity, item.value};
        }
        return item;
    }

    struct ContainerItem {
        std::string name;
        int count = 0;
        int rarity = 0;
        int value = 0;
    };

    std::vector<ContainerItem> ReadContainerItems(
        std::uintptr_t actor,
        int minimumValue,
        int minimumRarity,
        int maximumItems) {
        constexpr std::uintptr_t kItemsArrayOffset = 0x1E28 + 0x108;
        constexpr std::uintptr_t kItemStride = 0x7D8;
        constexpr std::uintptr_t kItemIdOffset = 0x10;
        constexpr std::uintptr_t kItemCountOffset = 0x38;

        const std::uintptr_t arrayAddress = actor + kItemsArrayOffset;
        const std::uintptr_t items = ReadPointer(arrayAddress);
        std::int32_t count = 0;
        if (!ReadValue(arrayAddress + 0x8, count) || !IsValidPointer(items) ||
            count <= 0 || count > 512) {
            return {};
        }

        std::vector<ContainerItem> result;
        result.reserve(static_cast<std::size_t>(std::min(count, maximumItems)));
        for (int index = 0; index < count; ++index) {
            const std::uintptr_t entry = items + kItemStride * index;
            const std::uint64_t itemId = ReadStructuredItemId(entry + kItemIdOffset);
            std::int32_t itemCount = 0;
            ReadValue(entry + kItemCountOffset, itemCount);
            if (itemId == 0 || itemCount <= 0) continue;

            const char* mapped = data::FindItemName(itemId);
            if (mapped == nullptr || *mapped == '\0') continue;
            int rarity = 0;
            int value = 0;
            const auto metadata = itemMetadata_.find(itemId);
            if (metadata != itemMetadata_.end()) {
                rarity = metadata->second.first;
                value = metadata->second.second;
                if (value < minimumValue || rarity < minimumRarity) continue;
            }
            result.push_back(ContainerItem{mapped, itemCount, rarity, value});
        }
        std::stable_sort(result.begin(), result.end(),
            [](const ContainerItem& left, const ContainerItem& right) {
                return left.value != right.value
                    ? left.value > right.value
                    : left.rarity > right.rarity;
            });
        if (maximumItems > 0 && result.size() > static_cast<std::size_t>(maximumItems)) {
            result.resize(static_cast<std::size_t>(maximumItems));
        }
        return result;
    }

    static std::optional<ui::ContainerKind> ContainerKindForClass(
        std::string_view name) {
        using Kind = ui::ContainerKind;
        if (name.find("Computer") != std::string_view::npos) return Kind::Computer;
        if (name.find("Weapon") != std::string_view::npos) return Kind::WeaponCrate;
        if (name.find("Tool") != std::string_view::npos) return Kind::LargeToolbox;
        if (name.find("Ammo") != std::string_view::npos) return Kind::AmmoCrate;
        if (name.find("Locker") != std::string_view::npos) return Kind::Locker;
        if (name.find("Mountain") != std::string_view::npos) return Kind::MountainPack;
        if (name.find("Capsule") != std::string_view::npos) return Kind::ReturnCapsule;
        if (name.find("Storage") != std::string_view::npos) return Kind::StorageBox;
        if (name.find("Briefcase") != std::string_view::npos) return Kind::Briefcase;
        if (name.find("Safe") != std::string_view::npos) return Kind::Safe;
        if (name.find("Lab") != std::string_view::npos) return Kind::LabCoat;
        if (name.find("Mixer") != std::string_view::npos) return Kind::MixerTruck;
        if (name.find("Truck") != std::string_view::npos) return Kind::ContainerTruck;
        if (name.find("Wire") != std::string_view::npos) return Kind::WireFence;
        if (name.find("Password") != std::string_view::npos) return Kind::PasswordDoor;
        if (name.find("Switch") != std::string_view::npos) return Kind::Switch;
        if (name.find("Recon") != std::string_view::npos) return Kind::Recon;
        if (name.find("Exit") != std::string_view::npos ||
            name.find("Extract") != std::string_view::npos) return Kind::ExtractionPoint;
        return std::nullopt;
    }

    static std::optional<ui::ContainerKind> ContainerKindForDisplayName(
        std::string_view name) {
        using Kind = ui::ContainerKind;
        if (name == "查看行动") return Kind::MissionTerminal;
        if (name == "楼梯") return Kind::Stairs;
        if (name == "保险柜") return Kind::Vault;
        if (name == "密码锁") return Kind::CombinationLock;
        return std::nullopt;
    }

    static bool IsContainerKindEnabled(
        const ui::LootSettings& settings,
        const std::optional<ui::ContainerKind>& kind) {
        if (!kind.has_value()) return false;
        const std::size_t index = static_cast<std::size_t>(*kind);
        return index < settings.containerKinds.size() && settings.containerKinds[index];
    }

    static std::string ContainerDetail(
        float distanceMeters,
        const std::vector<ContainerItem>& items) {
        std::string detail = FormatDistance(distanceMeters);
        const std::size_t count = std::min<std::size_t>(items.size(), 3);
        for (std::size_t index = 0; index < count; ++index) {
            if (!detail.empty()) detail += "  ";
            detail += items[index].name;
            if (items[index].count > 1) {
                detail += " x" + std::to_string(items[index].count);
            }
        }
        return detail;
    }

    void ProcessWorldActor(
        std::uintptr_t actor,
        const std::string& className,
        const FrameContext& context,
        const FeatureSettings& settings,
        GameFrame& frame,
        std::unordered_set<std::uintptr_t>& seenThreats,
        bool processStaticObjects,
        bool processThreats,
        bool deferStaticProjection = false,
        std::vector<WorldLabelProjectionSource>* labelSources = nullptr) {
        Vec3 position{};
        if (!ReadActorPosition(actor, position, settings.visual.antiFlicker)) return;
        const float distanceMeters = Length(Subtract(position, context.localPosition)) * 0.01f;
        if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) return;

        Vec2 screen{};
        const bool projectionNeeded =
            !deferStaticProjection || processThreats;
        const bool projected = projectionNeeded &&
            ProjectToScreen(
                position,
                context.preparedProjection,
                screen);
        const bool suppressLoot = settings.visual.combatMode &&
            (context.firing || context.zooming);
        const bool staticProjectionAccepted = deferStaticProjection ||
            (projected && IsInsideScreen(
                screen, options_.screenWidth, options_.screenHeight, 40.0f));
        const auto appendWorldLabel = [&](WorldLabel label) {
            if (labelSources != nullptr) {
                labelSources->push_back(WorldLabelProjectionSource{
                    frame.worldLabels.size(),
                    position,
                });
            }
            frame.worldLabels.push_back(std::move(label));
        };

        if (processStaticObjects && settings.visual.enabled &&
            settings.visual.classNameDebug && !className.empty() &&
            distanceMeters <= static_cast<float>(settings.visual.drawDistanceMeters) &&
            staticProjectionAccepted &&
            frame.worldLabels.size() < 2048) {
            WorldLabel label{};
            label.anchor = ImVec2(screen.x, screen.y);
            label.title = className;
            label.detail = FormatDistance(distanceMeters);
            label.kind = WorldLabelKind::Container;
            label.tone = SemanticTone::Muted;
            appendWorldLabel(std::move(label));
        }

        if (processStaticObjects && !suppressLoot && settings.loot.password &&
            distanceMeters <= static_cast<float>(settings.loot.containerDistanceMeters) &&
            staticProjectionAccepted) {
            std::int32_t password = 0;
            if (className.find("Computer") != std::string::npos) {
                ReadValue(actor + 0xFEC, password);
            } else if (className.find("Password") != std::string::npos ||
                       className.find("Combination") != std::string::npos) {
                ReadValue(actor + 0x1048, password);
            }
            if (password > 0 && password <= 10000) {
                appendWorldLabel(WorldLabel{
                    ImVec2(screen.x, screen.y),
                    "密码 " + std::to_string(password),
                    FormatDistance(distanceMeters),
                    WorldLabelKind::Container,
                    SemanticTone::Caution,
                    true,
                });
            }
        }

        const std::optional<data::CustomItemEntry> customItem =
            processStaticObjects && settings.loot.enabled
            ? customItems_.Match(className)
            : std::optional<data::CustomItemEntry>{};
        if (processStaticObjects && !suppressLoot &&
            (IsPickupClass(className) || customItem.has_value()) &&
            settings.loot.enabled &&
            distanceMeters <= static_cast<float>(settings.loot.itemDistanceMeters)) {
            ItemInfo item = ReadPickupItem(actor);
            if (item.name.empty() && customItem.has_value()) {
                item.name = customItem->displayName;
                item.rarity = std::max(item.rarity, 1);
            }
            if (!item.name.empty() && item.value >= settings.loot.minimumItemValue &&
                item.rarity >= settings.loot.minimumItemRarity &&
                staticProjectionAccepted) {
                WorldLabel label{};
                label.anchor = ImVec2(screen.x, screen.y);
                label.title = item.name;
                label.detail = "价值 " + std::to_string(std::max(0, item.value)) +
                    "  " + FormatDistance(distanceMeters);
                label.kind = WorldLabelKind::Item;
                label.tone = ToneForRarity(item.rarity);
                label.emphasized = item.rarity >= 5;
                if (customItem.has_value()) {
                    label.colorOverride = customItem->color;
                    label.titleSizeOverride = customItem->fontSize;
                }
                appendWorldLabel(std::move(label));
            }
            if (settings.loot.highValueList && !item.name.empty() &&
                item.value >= settings.loot.minimumListValue &&
                item.rarity >= settings.loot.minimumListRarity) {
                if (!frame.highValueList.has_value()) {
                    HighValueList list{};
                    list.origin = ImVec2(
                        std::max(12.0f, static_cast<float>(options_.screenWidth) - 332.0f),
                        76.0f);
                    list.width = 310.0f;
                    list.maxRows = std::clamp(settings.loot.listLimit, 1, 50);
                    list.title = "高价值物资";
                    frame.highValueList = std::move(list);
                }
                frame.highValueList->entries.push_back(HighValueEntry{
                    item.name,
                    item.value,
                    distanceMeters,
                    ToneForRarity(item.rarity),
                });
            }
        }

        if (processStaticObjects && !suppressLoot && IsDeadBodyClass(className) &&
            (settings.loot.playerBox || settings.loot.botBox) &&
            distanceMeters <= static_cast<float>(settings.loot.containerDistanceMeters) &&
            staticProjectionAccepted) {
            std::uint8_t looted = 0;
            ReadValue(actor + 0x2678, looted);
            if (looted == 0) {
                const std::uintptr_t owner = ReadPointer(actor + 0x2668);
                const bool bot = !IsValidPointer(owner) || !IsValidPointer(ReadPointer(owner + 0x390));
                if ((!bot && settings.loot.playerBox) || (bot && settings.loot.botBox)) {
                    std::vector<ContainerItem> items;
                    if (settings.loot.boxContents) {
                        items = ReadContainerItems(
                            actor,
                            settings.loot.minimumContainerValue,
                            settings.loot.minimumContainerRarity,
                            8);
                    }
                    WorldLabel label{};
                    label.anchor = ImVec2(screen.x, screen.y);
                    label.title = bot ? "人机盒子" : "玩家盒子";
                    label.detail = ContainerDetail(distanceMeters, items);
                    label.kind = WorldLabelKind::Container;
                    label.tone = bot ? SemanticTone::Muted : SemanticTone::Ally;
                    appendWorldLabel(std::move(label));
                }
            }
        }

        if (processStaticObjects && !suppressLoot && settings.loot.containers) {
            if (IsExtractionClass(className)) {
                const auto kind = ui::ContainerKind::ExtractionPoint;
                if (IsContainerKindEnabled(settings.loot, kind) &&
                    distanceMeters <=
                        static_cast<float>(settings.loot.containerDistanceMeters) &&
                    staticProjectionAccepted) {
                    appendWorldLabel(WorldLabel{
                        ImVec2(screen.x, screen.y),
                        "撤离点",
                        FormatDistance(distanceMeters),
                        WorldLabelKind::Container,
                        SemanticTone::Ally,
                        true,
                    });
                }
            }

            std::int32_t containerId = 0;
            float detectRatio = 0.0f;
            ReadValue(actor + 0x2378, containerId);
            ReadValue(actor + 0x720, detectRatio);
            const bool legacyCandidate = detectRatio == 200.0f ||
                detectRatio == 360.0f || detectRatio == 20.0f;
            const bool containerClass = className.find("B_neat") != std::string::npos;
            const bool knownContainer = IsKnownContainerId(containerId);
            const bool preciseContainer = knownContainer && containerClass;
            if (legacyCandidate &&
                distanceMeters <= static_cast<float>(settings.loot.containerDistanceMeters) &&
                staticProjectionAccepted) {
                std::uint8_t opened = 0;
                if (preciseContainer) {
                    ReadValue(actor + 0x2384, opened);
                }
                const std::string displayName = knownContainer
                    ? std::string(ContainerName(containerId))
                    : ReadDefinitionName(ReadPointer(actor + 0x11A8));
                auto kind = ContainerKindForId(containerId);
                if (!kind.has_value()) {
                    kind = ContainerKindForDisplayName(displayName);
                }
                if (!kind.has_value()) {
                    kind = ContainerKindForClass(className);
                }
                if (opened == 0 && IsContainerKindEnabled(settings.loot, kind)) {
                    std::vector<ContainerItem> items;
                    if (settings.loot.containerContents) {
                        items = ReadContainerItems(
                            actor,
                            settings.loot.minimumContainerValue,
                            settings.loot.minimumContainerRarity,
                            8);
                    }
                    WorldLabel label{};
                    label.anchor = ImVec2(screen.x, screen.y);
                    label.title = displayName.empty() ? "容器" : displayName;
                    label.detail = ContainerDetail(distanceMeters, items);
                    label.kind = WorldLabelKind::Container;
                    label.tone = SemanticTone::Accent;
                    appendWorldLabel(std::move(label));
                }
            }
        }

        const data::ThreatObjectInfo* threat = data::FindThreatObject(className);
        if (processThreats && settings.visual.enabled &&
            (settings.visual.throwableWarning ||
             settings.visual.throwableTrajectory) &&
            threat != nullptr &&
            distanceMeters <= 50.0f) {
            seenThreats.insert(actor);
            const auto now = std::chrono::steady_clock::now();
            const auto [firstSeen, inserted] = threatFirstSeen_.try_emplace(actor, now);
            (void)inserted;
            const bool expired =
                std::chrono::duration<float>(now - firstSeen->second).count() > 10.0f;
            if (!expired && settings.visual.throwableTrajectory) {
                auto& trail = projectileTrails_[actor];
                if (trail.empty() || Length(Subtract(trail.back(), position)) > 5.0f) {
                    trail.push_back(position);
                }
                if (trail.size() > 140) {
                    trail.erase(trail.begin(), trail.begin() + (trail.size() - 140));
                }
            }
            if (!expired && settings.visual.throwableWarning) {
                frame.dangerousObjectNearby = true;
            }
            if (!expired && projected) {
                ProjectileVisual projectile{};
                projectile.center = ImVec2(screen.x, screen.y);
                projectile.label = settings.visual.throwableWarning
                    ? std::string(threat->displayName)
                    : std::string{};
                projectile.distanceMeters = settings.visual.throwableWarning
                    ? distanceMeters
                    : -1.0f;
                projectile.tone = SemanticTone::Danger;
                projectile.colorOverride = IM_COL32(
                    threat->red,
                    threat->green,
                    threat->blue,
                    threat->alpha);
                if (settings.visual.throwableWarning) {
                    const std::vector<native::ScreenProjectionSegment>
                        rangeSegments = native::ProjectWorldHorizontalRing(
                            native::ProjectionPoint{
                                position.x, position.y, position.z},
                            threat->radiusCentimeters,
                            context.preparedProjection);
                    projectile.rangeSegments.reserve(rangeSegments.size());
                    for (const native::ScreenProjectionSegment& segment :
                         rangeSegments) {
                        projectile.rangeSegments.push_back(
                            GeometrySegmentVisual{
                                ImVec2(segment.start.x, segment.start.y),
                                ImVec2(segment.end.x, segment.end.y),
                            });
                    }
                }
                if (settings.visual.throwableTrajectory) {
                    const auto found = projectileTrails_.find(actor);
                    if (found != projectileTrails_.end()) {
                        for (const Vec3& point : found->second) {
                            Vec2 projectedPoint{};
                            if (ProjectToScreen(
                                    point,
                                    context.preparedProjection,
                                    projectedPoint)) {
                                projectile.trajectory.emplace_back(
                                    projectedPoint.x, projectedPoint.y);
                            }
                        }
                    }
                }
                frame.projectiles.push_back(std::move(projectile));
            }
        }
    }

    static bool StaticWorldFeaturesEnabled(
        const FeatureSettings& settings) {
        return settings.loot.enabled || settings.loot.playerBox ||
            settings.loot.botBox || settings.loot.password ||
            settings.loot.containers || settings.loot.highValueList ||
            (settings.visual.enabled && settings.visual.classNameDebug);
    }

    static std::uint64_t WorldObjectSettingsSignature(
        const FeatureSettings& settings) {
        std::uint64_t signature = 1469598103934665603ULL;
        const auto mix = [&signature](std::uint64_t value) {
            signature ^= value;
            signature *= 1099511628211ULL;
        };
        mix(settings.loot.enabled);
        mix(settings.loot.playerBox);
        mix(settings.loot.botBox);
        mix(settings.loot.password);
        mix(settings.loot.containers);
        mix(settings.loot.boxContents);
        mix(settings.loot.containerContents);
        mix(settings.loot.highValueList);
        mix(static_cast<std::uint64_t>(settings.loot.itemDistanceMeters));
        mix(static_cast<std::uint64_t>(settings.loot.minimumItemValue));
        mix(static_cast<std::uint64_t>(settings.loot.minimumItemRarity));
        mix(static_cast<std::uint64_t>(settings.loot.containerDistanceMeters));
        mix(static_cast<std::uint64_t>(settings.loot.minimumContainerValue));
        mix(static_cast<std::uint64_t>(settings.loot.minimumContainerRarity));
        mix(static_cast<std::uint64_t>(settings.loot.listLimit));
        mix(static_cast<std::uint64_t>(settings.loot.minimumListValue));
        mix(static_cast<std::uint64_t>(settings.loot.minimumListRarity));
        mix(settings.visual.enabled);
        mix(settings.visual.classNameDebug);
        mix(static_cast<std::uint64_t>(settings.visual.drawDistanceMeters));
        for (const bool enabled : settings.loot.containerKinds) {
            mix(enabled);
        }
        return signature;
    }

    bool IsStaticWorldObjectCandidate(
        std::uintptr_t actor,
        const std::string& className,
        bool characterClass,
        const FeatureSettings& settings) {
        if (className.empty() || characterClass) return false;
        if (settings.visual.enabled && settings.visual.classNameDebug) {
            return true;
        }
        if (settings.loot.password &&
            (className.find("Computer") != std::string::npos ||
             className.find("Password") != std::string::npos ||
             className.find("Combination") != std::string::npos)) {
            return true;
        }
        if (settings.loot.enabled &&
            (IsPickupClass(className) ||
             customItems_.Match(className).has_value())) {
            return true;
        }
        if ((settings.loot.playerBox || settings.loot.botBox) &&
            IsDeadBodyClass(className)) {
            return true;
        }
        if (!settings.loot.containers) return false;
        if (IsExtractionClass(className) ||
            className.find("B_neat") != std::string::npos) {
            return true;
        }
        std::int32_t containerId = 0;
        float detectRatio = 0.0f;
        ReadValue(actor + 0x2378, containerId);
        ReadValue(actor + 0x720, detectRatio);
        return IsKnownContainerId(containerId) || detectRatio == 200.0f ||
            detectRatio == 360.0f || detectRatio == 20.0f;
    }

    void RefreshWorldObjectCache(
        const FrameContext& context,
        const FeatureSettings& settings) {
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::WorldObjectRefresh);
        if (!StaticWorldFeaturesEnabled(settings)) {
            worldObjectFrameCache_.Clear();
            worldObjectActors_.clear();
            worldObjectDiscoveryPolicy_.Invalidate();
            worldObjectContentPolicy_.Invalidate();
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const std::uint64_t signature =
            WorldObjectSettingsSignature(settings);
        FeatureSettings staticSettings = settings;
        staticSettings.visual.combatMode = false;
        if (worldObjectDiscoveryPolicy_.ShouldRefresh(
                context.world, signature, now)) {
            std::vector<CachedWorldActor> discovered;
            const std::vector<std::uintptr_t> actors =
                CollectWorldObjectAddresses(context);
            discovered.reserve(std::min<std::size_t>(actors.size(), 2048));
            for (const std::uintptr_t actor : actors) {
                std::int32_t nameIndex = -1;
                if (!ReadValue(actor + 0x1C, nameIndex) || nameIndex < 0) {
                    continue;
                }
                std::string className =
                    DecodeName(nameIndex, context.namePool);
                const FNameClassTraits classTraits =
                    ResolveClassTraits(nameIndex, className);
                if (!IsStaticWorldObjectCandidate(
                        actor,
                        className,
                        classTraits.character,
                        staticSettings)) {
                    continue;
                }
                discovered.push_back(CachedWorldActor{
                    actor,
                    std::move(className),
                });
            }
            worldObjectActors_ = std::move(discovered);
            worldObjectDiscoveryPolicy_.MarkRefreshed(
                context.world, signature, now);
            worldObjectContentPolicy_.Invalidate();
        }
        if (!worldObjectContentPolicy_.ShouldRefresh(
                context.world, signature, now)) {
            return;
        }

        GameFrame refreshed{};
        std::vector<WorldLabelProjectionSource> labelSources;
        std::unordered_set<std::uintptr_t> ignoredThreats;
        labelSources.reserve(
            std::min<std::size_t>(worldObjectActors_.size(), 2048));
        for (const CachedWorldActor& actor : worldObjectActors_) {
            ProcessWorldActor(
                actor.actor,
                actor.className,
                context,
                staticSettings,
                refreshed,
                ignoredThreats,
                true,
                false,
                true,
                &labelSources);
        }
        if (refreshed.highValueList.has_value()) {
            auto& entries = refreshed.highValueList->entries;
            std::stable_sort(
                entries.begin(), entries.end(),
                [](const HighValueEntry& left,
                   const HighValueEntry& right) {
                    return left.value != right.value
                        ? left.value > right.value
                        : left.distanceMeters < right.distanceMeters;
                });
            if (entries.size() >
                static_cast<std::size_t>(refreshed.highValueList->maxRows)) {
                entries.resize(static_cast<std::size_t>(
                    refreshed.highValueList->maxRows));
            }
        }
        worldObjectFrameCache_.labels = std::move(refreshed.worldLabels);
        worldObjectFrameCache_.labelSources = std::move(labelSources);
        worldObjectFrameCache_.highValueList =
            std::move(refreshed.highValueList);
        worldObjectContentPolicy_.MarkRefreshed(
            context.world, signature, now);
    }

    void AppendWorldObjectCache(
        const FeatureSettings& settings,
        const FrameContext& context,
        GameFrame& frame) const {
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::WorldObjects);
        if (settings.visual.combatMode &&
            (context.firing || context.zooming)) {
            return;
        }
        for (const WorldLabelProjectionSource& source :
             worldObjectFrameCache_.labelSources) {
            if (source.labelIndex >= worldObjectFrameCache_.labels.size() ||
                frame.worldLabels.size() >= 2048) {
                continue;
            }
            Vec2 screen{};
            if (!ProjectToScreen(
                    source.world,
                    context.preparedProjection,
                    screen) ||
                !IsInsideScreen(
                    screen,
                    options_.screenWidth,
                    options_.screenHeight,
                    40.0f)) {
                continue;
            }
            WorldLabel label =
                worldObjectFrameCache_.labels[source.labelIndex];
            label.anchor = ImVec2(screen.x, screen.y);
            frame.worldLabels.push_back(std::move(label));
        }
        if (worldObjectFrameCache_.highValueList.has_value()) {
            frame.highValueList = worldObjectFrameCache_.highValueList;
        }
    }

    bool BuildFrameContext(FrameContext& context,
                           std::uint64_t frameSequence,
                           bool battlefieldMode,
                           native::PositionReadMode positionMode,
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                           bool trajectoryTracking,
#endif
                           bool antiFlicker,
                           RuntimeDiagnostic& failure,
                           std::string& diagnostic) {
        failure = {};
        if (!IsValidPointer(moduleBase_)) {
            failure.error = RuntimeError::ModuleAddressInvalid;
            diagnostic = "数据链等待：模块地址无效";
            return false;
        }
        context.namePool = moduleBase_ + layout_.namePoolOffset;
        const std::uintptr_t worldAddress = moduleBase_ + layout_.worldOffset;
        if (!IsValidPointer(context.namePool) || !IsValidPointer(worldAddress)) {
            failure.error = RuntimeError::WorldEntryInvalid;
            diagnostic = "数据链等待：世界入口无效";
            return false;
        }

        context.world = ReadPointer(worldAddress);
        if (!IsValidPointer(context.world)) {
            failure.error = RuntimeError::WorldUnavailable;
            diagnostic = "数据链等待：世界对象暂不可读";
            return false;
        }
        context.level = ReadPointer(context.world + 0xF8);
        if (!IsValidPointer(context.level)) {
            failure.error = RuntimeError::LevelUnavailable;
            diagnostic = "数据链等待：关卡对象暂不可读";
            return false;
        }

        ActorArrayHeader actors{};
        const bool ordinaryActorsAvailable =
            ReadValue(context.level + 0x1F0, actors) &&
            IsValidPointer(actors.data) && actors.count > 0 &&
            actors.count <= kMaximumActorCount &&
            actors.capacity >= actors.count;
        if (!ordinaryActorsAvailable) {
            failure.error = RuntimeError::ActorSourceUnavailable;
            diagnostic = "数据链等待：人物数组暂不可读";
            return false;
        }
        context.actorArray = actors.data;
        context.actorCount = actors.count;
        const std::uintptr_t gameInstance = ReadPointer(context.world + 0x190);
        const std::uintptr_t localPlayers = ReadPointer(gameInstance + 0x38);
        const std::uintptr_t localPlayer = ReadPointer(localPlayers);
        context.localController = ReadPointer(localPlayer + 0x30);
        context.localPawn = ReadPointer(context.localController + 0x3F0);
        context.cameraManager = ReadPointer(context.localController + 0x408);
        const bool decodedRequired =
            native::ShouldRequireDecodedActorRecords(
                positionMode,
                hardwareBreakpointRequested_,
#if LENGJING_ENABLE_PROJECTILE_TRACKING
                trajectoryTracking
#else
                false
#endif
            );
        RefreshActorRecordSnapshot(context, decodedRequired);
        if (decodedRequired && !context.decodedRecordSourceReady) {
            failure.error = RuntimeError::ActorSourceUnavailable;
            diagnostic = "数据链等待：人物列表暂不可读";
            return false;
        }
        if (actorRecordSnapshot_.records.empty()) {
            failure.error = RuntimeError::ActorSourceUnavailable;
            diagnostic = "数据链等待：人物数组暂不可读";
            return false;
        }
        if (!IsValidPointer(context.localController) || !IsValidPointer(context.localPawn) ||
            !IsValidPointer(context.cameraManager)) {
            failure.error = RuntimeError::LocalViewUnavailable;
            diagnostic = "数据链等待：本地角色或相机暂不可读";
            return false;
        }

        const std::uintptr_t mapState = ReadPointer(context.world + 0x140);
        std::int32_t mapBuild = 0;
        if (IsValidPointer(mapState)) ReadValue(mapState + 0x6E8, mapBuild);
        context.mapBuildId = mapBuild;
        context.warfare = mapBuild > 9000;

        const std::uintptr_t localState = ReadPointer(context.localPawn + 0x390);
        std::int32_t primaryTeam = native::kUnknownPlayerTeam;
        std::int32_t secondaryTeam = native::kUnknownPlayerTeam;
        if (IsValidPointer(localState)) {
            ReadValue(localState + 0x658, primaryTeam);
            ReadValue(localState + 0x65C, secondaryTeam);
        }
        context.localPrimaryTeam = primaryTeam > 0
            ? primaryTeam
            : native::kUnknownPlayerTeam;
        context.localSecondaryTeam = secondaryTeam > 0
            ? secondaryTeam
            : native::kUnknownPlayerTeam;
        context.localTeam = (context.warfare || battlefieldMode)
            ? context.localSecondaryTeam
            : context.localPrimaryTeam;

        const std::uintptr_t blackboard = ReadPointer(context.localPawn + 0x2430);
        std::uint8_t zooming = 0;
        std::uint8_t firing = 0;
        if (IsValidPointer(blackboard)) {
            ReadValue(blackboard + 0x5AC, zooming);
            ReadValue(blackboard + 0x5AD, firing);
        }
        context.zooming = zooming != 0;
        context.firing = firing != 0;
        context.weaponRoot = ReadPointer(context.localPawn + 0x1790);
        if (IsValidPointer(context.weaponRoot)) {
            std::uint32_t weaponId = 0;
            if (ReadValue(context.weaponRoot + 0x838, weaponId)) {
                context.weaponId = weaponId;
            }
        }

        if (!ReadStableCameraView(
                context.world,
                context.cameraManager,
                frameSequence,
                context.view)) {
            failure.error = RuntimeError::CameraUnavailable;
            diagnostic = "数据链等待：相机数据暂不可读";
            return false;
        }
        context.preparedProjection = PrepareProjection(
            context.view, options_.screenWidth, options_.screenHeight);
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        context.firingOriginValid = trajectoryTracking &&
            ResolveTrackingOrigin(context, context.firingOrigin);
#endif

        const auto localRecord = std::find_if(
            actorRecordSnapshot_.records.begin(),
            actorRecordSnapshot_.records.end(),
            [&context](const RuntimeActorRecord& record) {
                return record.actor == context.localPawn &&
                    record.resolverRecord;
            });

        std::int32_t localNameIndex = -1;
        ReadValue(context.localPawn + 0x1C, localNameIndex);
        const std::string localClassName =
            DecodeName(localNameIndex, context.namePool);
        std::uintptr_t localRoot = 0;
        if (positionMode == native::PositionReadMode::Direct &&
            localRecord != actorRecordSnapshot_.records.end()) {
            localRoot = localRecord->root;
        }
        if (!ReadCharacterPosition(
                context.localPawn,
                localRoot,
                localClassName,
                positionMode,
                antiFlicker,
                context.localPosition,
                true)) {
            context.localPosition = context.view.location;
        }
        const std::uintptr_t verifiedWorld = ReadPointer(worldAddress);
        if (verifiedWorld != context.world) {
            failure.error = RuntimeError::WorldTransition;
            diagnostic = "数据链等待：世界对象正在切换";
            return false;
        }
        diagnostic.clear();
        return true;
    }

#if 0
    bool StartHardwareBreakpointRuntime() {
        hardwareBreakpointFailure_ = {};
        if (memory_ == nullptr || !memory_->IsOpen() ||
            !memory_->SupportsExecutionBreakpoints()) {
            hardwareBreakpointFailure_.error =
                CoordinateDecryptError::MemoryTransportUnavailable;
            return false;
        }

        const CoordinateExecutionProfile* profile =
            FindCoordinateExecutionProfile(moduleBuildId_);
        if (profile == nullptr ||
            profile->firstVeneerRva == 0 ||
            (profile->firstVeneerRva & 3U) != 0 ||
            moduleBase_ >
                std::numeric_limits<std::uintptr_t>::max() -
                    profile->firstVeneerRva) {
            hardwareBreakpointFailure_.error =
                CoordinateDecryptError::InvalidConfiguration;
            return false;
        }

        const std::uintptr_t firstVeneerAddress =
            moduleBase_ + profile->firstVeneerRva;
        if (!IsMappedNamedExecutableAddress(
                processId_, firstVeneerAddress, "libUE4.so")) {
            hardwareBreakpointFailure_.error =
                CoordinateDecryptError::EntryMappingMissing;
            return false;
        }

        const native::ExecutionVeneerReadMemory readMemory =
            [this](std::uintptr_t address,
                   void* destination,
                   std::size_t size) {
                return memory_ != nullptr && destination != nullptr &&
                    size != 0 && IsValidReadAddress(address) &&
                    size <= kMaximumRemoteAddress - address &&
                    memory_->Read(address, destination, size);
            };
        std::uintptr_t breakpointAddress = 0;
        if (!native::LocateSecondExecutionVeneer(
                moduleBase_,
                profile->firstVeneerRva,
                readMemory,
                breakpointAddress)) {
            hardwareBreakpointFailure_.error =
                CoordinateDecryptError::EntryResolveFailed;
            return false;
        }
        if (!IsMappedExecutableAddress(
                processId_, breakpointAddress)) {
            hardwareBreakpointFailure_.error =
                CoordinateDecryptError::EntryMappingMissing;
            return false;
        }

        native::HardwareBreakpointCoordinateCallbacks callbacks{
            [this](std::uintptr_t address) {
                return memory_ != nullptr &&
                    memory_->ConfigureExecutionBreakpoint(address);
            },
            [this](native::ExecutionBreakpointRecord* records,
                   std::size_t capacity,
                   std::size_t& recordsRead,
                   std::uintptr_t& hitAddress,
                   std::size_t& totalRecords) {
                return memory_ != nullptr &&
                    memory_->ReadExecutionBreakpointRecords(
                        records, capacity, recordsRead, hitAddress,
                        totalRecords);
            },
            [readMemory](std::uintptr_t address,
                         void* destination,
                         std::size_t size) {
                return readMemory(address, destination, size);
            },
            [this] {
                return memory_ != nullptr &&
                    memory_->RemoveExecutionBreakpoints();
            },
        };
        if (!hardwareBreakpointRuntime_.Start(
                breakpointAddress, std::move(callbacks))) {
            hardwareBreakpointFailure_.error =
                CoordinateDecryptError::EngineSetupFailed;
            return false;
        }
        hardwareBreakpointFailure_.error =
            CoordinateDecryptError::PoolPointerStateInvalid;
        return true;
    }

    bool StopHardwareBreakpointRuntime() noexcept {
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (hardwareBreakpointRuntime_.Stop()) return true;
        }
        return false;
    }

    bool ReadHardwareBreakpointPosition(
        std::uintptr_t mesh,
        Vec3& position) {
        if (!IsValidPointer(mesh)) return false;
        std::uint32_t coordinateId = 0;
        if (!ReadValue(
                mesh + kCoordinateIdOffset, coordinateId) ||
            coordinateId == 0) {
            return false;
        }
        native::HardwareBreakpointCoordinate coordinate{};
        const bool found =
            hardwareBreakpointRequested_ &&
            hardwareBreakpointRuntime_.Lookup(
                coordinateId, mesh, world_, coordinate);
        if (IsCoordinateTraceEnabled() &&
            ShouldWriteCoordinateFrameTrace(coordinateTraceFrame_)) {
            std::fprintf(
                stderr,
                "[hwbp-coordinate] frame=%llu mesh=%llx id=%u "
                "found=%d base=%llx published=%zu xyz=(%.3f,%.3f,%.3f)\n",
                static_cast<unsigned long long>(coordinateTraceFrame_),
                static_cast<unsigned long long>(mesh),
                static_cast<unsigned int>(coordinateId),
                found ? 1 : 0,
                static_cast<unsigned long long>(
                    hardwareBreakpointRuntime_.RecordsBase()),
                hardwareBreakpointRuntime_.PublishedCoordinateCount(),
                coordinate.x,
                coordinate.y,
                coordinate.z);
            std::fflush(stderr);
        }
        if (!found) {
            return false;
        }
        const Vec3 candidate{
            coordinate.x,
            coordinate.y,
            coordinate.z,
        };
        if (!IsFinite(candidate) || !IsNonzero(candidate)) return false;
        position = candidate;
        return true;
    }
#endif

    bool ReadActorPosition(std::uintptr_t actor, Vec3& position, bool allowCache) {
        constexpr auto kCacheLifetime = std::chrono::milliseconds(300);
        const auto now = std::chrono::steady_clock::now();
        const std::uintptr_t component = ReadPointer(actor + 0x180);
        Vec3 candidate{};
        bool valid = IsValidPointer(component) &&
            ((ReadValue(component + 0x168, candidate) && IsFinite(candidate) && IsNonzero(candidate)) ||
             (ReadValue(component + 0x220, candidate) && IsFinite(candidate) && IsNonzero(candidate)));
        if (valid) {
            if (allowCache) {
                positionCache_[actor] =
                    PositionCacheEntry{candidate, now, world_};
            }
            position = candidate;
            return true;
        }
        if (allowCache) {
            const auto found = positionCache_.find(actor);
            if (found != positionCache_.end() &&
                found->second.world == world_ &&
                now - found->second.updatedAt <= kCacheLifetime) {
                position = found->second.position;
                return true;
            }
            if (found != positionCache_.end()) positionCache_.erase(found);
        }
        return false;
    }

    bool ReadCharacterPosition(
        std::uintptr_t actor,
        std::uintptr_t decodedRoot,
        std::string_view className,
        native::PositionReadMode mode,
        bool antiFlicker,
        Vec3& position,
        bool localActor = false,
        native::CharacterPositionSource* positionSource = nullptr) {
        platform::PerformanceTraceScope trace(
            mode == native::PositionReadMode::Direct
                ? platform::PerformancePhase::CoordinateDecode
                : platform::PerformancePhase::PositionRead);
        if (positionSource != nullptr) {
            *positionSource = native::CharacterPositionSource::None;
        }
#if 0
        if (hardwareBreakpointRequested_) {
            position = Vec3{};
            const std::uintptr_t mesh =
                ReadPointer(actor + native::kOrdinaryActorMeshOffset);
            const bool available =
                ReadHardwareBreakpointPosition(mesh, position);
            if (IsCoordinateTraceEnabled()) {
                auto& traceRecord = coordinateTraceRecords_[actor];
                traceRecord = CoordinateTraceRecord{};
                traceRecord.root = decodedRoot;
                traceRecord.component = mesh;
                traceRecord.raw = position;
                traceRecord.output = position;
                traceRecord.source = available
                    ? CoordinateTraceSource::HardwareBreakpoint
                    : CoordinateTraceSource::Failure;
                traceRecord.attempted = true;
                if (!available) {
                    traceRecord.error =
                        CoordinateDecryptError::PositionReadFailed;
                }
            }
            if (available && positionSource != nullptr) {
                *positionSource =
                    native::CharacterPositionSource::HardwareBreakpoint;
            }
            return available;
        }
#endif
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        if (IsCoordinateTableProbeEnabled() && algorithmPositionRequested_) {
            Vec3 tableCandidate{};
            const bool tableAvailable =
                ReadAlgorithmCoordinate(actor, 0, tableCandidate, true);
            std::fprintf(
                stderr,
                "[coordinate-table-probe] frame=%llu actor=%llx "
                "root=%llx available=%d xyz=(%.3f,%.3f,%.3f)\n",
                static_cast<unsigned long long>(coordinateTraceFrame_),
                static_cast<unsigned long long>(actor),
                static_cast<unsigned long long>(decodedRoot),
                tableAvailable ? 1 : 0,
                tableCandidate.x,
                tableCandidate.y,
                tableCandidate.z);
        }
        const bool algorithmCoordinateActive =
            native::ShouldReadAlgorithmCoordinate(
                algorithmPositionRequested_,
                algorithmDecryptRequested_);
        if (algorithmCoordinateActive) {
            Vec3 algorithmRaw{};
            const bool algorithmCoordinateAvailable =
                ReadAlgorithmCoordinate(
                    actor,
                    decodedRoot,
                    position,
                    false,
                    &algorithmRaw);
            if (IsCoordinateTraceEnabled()) {
                auto& trace = coordinateTraceRecords_[actor];
                trace = CoordinateTraceRecord{};
                trace.root = decodedRoot;
                trace.component = ReadPointer(actor + 0x180);
                trace.raw = algorithmRaw;
                trace.output = position;
                trace.source = algorithmCoordinateAvailable
                    ? CoordinateTraceSource::Algorithm
                    : CoordinateTraceSource::Failure;
                trace.attempted = true;
                if (!algorithmCoordinateAvailable) {
                    trace.error = CoordinateDecryptError::PositionReadFailed;
                }
            }
            const native::CharacterCoordinateDisposition disposition =
                native::ResolveCharacterCoordinateDisposition(
                    algorithmCoordinateActive,
                    algorithmCoordinateAvailable);
            if (disposition ==
                native::CharacterCoordinateDisposition::ReturnAlgorithm) {
                if (positionSource != nullptr) {
                    *positionSource = algorithmCoordinateFrameSource_ ==
                            native::AlgorithmCoordinateSource::RuntimeObject
                        ? native::CharacterPositionSource::AlgorithmObject
                        : native::CharacterPositionSource::AlgorithmTable;
                }
                return true;
            }
            if (disposition == native::CharacterCoordinateDisposition::
                    ReturnUnavailable) {
                return false;
            }
            ++algorithmCoordinateFallbackCount_;
            return false;
        }
#endif
        const bool coordinateDecryptRequested =
            mode == native::PositionReadMode::Direct &&
            UsesAnyCoordinatePoolRuntime();
        if (native::ShouldRequireAlgorithmPosition(
                localActor, coordinateDecryptRequested)) {
            if (!IsValidPointer(decodedRoot)) return false;
            auto now = std::chrono::steady_clock::now();
            const std::uintptr_t coordinateIdentity = decodedRoot;
            const native::DecodedPositionCacheIdentity currentCacheIdentity{
                world_,
                actor,
                coordinateIdentity,
            };
            const auto stalePending = decodedPositionPending_.find(actor);
            if (stalePending != decodedPositionPending_.end() &&
                native::ClassifyDecodedPositionCacheIdentity(
                    stalePending->second.identity,
                    currentCacheIdentity) !=
                    native::DecodedPositionCacheIdentityState::Match) {
                decodedPositionPending_.erase(stalePending);
            }
            CoordinateTraceRecord* trace = nullptr;
            if (IsCoordinateTraceEnabled()) {
                auto& record = coordinateTraceRecords_[actor];
                record = CoordinateTraceRecord{};
                record.root = decodedRoot;
                record.component = coordinateIdentity;
                record.attempted = true;
                trace = &record;
            }
            const auto storeDecoded = [&, trace](
                                           const Vec3& decoded,
                                           CoordinateTraceSource source) {
                const Vec3 adjusted = AdjustDecodedPosition(decoded);
                now = std::chrono::steady_clock::now();
                decodedPositionPending_.erase(actor);
                if (IsValidPointer(coordinateIdentity)) {
                    decodedPositionCache_[actor] =
                        DecodedPositionCacheEntry{
                            adjusted,
                            now,
                            now,
                            currentCacheIdentity,
                        };
                }
                position = adjusted;
                if (trace != nullptr) {
                    trace->raw = decoded;
                    trace->output = adjusted;
                    trace->source = source;
                }
                if (positionSource != nullptr) {
                    *positionSource = native::CharacterPositionSource::Decoded;
                }
            };
            const auto readStableHistory = [&]() {
                if (!IsValidPointer(coordinateIdentity)) return false;
                const auto found = decodedPositionCache_.find(actor);
                if (found == decodedPositionCache_.end()) return false;
                const auto identityState =
                    native::ClassifyDecodedPositionCacheIdentity(
                        found->second.identity,
                        currentCacheIdentity);
                if (!native::CanUseDecodedPositionHistory(
                        identityState,
                        found->second.updatedAt,
                        now)) {
                    decodedPositionCache_.erase(found);
                    return false;
                }
                const Vec3 history = found->second.position;
                now = std::chrono::steady_clock::now();
                found->second.observedAt = now;
                position = history;
                if (trace != nullptr) {
                    trace->history = history;
                    trace->output = history;
                    trace->source = CoordinateTraceSource::StabilityHistory;
                }
                if (positionSource != nullptr) {
                    *positionSource = native::CharacterPositionSource::Decoded;
                }
                return true;
            };
            const auto readCached = [&]() {
                if (!antiFlicker) return false;
                const auto found = decodedPositionCache_.find(actor);
                if (found == decodedPositionCache_.end()) return false;
                const native::DecodedPositionCacheIdentityState identityState =
                    native::ClassifyDecodedPositionCacheIdentity(
                        found->second.identity,
                        currentCacheIdentity);
                if (native::IsDecodedPositionCacheOwnerMatch(
                        found->second.identity,
                        currentCacheIdentity) &&
                    native::ShouldEscalateDecodedPositionFailure(
                        found->second.updatedAt,
                        now)) {
                    algorithmFrameAgedDecodedFailure_ = true;
                }
                if (identityState ==
                    native::DecodedPositionCacheIdentityState::Unknown) {
                    return false;
                }
                if (native::ShouldDiscardDecodedPositionCache(
                        identityState)) {
                    decodedPositionCache_.erase(found);
                    return false;
                }
                if (!native::CanRetainDecodedPosition(
                        antiFlicker,
                        found->second.identity,
                        currentCacheIdentity,
                        found->second.updatedAt,
                        now)) {
                    decodedPositionCache_.erase(found);
                    return false;
                }
                position = found->second.position;
                if (trace != nullptr) {
                    trace->output = position;
                    trace->source = CoordinateTraceSource::Cache;
                }
                if (positionSource != nullptr) {
                    *positionSource = native::CharacterPositionSource::Decoded;
                }
                return true;
            };
            if (!algorithmReplayAllowedThisFrame_ &&
                !UsesAnyCoordinatePoolRuntime()) {
                return readStableHistory() || readCached();
            }
            if (UsesAnyCoordinatePoolRuntime()) {
                ++algorithmAttemptCount_;
                ++algorithmFrameAttemptCount_;
                native::CoordinatePoolCandidateSet candidates{};
                const bool canReadPosition = coordinatePoolReady_ &&
                    memory_ != nullptr && IsValidPointer(coordinateIdentity);
                const bool indexedDecrypt =
                    UsesCoordinateDecrypt2Runtime();
                const bool candidatesReady = canReadPosition &&
                    (indexedDecrypt
                         ? coordinateDecrypt2Runtime_.ReadCandidates(
                               coordinateIdentity,
                               coordinateDecrypt2Index_,
                               candidates)
                         : coordinatePoolRuntime_.ReadCandidates(
                               coordinateIdentity,
                               candidates));
                if (candidatesReady) {
                    ++algorithmSuccessCount_;

                    std::uint32_t validMask = 0;
                    Vec3 observedRaw{};
                    bool observedValid = false;
                    if (indexedDecrypt) {
                        const auto& observed = candidates.resolvedPosition;
                        observedRaw = {
                            observed.x,
                            observed.y,
                            observed.z,
                        };
                        observedValid = candidates.resolvedValid &&
                            IsFinite(observedRaw) && IsNonzero(observedRaw);
                    } else {
                        for (std::size_t slot = 0;
                             slot < candidates.positions.size();
                             ++slot) {
                            const auto& candidate =
                                candidates.positions[slot];
                            const Vec3 decoded{
                                candidate.x,
                                candidate.y,
                                candidate.z,
                            };
                            if (candidates.valid[slot] &&
                                IsFinite(decoded) &&
                                IsNonzero(decoded)) {
                                validMask |= 1U << slot;
                            }
                        }
                        const std::size_t observedSlot =
                            candidates.selectedLogicalSlot;
                        if (observedSlot < candidates.positions.size()) {
                            const auto& observed =
                                candidates.positions[observedSlot];
                            observedRaw = {
                                observed.x,
                                observed.y,
                                observed.z,
                            };
                            observedValid =
                                candidates.valid[observedSlot] &&
                                IsFinite(observedRaw) &&
                                IsNonzero(observedRaw);
                        }
                    }
                    const auto observeIndexedOutputStability =
                        [&](bool flicker) {
                            if (!indexedDecrypt) return;
                            static_cast<void>(coordinateDecrypt2Runtime_
                                .ObserveOutputStability(
                                coordinateIdentity,
                                candidates.decryptIndexOffset,
                                candidates.poolBlockCount,
                                flicker));
                        };

                    if (trace != nullptr) {
                        trace->raw = observedRaw;
                        trace->guestPc =
                            ActiveCoordinatePoolProbe().guestEntry;
                        trace->source = observedValid
                            ? CoordinateTraceSource::Pool
                            : CoordinateTraceSource::Pending;
                    }
                    if (IsCoordinateTraceEnabled()) {
                        if (indexedDecrypt) {
                            std::fprintf(
                                stderr,
                                "[coordinate-decrypt2-selected] "
                                "frame=%llu world=%llx actor=%llx "
                                "component=%llx ring=%llx index=%llx "
                                "decoded=%u offset=%u blocks=%u slot=%u "
                                "accepted=%d xyz=(%.3f,%.3f,%.3f)\n",
                                static_cast<unsigned long long>(
                                    coordinateTraceFrame_),
                                static_cast<unsigned long long>(world_),
                                static_cast<unsigned long long>(actor),
                                static_cast<unsigned long long>(
                                    coordinateIdentity),
                                static_cast<unsigned long long>(
                                    candidates.ring),
                                static_cast<unsigned long long>(
                                    candidates.index),
                                static_cast<unsigned int>(
                                    candidates.decodedPhysicalSlot),
                                static_cast<unsigned int>(
                                    candidates.decryptIndexOffset),
                                static_cast<unsigned int>(
                                    candidates.poolBlockCount),
                                static_cast<unsigned int>(
                                    candidates.resolvedPoolSlot),
                                observedValid ? 1 : 0,
                                observedRaw.x,
                                observedRaw.y,
                                observedRaw.z);
                        } else {
                            std::fprintf(
                                stderr,
                                "[coordinate-pool-selected] frame=%llu "
                                "world=%llx actor=%llx component=%llx "
                                "ring=%llx index=%llx decoded=%u "
                                "physical=%u bank=%u selected=%u "
                                "layout=%u/%u phase=%u valid_mask=%02x "
                                "accepted=%d xyz=(%.3f,%.3f,%.3f)\n",
                                static_cast<unsigned long long>(
                                    coordinateTraceFrame_),
                                static_cast<unsigned long long>(world_),
                                static_cast<unsigned long long>(actor),
                                static_cast<unsigned long long>(
                                    coordinateIdentity),
                                static_cast<unsigned long long>(
                                    candidates.ring),
                                static_cast<unsigned long long>(
                                    candidates.index),
                                static_cast<unsigned int>(
                                    candidates.decodedPhysicalSlot),
                                static_cast<unsigned int>(
                                    candidates.selectedPhysicalSlot),
                                static_cast<unsigned int>(
                                    candidates.activeBank),
                                static_cast<unsigned int>(
                                    candidates.selectedLogicalSlot),
                                static_cast<unsigned int>(
                                    candidates.logicalSlotCount),
                                static_cast<unsigned int>(
                                    candidates.physicalSlotCount),
                                static_cast<unsigned int>(
                                    candidates.slotPhase),
                                static_cast<unsigned int>(validMask),
                                observedValid ? 1 : 0,
                                observedRaw.x,
                                observedRaw.y,
                                observedRaw.z);
                        }
                    }
                    if (!observedValid) {
                        observeIndexedOutputStability(true);
                        const bool historyRecovered = readStableHistory();
                        const bool cacheRecovered =
                            !historyRecovered && readCached();
                        if (!native::ShouldReportCoordinateOutputError(
                                historyRecovered, cacheRecovered)) {
                            ++algorithmFrameSuccessCount_;
                            return true;
                        }
                        algorithmFrameOutputError_ =
                            CoordinateDecryptError::OutputZero;
                        if (trace != nullptr) {
                            trace->source = CoordinateTraceSource::Failure;
                            trace->error = CoordinateDecryptError::OutputZero;
                            trace->systemError = 0;
                        }
                        decodedPositionCache_.erase(actor);
                        return false;
                    }

                    now = std::chrono::steady_clock::now();
                    const Vec3 observedAdjusted =
                        AdjustDecodedPosition(observedRaw);
                    auto history = decodedPositionCache_.find(actor);
                    if (history != decodedPositionCache_.end()) {
                        const native::DecodedPositionCacheIdentityState
                            identityState =
                                native::ClassifyDecodedPositionCacheIdentity(
                                    history->second.identity,
                                    currentCacheIdentity);
                        if (!native::CanUseDecodedPositionHistory(
                                identityState,
                                history->second.updatedAt,
                                now)) {
                            decodedPositionCache_.erase(history);
                            decodedPositionPending_.erase(actor);
                            history = decodedPositionCache_.end();
                        }
                    }
                    if (history != decodedPositionCache_.end()) {
                        const native::AlgorithmPosition historyPosition{
                            history->second.position.x,
                            history->second.position.y,
                            history->second.position.z,
                        };
                        const native::AlgorithmPosition observedPosition{
                            observedAdjusted.x,
                            observedAdjusted.y,
                            observedAdjusted.z,
                        };
                        DecodedPositionPendingEntry& pending =
                            decodedPositionPending_[actor];
                        if (native::ClassifyDecodedPositionCacheIdentity(
                                pending.identity,
                                currentCacheIdentity) !=
                            native::DecodedPositionCacheIdentityState::Match) {
                            pending = {};
                            pending.identity = currentCacheIdentity;
                        }
                        const bool hadPending = pending.sample.count != 0;
                        const native::AlgorithmPositionOutputObservation
                            observation =
                                native::ObserveAlgorithmPositionOutput(
                                    historyPosition,
                                    observedPosition,
                                    pending.sample,
                                    coordinateTraceFrame_,
                                    now);
                        observeIndexedOutputStability(
                            observation.decision ==
                            native::AlgorithmPositionOutputDecision::
                                RetainHistory);
                        if (trace != nullptr) {
                            trace->history = history->second.position;
                            trace->stabilityDelta = std::sqrt(
                                std::max(observation.distanceSquared, 0.0));
                            trace->pendingCount = observation.pendingCount;
                            trace->secondAttempted = hadPending;
                            switch (observation.decision) {
                                case native::AlgorithmPositionOutputDecision::
                                        AcceptImmediate:
                                    trace->stabilityDecision =
                                        CoordinateStabilityDecision::
                                            FirstNearHistory;
                                    break;
                                case native::AlgorithmPositionOutputDecision::
                                        RetainHistory:
                                    trace->stabilityDecision =
                                        CoordinateStabilityDecision::
                                            SecondPending;
                                    break;
                                case native::AlgorithmPositionOutputDecision::
                                        AcceptConfirmed:
                                    trace->stabilityDecision =
                                        CoordinateStabilityDecision::
                                            SecondConfirmed;
                                    break;
                            }
                        }
                        if (observation.decision ==
                            native::AlgorithmPositionOutputDecision::
                                RetainHistory) {
                            const bool retained = readStableHistory();
                            if (retained) ++algorithmFrameSuccessCount_;
                            return retained;
                        }
                    } else {
                        observeIndexedOutputStability(false);
                        if (trace != nullptr) {
                            trace->stabilityDecision =
                                CoordinateStabilityDecision::FirstNoHistory;
                        }
                    }
                    storeDecoded(observedRaw, CoordinateTraceSource::Pool);
                    ++algorithmFrameSuccessCount_;
                    return true;
                } else {
                    const native::CoordinatePoolRuntimeProbe failedProbe =
                        ActiveCoordinatePoolProbe();
                    const CoordinateDecryptError failureError =
                        CoordinatePoolError(
                            failedProbe.error, failedProbe.read);
                    if (readStableHistory() || readCached()) {
                        ++algorithmFrameSuccessCount_;
                        if (trace != nullptr) {
                            trace->guestPc = failedProbe.guestEntry;
                        }
                        if (IsCoordinateTraceEnabled()) {
                            std::fprintf(
                                stderr,
                                "[coordinate-pool-retain] frame=%llu "
                                "actor=%llx component=%llx error=%u "
                                "sys=%d read_stage=%u read_failure=%u "
                                "read_path=%u read_at=%llx read_n=%zu\n",
                                static_cast<unsigned long long>(
                                    coordinateTraceFrame_),
                                static_cast<unsigned long long>(actor),
                                static_cast<unsigned long long>(
                                    coordinateIdentity),
                                static_cast<unsigned int>(failureError),
                                failedProbe.systemError,
                                static_cast<unsigned int>(
                                    failedProbe.read.stage),
                                static_cast<unsigned int>(
                                    failedProbe.read.failure),
                                static_cast<unsigned int>(
                                    failedProbe.read.lastPath),
                                static_cast<unsigned long long>(
                                    failedProbe.read.address),
                                failedProbe.read.size);
                        }
                        return true;
                    }
                    if (trace != nullptr) {
                        trace->source = CoordinateTraceSource::Failure;
                        trace->error = failureError;
                        trace->systemError = failedProbe.systemError;
                    }
                    RecordCoordinateFrameFailure(CoordinateFailure{
                        failureError != CoordinateDecryptError::None
                            ? failureError
                            : CoordinateDecryptError::PositionReadFailed,
                        failedProbe.systemError,
                        failedProbe.read,
                    });
                }
                decodedPositionCache_.erase(actor);
                return false;
            }
            if (!localActor && coordinateDecryptRequested &&
                algorithmEntryReady_ && algorithmExecutionContextReady_) {
                native::AlgorithmPosition candidate{};
                const bool refreshCachedPages =
                    algorithmReplayPagePolicy_.ConsumeRefresh(
                        native::AlgorithmReplayPageKey{
                            algorithmGuestPc_,
                            algorithmExecutionContext_.tpidrEl0,
                            algorithmExecutionContext_.threadId,
                            algorithmExecutionContext_.threadStartTimeTicks,
                            algorithmExecutionContext_.generation,
                        });
                ++algorithmAttemptCount_;
                const native::AlgorithmPositionRuntimeResult replayResult =
                    memory_ != nullptr
                    ? algorithmPositionRuntime_.ExecuteAtGuestPcResult(
                          *memory_,
                          algorithmGuestPc_,
                          coordinateIdentity,
                          algorithmExecutionContext_,
                          candidate,
                          refreshCachedPages)
                    : native::AlgorithmPositionRuntimeResult::Failed;
                const native::AlgorithmPositionRuntimeProbe replayProbe =
                    algorithmPositionRuntime_.Probe();
                if (IsCoordinateTraceEnabled()) {
                    const char* replayState = replayResult ==
                            native::AlgorithmPositionRuntimeResult::Pending
                        ? "pending"
                        : (replayResult ==
                                   native::AlgorithmPositionRuntimeResult::Ready
                               ? "ready"
                               : "failed");
                    const CoordinateDecryptError replayError =
                        AlgorithmPositionError(replayProbe.error);
                    std::fprintf(
                        stderr,
                        "[coordinate-replay-probe] frame=%llu actor=%llx "
                        "component=%llx state=%s guest_pc=%llx request=%llu "
                        "completed=%llu attempts=%llu successes=%llu "
                        "generation=%llu stage=%u "
                        "runtime_error=%u cd=%u sys=%d fault=%llx "
                        "final_pc=%llx expected_pc=%llx fault_type=%d "
                        "fault_size=%d fault_value=%lld sp=%llx x8=%llx "
                        "x9=%llx x10=%llx x23=%llx x26=%llx x27=%llx "
                        "pac_at=%llx pac_data=%llx pac_mod=%llx "
                        "pac_result=%llx pac_count=%llu pac_source=%u "
                        "tpidr=%llx ctr=%llx cntfrq=%llx "
                        "counter_first=%llx counter_last=%llx "
                        "mrs_ctr=%llu mrs_tpidr=%llu mrs_cntfrq=%llu "
                        "mrs_counter=%llu "
                        "read_stage=%u read_failure=%u read_path=%u "
                        "read_at=%llx read_n=%zu\n",
                        static_cast<unsigned long long>(coordinateTraceFrame_),
                        static_cast<unsigned long long>(actor),
                        static_cast<unsigned long long>(coordinateIdentity),
                        replayState,
                        static_cast<unsigned long long>(algorithmGuestPc_),
                        static_cast<unsigned long long>(replayProbe.requestId),
                        static_cast<unsigned long long>(
                            replayProbe.completedRequestId),
                        static_cast<unsigned long long>(replayProbe.attempts),
                        static_cast<unsigned long long>(replayProbe.successes),
                        static_cast<unsigned long long>(replayProbe.generation),
                        static_cast<unsigned int>(replayProbe.stage),
                        static_cast<unsigned int>(replayProbe.error),
                        static_cast<unsigned int>(replayError),
                        replayProbe.unicornError != 0
                            ? replayProbe.unicornError
                            : replayProbe.read.systemError,
                        static_cast<unsigned long long>(replayProbe.faultAddress),
                        static_cast<unsigned long long>(replayProbe.finalPc),
                        static_cast<unsigned long long>(replayProbe.expectedPc),
                        replayProbe.faultType,
                        replayProbe.faultSize,
                        static_cast<long long>(replayProbe.faultValue),
                        static_cast<unsigned long long>(replayProbe.stackPointer),
                        static_cast<unsigned long long>(replayProbe.x8),
                        static_cast<unsigned long long>(replayProbe.x9),
                        static_cast<unsigned long long>(replayProbe.x10),
                        static_cast<unsigned long long>(replayProbe.x23),
                        static_cast<unsigned long long>(replayProbe.x26),
                        static_cast<unsigned long long>(replayProbe.x27),
                        static_cast<unsigned long long>(replayProbe.pacgaAddress),
                        static_cast<unsigned long long>(replayProbe.pacgaData),
                        static_cast<unsigned long long>(replayProbe.pacgaModifier),
                        static_cast<unsigned long long>(replayProbe.pacgaResult),
                        static_cast<unsigned long long>(replayProbe.pacgaCount),
                        static_cast<unsigned int>(replayProbe.pacgaSource),
                        static_cast<unsigned long long>(replayProbe.tpidrEl0),
                        static_cast<unsigned long long>(replayProbe.ctrEl0),
                        static_cast<unsigned long long>(replayProbe.cntfrqEl0),
                        static_cast<unsigned long long>(
                            replayProbe.counterFirst),
                        static_cast<unsigned long long>(
                            replayProbe.counterLast),
                        static_cast<unsigned long long>(
                            replayProbe.ctrReadCount),
                        static_cast<unsigned long long>(
                            replayProbe.tpidrReadCount),
                        static_cast<unsigned long long>(
                            replayProbe.cntfrqReadCount),
                        static_cast<unsigned long long>(
                            replayProbe.counterReadCount),
                        static_cast<unsigned int>(replayProbe.read.stage),
                        static_cast<unsigned int>(replayProbe.read.failure),
                        static_cast<unsigned int>(replayProbe.read.lastPath),
                        static_cast<unsigned long long>(replayProbe.read.address),
                        replayProbe.read.size);
                    std::fflush(stderr);
                    if (replayResult ==
                            native::AlgorithmPositionRuntimeResult::Failed &&
                        replayProbe.instructionTraceCount != 0 &&
                        (replayProbe.finalPc !=
                             algorithmLastInstructionTracePc_ ||
                         replayProbe.faultAddress !=
                             algorithmLastInstructionTraceFault_)) {
                        algorithmLastInstructionTracePc_ =
                            replayProbe.finalPc;
                        algorithmLastInstructionTraceFault_ =
                            replayProbe.faultAddress;
                        std::fprintf(
                            stderr,
                            "[coordinate-replay-path] request=%llu "
                            "count=%zu pcs=",
                            static_cast<unsigned long long>(
                                replayProbe.completedRequestId),
                            replayProbe.instructionTraceCount);
                        for (std::size_t index = 0;
                             index < replayProbe.instructionTraceCount;
                             ++index) {
                            std::fprintf(
                                stderr,
                                "%s%llx",
                                index == 0 ? "" : ",",
                                static_cast<unsigned long long>(
                                    replayProbe.instructionTrace[index]));
                        }
                        std::fputc('\n', stderr);
                        std::fflush(stderr);
                    }
                }
                if (replayResult ==
                    native::AlgorithmPositionRuntimeResult::Pending) {
                    if (trace != nullptr) {
                        trace->source = CoordinateTraceSource::Pending;
                        trace->guestPc = algorithmGuestPc_;
                        trace->error = CoordinateDecryptError::None;
                        trace->systemError = 0;
                    }
                    return readStableHistory() || readCached();
                }
                ++algorithmFrameAttemptCount_;
                if (replayResult ==
                    native::AlgorithmPositionRuntimeResult::Ready) {
                    const Vec3 decoded{
                        candidate.x,
                        candidate.y,
                        candidate.z,
                    };
                    if (IsFinite(decoded) && IsNonzero(decoded)) {
                        ++algorithmSuccessCount_;
                        ++algorithmFrameSuccessCount_;
                        decodedPositionPending_.erase(actor);
                        storeDecoded(decoded, CoordinateTraceSource::Replay);
                        if (trace != nullptr) {
                            trace->guestPc = algorithmGuestPc_;
                            trace->error = CoordinateDecryptError::None;
                            trace->systemError = 0;
                        }
                        return true;
                    }
                    if (trace != nullptr) {
                        trace->raw = decoded;
                        trace->source = CoordinateTraceSource::Failure;
                    }
                    const bool historyRecovered = readStableHistory();
                    const bool cacheRecovered =
                        !historyRecovered && readCached();
                    if (!native::ShouldReportCoordinateOutputError(
                            historyRecovered, cacheRecovered)) {
                        return true;
                    }
                    const CoordinateDecryptError outputError = IsFinite(decoded)
                        ? CoordinateDecryptError::OutputZero
                        : CoordinateDecryptError::OutputNotFinite;
                    algorithmFrameOutputError_ = outputError;
                    if (trace != nullptr) {
                        trace->source = CoordinateTraceSource::Failure;
                        trace->error = outputError;
                        trace->systemError = 0;
                    }
                    return false;
                } else {
                    if (trace != nullptr) {
                        trace->source = CoordinateTraceSource::Failure;
                        trace->guestPc = algorithmGuestPc_;
                        trace->error = AlgorithmPositionError(
                            replayProbe.error);
                        trace->systemError = replayProbe.unicornError != 0
                            ? replayProbe.unicornError
                            : replayProbe.read.systemError;
                    }
                    RecordCoordinateFrameFailure(CoordinateFailure{
                        AlgorithmPositionError(replayProbe.error) ==
                                CoordinateDecryptError::None
                            ? CoordinateDecryptError::ReplayExecutionFailed
                            : AlgorithmPositionError(replayProbe.error),
                        replayProbe.unicornError != 0
                            ? replayProbe.unicornError
                            : replayProbe.read.systemError,
                        replayProbe.read,
                    });
                }
            }
            return readStableHistory() || readCached();
        }

        auto readBytes = [this](std::uintptr_t address,
                                void* destination,
                                std::size_t size) {
            return memory_ != nullptr && IsValidReadAddress(address) &&
                size <= kMaximumRemoteAddress - address &&
                memory_->Read(address, destination, size);
        };
        native::CharacterPositionResolver::Coordinate coordinate{};
        const bool resolved = localActor
            ? characterPositions_.ReadLocalWithRoot(
                actor,
                decodedRoot,
                className,
                mode,
                antiFlicker,
                coordinate,
                readBytes)
            : characterPositions_.ReadWithRoot(
                actor,
                decodedRoot,
                className,
                mode,
                antiFlicker,
                coordinate,
                readBytes);
        if (!resolved) {
            return false;
        }
        position = Vec3{coordinate[0], coordinate[1], coordinate[2]};
        if (!IsFinite(position) || !IsNonzero(position)) return false;
        if (IsCoordinateTraceEnabled()) {
            auto& trace = coordinateTraceRecords_[actor];
            trace = CoordinateTraceRecord{};
            trace.root = decodedRoot;
            trace.output = position;
            trace.source = CoordinateTraceSource::Standard;
            trace.attempted = true;
        }
        if (positionSource != nullptr) {
            *positionSource = native::CharacterPositionSource::Standard;
        }
        return true;
    }

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    void RefreshAlgorithmCoordinateSources() {
        algorithmCoordinateSnapshot_.clear();
        algorithmCoordinateTableReady_ = false;
        algorithmCoordinateTableDiagnostic_ = {};
        algorithmCoordinateRuntimeReady_ = false;
        algorithmCoordinateRuntimeDiagnostic_ = {};
        if (!native::ShouldReadAlgorithmCoordinate(
                algorithmPositionRequested_,
                algorithmDecryptRequested_) ||
            memory_ == nullptr || !IsValidPointer(moduleBase_)) {
            runtimeCoordinateCodec_.Reset();
            return;
        }

        ++algorithmCoordinateRefreshCount_;
        auto readBytes = [this](std::uintptr_t address,
                                void* destination,
                                std::size_t size) {
            return memory_ != nullptr && IsValidReadAddress(address) &&
                size != 0 && size <= kMaximumRemoteAddress - address &&
                memory_->Read(address, destination, size);
        };
        ++algorithmCoordinateResolveAttemptCount_;
        algorithmCoordinateRuntimeReady_ = runtimeCoordinateCodec_.Refresh(
            moduleBase_,
            readBytes,
            [this](std::uintptr_t address) {
                return IsMappedExecutableAddress(processId_, address);
            },
            [this](std::uintptr_t address) {
                return IsMappedNamedExecutableAddress(
                    processId_, address, "[anon:objects_external_alloc]");
            });
        algorithmCoordinateRuntimeDiagnostic_ =
            runtimeCoordinateCodec_.Diagnostic();
        if (algorithmCoordinateRuntimeReady_) {
            ++algorithmCoordinateResolveSuccessCount_;
        } else {
            algorithmCoordinateFrameRuntimeFailure_ =
                algorithmCoordinateRuntimeDiagnostic_;
        }
        algorithmCoordinateTableReady_ = algorithmCoordinateReader_.ReadTable(
            moduleBase_,
            algorithmCoordinateSnapshot_,
            algorithmCoordinateTableDiagnostic_,
            readBytes);
    }

    void RecordAlgorithmCoordinateFailure(
        const native::AlgorithmCoordinateDiagnostic& diagnostic) {
        if (algorithmCoordinateFrameFailure_.error ==
            native::AlgorithmCoordinateReadError::None) {
            algorithmCoordinateFrameFailure_ = diagnostic;
        }
    }

    bool ReadAlgorithmTableCoordinate(std::uintptr_t actor,
                                      Vec3& position) {
        ++algorithmCoordinateTableAttemptCount_;
        if (!algorithmCoordinateTableReady_) {
            RecordAlgorithmCoordinateFailure(
                algorithmCoordinateTableDiagnostic_);
            return false;
        }

        for (std::uint32_t index = 0;
             index < static_cast<std::uint32_t>(
                 algorithmCoordinateSnapshot_.size());
             ++index) {
            const native::AlgorithmCoordinateRecord& record =
                algorithmCoordinateSnapshot_[index];
            if (record.actor != actor) continue;

            native::AlgorithmCoordinateDiagnostic diagnostic =
                algorithmCoordinateTableDiagnostic_;
            diagnostic.actor = actor;
            diagnostic.recordIndex = index;
            diagnostic.x = record.coordinate.x;
            diagnostic.y = record.coordinate.y;
            diagnostic.z = record.coordinate.z;
            if (!record.valid) {
                diagnostic.error =
                    native::AlgorithmCoordinateReadError::CoordinateInvalid;
                RecordAlgorithmCoordinateFailure(diagnostic);
                return false;
            }

            position = Vec3{
                record.coordinate.x,
                record.coordinate.y,
                record.coordinate.z,
            };
            if (!IsFinite(position) || !IsNonzero(position)) {
                diagnostic.error =
                    native::AlgorithmCoordinateReadError::CoordinateInvalid;
                RecordAlgorithmCoordinateFailure(diagnostic);
                return false;
            }
            diagnostic.error = native::AlgorithmCoordinateReadError::None;
            return true;
        }

        native::AlgorithmCoordinateDiagnostic diagnostic =
            algorithmCoordinateTableDiagnostic_;
        diagnostic.error =
            native::AlgorithmCoordinateReadError::ActorNotFound;
        diagnostic.actor = actor;
        RecordAlgorithmCoordinateFailure(diagnostic);
        return false;
    }

    bool ReadAlgorithmObjectCoordinate(std::uintptr_t actor,
                                       std::uintptr_t component,
                                       Vec3& rawPosition,
                                       Vec3& position) {
        rawPosition = Vec3{};
        position = Vec3{};
        if (!algorithmCoordinateRuntimeReady_) {
            if (algorithmCoordinateFrameRuntimeFailure_.error ==
                native::RuntimeCoordinateCodecError::None) {
                algorithmCoordinateFrameRuntimeFailure_ =
                    algorithmCoordinateRuntimeDiagnostic_;
            }
            return false;
        }

        const std::uintptr_t ordinaryRoot = ReadPointer(actor + 0x180);
        if (!IsValidPointer(ordinaryRoot) ||
            (IsValidPointer(component) && component != ordinaryRoot)) {
            native::RuntimeCoordinateCodecDiagnostic diagnostic =
                algorithmCoordinateRuntimeDiagnostic_;
            diagnostic.stage =
                native::RuntimeCoordinateCodecStage::Failed;
            diagnostic.error =
                native::RuntimeCoordinateCodecError::ObjectInvalid;
            diagnostic.object = component;
            algorithmCoordinateFrameRuntimeFailure_ = diagnostic;
            return false;
        }
        component = ordinaryRoot;

        const auto cached = algorithmCoordinateObjectCache_.find(component);
        if (cached != algorithmCoordinateObjectCache_.end()) {
            rawPosition = cached->second.raw;
            if (cached->second.owner != actor) {
                native::RuntimeCoordinateCodecDiagnostic diagnostic =
                    cached->second.diagnostic;
                diagnostic.stage =
                    native::RuntimeCoordinateCodecStage::Failed;
                diagnostic.error =
                    native::RuntimeCoordinateCodecError::OwnerMismatch;
                diagnostic.owner = actor;
                algorithmCoordinateFrameRuntimeFailure_ = diagnostic;
                return false;
            }
            if (!cached->second.valid) {
                algorithmCoordinateFrameRuntimeFailure_ =
                    cached->second.diagnostic;
                return false;
            }
            position = cached->second.position;
            algorithmCoordinateFrameRuntimeSuccess_ =
                cached->second.diagnostic;
            algorithmCoordinateFrameSource_ =
                native::AlgorithmCoordinateSource::RuntimeObject;
            return true;
        }

        ++algorithmCoordinateObjectAttemptCount_;
        AlgorithmCoordinateObjectCacheEntry entry{};
        entry.owner = actor;
        entry.diagnostic = algorithmCoordinateRuntimeDiagnostic_;
        if (component > kMaximumRemoteAddress - 0x5A0) {
            entry.diagnostic.stage =
                native::RuntimeCoordinateCodecStage::Failed;
            entry.diagnostic.error =
                native::RuntimeCoordinateCodecError::ObjectInvalid;
            entry.diagnostic.object = component;
            algorithmCoordinateObjectCache_.emplace(component, entry);
            algorithmCoordinateFrameRuntimeFailure_ = entry.diagnostic;
            return false;
        }

        const std::uintptr_t adjustmentAddress = component + 0x5A0;
        constexpr unsigned kMaximumPresentationAttempts = 3;
        for (unsigned attempt = 0;
             attempt < kMaximumPresentationAttempts;
             ++attempt) {
            float firstAdjustment = 0.0f;
            if (!ReadValue(adjustmentAddress, firstAdjustment)) {
                entry.diagnostic.stage =
                    native::RuntimeCoordinateCodecStage::Failed;
                entry.diagnostic.error = native::RuntimeCoordinateCodecError::
                    VerticalAdjustmentReadFailed;
                entry.diagnostic.object = component;
                continue;
            }

            native::RuntimeCoordinateCodec::Coordinate candidate{};
            native::RuntimeCoordinateCodecDiagnostic diagnostic{};
            const bool decoded = runtimeCoordinateCodec_.Decode(
                component, actor, candidate, diagnostic,
                [this](std::uintptr_t address,
                       void* destination,
                       std::size_t size) {
                    return memory_ != nullptr &&
                        IsValidReadAddress(address) && size != 0 &&
                        size <= kMaximumRemoteAddress - address &&
                        memory_->Read(address, destination, size);
                });
            entry.raw = Vec3{candidate.x, candidate.y, candidate.z};
            diagnostic.verticalAdjustmentFirst = firstAdjustment;
            if (!decoded || !IsFinite(entry.raw) ||
                !IsNonzero(entry.raw)) {
                entry.diagnostic = diagnostic;
                break;
            }

            float secondAdjustment = 0.0f;
            if (!ReadValue(adjustmentAddress, secondAdjustment)) {
                diagnostic.stage =
                    native::RuntimeCoordinateCodecStage::Failed;
                diagnostic.error = native::RuntimeCoordinateCodecError::
                    VerticalAdjustmentReadFailed;
                entry.diagnostic = diagnostic;
                continue;
            }
            diagnostic.verticalAdjustmentSecond = secondAdjustment;
            if (ReadPointer(actor + 0x180) != component) {
                diagnostic.stage =
                    native::RuntimeCoordinateCodecStage::Failed;
                diagnostic.error =
                    native::RuntimeCoordinateCodecError::ObjectUnstable;
                entry.diagnostic = diagnostic;
                continue;
            }

            const native::AlgorithmCoordinateFinalizeResult finalized =
                native::FinalizeAlgorithmCharacterCoordinate(
                    entry.raw.x,
                    entry.raw.y,
                    entry.raw.z,
                    true,
                    firstAdjustment,
                    true,
                    secondAdjustment);
            if (!finalized.Accepted()) {
                diagnostic.stage =
                    native::RuntimeCoordinateCodecStage::Failed;
                switch (finalized.error) {
                    case native::AlgorithmCoordinateFinalizeError::
                            VerticalAdjustmentReadFailed:
                        diagnostic.error =
                            native::RuntimeCoordinateCodecError::
                                VerticalAdjustmentReadFailed;
                        break;
                    case native::AlgorithmCoordinateFinalizeError::
                            VerticalAdjustmentInvalid:
                        diagnostic.error =
                            native::RuntimeCoordinateCodecError::
                                VerticalAdjustmentInvalid;
                        break;
                    case native::AlgorithmCoordinateFinalizeError::
                            VerticalAdjustmentUnstable:
                        diagnostic.error =
                            native::RuntimeCoordinateCodecError::
                                VerticalAdjustmentUnstable;
                        break;
                    case native::AlgorithmCoordinateFinalizeError::None:
                    case native::AlgorithmCoordinateFinalizeError::
                            RawInvalid:
                    case native::AlgorithmCoordinateFinalizeError::
                            OutputInvalid:
                        diagnostic.error =
                            native::RuntimeCoordinateCodecError::OutputInvalid;
                        break;
                }
                entry.diagnostic = diagnostic;
                continue;
            }

            entry.position = Vec3{finalized.x, finalized.y, finalized.z};
            diagnostic.presentedZ = finalized.z;
            entry.valid = true;
            entry.diagnostic = diagnostic;
            break;
        }

        algorithmCoordinateObjectCache_.emplace(component, entry);
        rawPosition = entry.raw;
        if (!entry.valid) {
            algorithmCoordinateFrameRuntimeFailure_ = entry.diagnostic;
            return false;
        }

        position = entry.position;
        algorithmCoordinateFrameRuntimeSuccess_ = entry.diagnostic;
        algorithmCoordinateFrameSource_ =
            native::AlgorithmCoordinateSource::RuntimeObject;
        ++algorithmCoordinateObjectSuccessCount_;
        ++algorithmCoordinateSuccessCount_;
        ++algorithmCoordinateFrameSuccessCount_;
        return true;
    }

    bool ReadAlgorithmCoordinate(std::uintptr_t actor,
                                 std::uintptr_t component,
                                 Vec3& position,
                                 bool diagnosticOverride = false,
                                 Vec3* rawPosition = nullptr) {
        position = Vec3{};
        if (rawPosition != nullptr) *rawPosition = Vec3{};
        if (!native::kAlgorithmCoordinateEnabled) return false;
        if (diagnosticOverride) {
            if (memory_ == nullptr || !IsValidPointer(moduleBase_) ||
                !IsValidPointer(actor)) {
                return false;
            }
            auto readBytes = [this](std::uintptr_t address,
                                    void* destination,
                                    std::size_t size) {
                return memory_ != nullptr && IsValidReadAddress(address) &&
                    size != 0 && size <= kMaximumRemoteAddress - address &&
                    memory_->Read(address, destination, size);
            };
            native::AlgorithmCoordinate candidate{};
            native::AlgorithmCoordinateDiagnostic diagnostic{};
            if (!algorithmCoordinateReader_.Read(
                    moduleBase_,
                    actor,
                    candidate,
                    diagnostic,
                    readBytes)) {
                return false;
            }
            position = Vec3{candidate.x, candidate.y, candidate.z};
            if (rawPosition != nullptr) *rawPosition = position;
            return IsFinite(position) && IsNonzero(position);
        }

        if (!native::ShouldReadAlgorithmCoordinate(
                algorithmPositionRequested_,
                algorithmDecryptRequested_) ||
            memory_ == nullptr ||
            !IsValidPointer(moduleBase_) || !IsValidPointer(actor)) {
            return false;
        }
        ++algorithmCoordinateAttemptCount_;
        ++algorithmCoordinateFrameAttemptCount_;
        Vec3 decodedRaw{};
        if (ReadAlgorithmObjectCoordinate(
                actor, component, decodedRaw, position)) {
            if (rawPosition != nullptr) *rawPosition = decodedRaw;
            return true;
        }
        if (rawPosition != nullptr) *rawPosition = decodedRaw;
        return false;
    }
#endif

    bool ReadCharacterPosition(
        const RuntimeActorRecord& record,
        std::string_view className,
        native::PositionReadMode preferredMode,
        bool antiFlicker,
        Vec3& position,
        native::CharacterPositionSource* positionSource = nullptr) {
        position = Vec3{};
        if (positionSource != nullptr) {
            *positionSource = native::CharacterPositionSource::None;
        }
#if 0
        if (hardwareBreakpointRequested_) {
            std::uintptr_t mesh = record.ordinaryMesh;
            if (!IsValidPointer(mesh)) {
                mesh = ReadPointer(
                    record.actor + native::kOrdinaryActorMeshOffset);
            }
            if (!IsValidPointer(mesh) && IsValidPointer(record.mesh)) {
                mesh = record.mesh;
            }
            const bool available =
                ReadHardwareBreakpointPosition(mesh, position);
            if (IsCoordinateTraceEnabled()) {
                auto& traceRecord =
                    coordinateTraceRecords_[record.actor];
                traceRecord = CoordinateTraceRecord{};
                traceRecord.root = record.root;
                traceRecord.component = mesh;
                traceRecord.raw = position;
                traceRecord.output = position;
                traceRecord.source = available
                    ? CoordinateTraceSource::HardwareBreakpoint
                    : CoordinateTraceSource::Failure;
                traceRecord.attempted = true;
                if (!available) {
                    traceRecord.error =
                        CoordinateDecryptError::PositionReadFailed;
                }
            }
            if (available && positionSource != nullptr) {
                *positionSource =
                    native::CharacterPositionSource::HardwareBreakpoint;
            }
            return available;
        }
#endif
        if (UsesCoordinateDecrypt2Runtime()) {
            if (!record.resolverRecord ||
                !IsValidPointer(record.root)) {
                return false;
            }
            return ReadCharacterPosition(
                record.actor,
                record.root,
                className,
                native::PositionReadMode::Direct,
                antiFlicker,
                position,
                false,
                positionSource);
        }
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        if (native::ShouldReadAlgorithmCoordinate(
                algorithmPositionRequested_,
                algorithmDecryptRequested_)) {
            std::uintptr_t ordinaryRoot = record.ordinaryRoot;
            if (!IsValidPointer(ordinaryRoot)) {
                ordinaryRoot = ReadPointer(record.actor + 0x180);
            }
            return ReadCharacterPosition(
                record.actor,
                ordinaryRoot,
                className,
                native::PositionReadMode::Standard,
                antiFlicker,
                position,
                false,
                positionSource);
        }
#endif
        if (native::ShouldKeepDecodedPositionSource(
                algorithmPositionRequested_,
                preferredMode == native::PositionReadMode::Direct)) {
            // 解密模式只允许解析器提供的根对象进入解密读取链。
            if (!record.resolverRecord || !IsValidPointer(record.root)) {
                return false;
            }
            const std::uintptr_t decodedRoot = record.root;
            return ReadCharacterPosition(
                record.actor,
                decodedRoot,
                className,
                native::PositionReadMode::Direct,
                antiFlicker,
                position,
                false,
                positionSource);
        }
        return native::ReadActorRecordSourceWithFallback(
            record,
            [&] {
                Vec3 candidate{};
                native::CharacterPositionSource candidateSource =
                    native::CharacterPositionSource::None;
                if (!ReadCharacterPosition(
                        record.actor,
                        record.root,
                        className,
                        preferredMode,
                        antiFlicker,
                        candidate,
                        false,
                        &candidateSource) ||
                    !IsFinite(candidate) || !IsNonzero(candidate)) {
                    return false;
                }
                position = candidate;
                if (positionSource != nullptr) {
                    *positionSource = candidateSource;
                }
                return true;
            },
            [&] {
                Vec3 candidate{};
                if (!ReadCharacterPosition(
                        record.actor,
                        record.ordinaryRoot,
                        className,
                        native::PositionReadMode::Standard,
                        antiFlicker,
                        candidate) ||
                    !IsFinite(candidate) || !IsNonzero(candidate)) {
                    return false;
                }
                position = candidate;
                if (positionSource != nullptr) {
                    *positionSource = native::CharacterPositionSource::Standard;
                }
                return true;
            });
    }

    bool ReadActorFacingFromMesh(std::uintptr_t mesh,
                                 Vec3& forward,
                                 float& headingRadians) {
        forward = Vec3{};
        headingRadians = 0.0f;
        if (!IsValidPointer(mesh)) return false;

        Transform transform{};
        if (!ReadValue(mesh + 0x210, transform)) return false;
        const Quaternion& rotation = transform.rotation;
        const float normSquared = rotation.x * rotation.x + rotation.y * rotation.y +
            rotation.z * rotation.z + rotation.w * rotation.w;
        if (!std::isfinite(normSquared) || normSquared < 0.25f || normSquared > 2.25f) {
            return false;
        }

        forward = Vec3{
            1.0f - 2.0f * (rotation.y * rotation.y + rotation.z * rotation.z),
            2.0f * (rotation.x * rotation.y + rotation.w * rotation.z),
            2.0f * (rotation.x * rotation.z - rotation.w * rotation.y),
        };
        const float horizontalLength = HorizontalLength(forward);
        if (!IsFinite(forward) || !std::isfinite(horizontalLength) ||
            horizontalLength <= 0.0001f) {
            forward = Vec3{};
            return false;
        }
        headingRadians = std::atan2(forward.y, forward.x);
        return std::isfinite(headingRadians);
    }

    bool ReadActorFacing(const RuntimeActorRecord& record,
                         Vec3& forward,
                         float& headingRadians) {
        return native::ReadActorRecordSourceWithFallback(
            record,
            [&] {
                return ReadActorFacingFromMesh(
                    record.mesh, forward, headingRadians);
            },
            [&] {
                return ReadActorFacingFromMesh(
                    record.ordinaryMesh, forward, headingRadians);
            });
    }

    bool HasReadableMesh(const RuntimeActorRecord& record) const {
        return (record.resolverRecord && IsValidPointer(record.mesh)) ||
            (record.ordinarySource &&
             IsValidPointer(record.ordinaryMesh));
    }

    bool HasReadableBoneArray(const RuntimeActorRecord& record) {
        const auto hasBoneArray = [this](std::uintptr_t mesh) {
            if (!IsValidPointer(mesh)) return false;
            return IsValidPointer(ReadPointer(mesh + 0x730)) ||
                IsValidPointer(ReadPointer(mesh + 0x740));
        };
        if (record.ordinarySource && hasBoneArray(record.ordinaryMesh)) {
            return true;
        }
        return record.resolverRecord && hasBoneArray(record.mesh);
    }

    bool ObserveAimWarning(
        std::uintptr_t actor,
        bool aligned,
        std::uint64_t sequence,
        std::chrono::steady_clock::time_point now) {
        constexpr float kSustainSeconds = 1.0f;
        AimWarningState& state = aimWarningStates_[actor];
        state.lastSeenSequence = sequence;
        if (!aligned) {
            state.tracking = false;
            state.active = false;
            state.alignedSince = std::chrono::steady_clock::time_point{};
            return false;
        }
        if (!state.tracking) {
            state.tracking = true;
            state.active = false;
            state.alignedSince = now;
        }
        state.active = std::chrono::duration<float>(now - state.alignedSince).count() >=
            kSustainSeconds;
        return state.active;
    }

    void FinalizeThreatSignals(const FeatureSettings& settings,
                               GameFrame& frame) {
        for (auto iterator = aimWarningStates_.begin();
             iterator != aimWarningStates_.end();) {
            if (iterator->second.lastSeenSequence != frame.sequence) {
                iterator = aimWarningStates_.erase(iterator);
            } else {
                ++iterator;
            }
        }

        if (!settings.visual.enabled) return;
        float alertY = static_cast<float>(options_.screenHeight) * 0.70f;
        const float alertStep = std::max(44.0f, 48.0f * settings.visual.fontScale);
        if (settings.visual.nearbyEnemy && frame.nearbyEnemyCount > 0) {
            WorldLabel label{};
            label.anchor = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                alertY);
            label.title = "50米内敌人数量：" + std::to_string(frame.nearbyEnemyCount);
            label.kind = WorldLabelKind::ScreenAlert;
            label.tone = SemanticTone::Danger;
            label.emphasized = true;
            frame.worldLabels.push_back(std::move(label));
            alertY += alertStep;
        }

        if (settings.visual.throwableWarning && frame.dangerousObjectNearby) {
            WorldLabel label{};
            label.anchor = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                alertY);
            label.title = "警告：附近有危险道具！";
            label.kind = WorldLabelKind::ScreenAlert;
            label.tone = SemanticTone::Danger;
            label.emphasized = true;
            frame.worldLabels.push_back(std::move(label));
            alertY += alertStep;
        }

        if (!settings.visual.aimWarning) return;
        const std::size_t visibleCount =
            std::min<std::size_t>(frame.aimWarningPlayers.size(), 3);
        for (std::size_t index = 0; index < visibleCount; ++index) {
            WorldLabel label{};
            label.anchor = ImVec2(
                static_cast<float>(options_.screenWidth) * 0.5f,
                alertY);
            if (frame.aimWarningPlayers[index].empty()) {
                label.title = "正在瞄准你";
            } else {
                label.title = frame.aimWarningPlayers[index];
                label.detail = "正在瞄准你";
            }
            label.kind = WorldLabelKind::ScreenAlert;
            label.tone = SemanticTone::Danger;
            label.emphasized = true;
            frame.worldLabels.push_back(std::move(label));
            alertY += alertStep;
        }
    }

    bool ReadCoreHealth(std::uintptr_t actor,
                        HealthState& state,
                        std::uintptr_t* resolvedHealthSet = nullptr) {
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::HealthRead);
        if (resolvedHealthSet != nullptr) {
            *resolvedHealthSet = 0;
        }
        const std::uintptr_t healthComponent = ReadPointer(actor + 0x10C8);
        const std::uintptr_t healthSet = ReadPointer(healthComponent + 0x280);
        if (!IsValidPointer(healthSet)) return false;
        constexpr std::size_t kHealthSnapshotSize = 0x1C;
        std::array<std::byte, kHealthSnapshotSize> healthSnapshot{};
        bool healthValuesRead = memory_ != nullptr &&
            memory_->Read(
                healthSet + 0x3C,
                healthSnapshot.data(),
                healthSnapshot.size());
        if (healthValuesRead) {
            std::memcpy(
                &state.health,
                healthSnapshot.data(),
                sizeof(state.health));
            std::memcpy(
                &state.maxHealth,
                healthSnapshot.data() + 0x18,
                sizeof(state.maxHealth));
        } else {
            healthValuesRead =
                ReadValue(healthSet + 0x3C, state.health) &&
                ReadValue(healthSet + 0x54, state.maxHealth);
        }
        if (!healthValuesRead ||
            !std::isfinite(state.health) || !std::isfinite(state.maxHealth) ||
            state.maxHealth <= 0.0f || state.maxHealth > 100000.0f) {
            return false;
        }
        state.health = std::clamp(state.health, 0.0f, state.maxHealth);
        if (resolvedHealthSet != nullptr) {
            *resolvedHealthSet = healthSet;
        }
        return true;
    }

    void ReadDownedState(std::uintptr_t actor,
                         std::uintptr_t healthSet,
                         HealthState& state) {
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::DownedRead);
        std::uint8_t downed = 0;
        const std::uintptr_t blackboard = ReadPointer(actor + 0x1030);
        if (IsValidPointer(blackboard)) {
            ReadValue(blackboard + 0x372, downed);
        }
        float downedHealth = 0.0f;
        float maximumDownedHealth = 0.0f;
        constexpr std::size_t kDownedSnapshotSize = 0x14;
        std::array<std::byte, kDownedSnapshotSize> downedSnapshot{};
        if (memory_ != nullptr &&
            memory_->Read(
                healthSet + 0x114,
                downedSnapshot.data(),
                downedSnapshot.size())) {
            std::memcpy(
                &downedHealth,
                downedSnapshot.data(),
                sizeof(downedHealth));
            std::memcpy(
                &maximumDownedHealth,
                downedSnapshot.data() + 0x10,
                sizeof(maximumDownedHealth));
        } else {
            ReadValue(healthSet + 0x114, downedHealth);
            ReadValue(healthSet + 0x124, maximumDownedHealth);
        }
        const bool downedPoolValid =
            std::isfinite(downedHealth) &&
            std::isfinite(maximumDownedHealth);
        state.downed = native::ResolveDownedState(
            downed != 0,
            state.health,
            downedPoolValid ? downedHealth : 0.0f,
            downedPoolValid ? maximumDownedHealth : 0.0f);
    }

    void ReadEquipment(std::uintptr_t actor,
                       native::PlayerDetailReadMask mask,
                       HealthState& state) {
        if (mask == 0) return;
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::EquipmentRead);
        const std::uintptr_t equipmentComponent = ReadPointer(actor + 0x2420);
        const std::uintptr_t equipment = ReadPointer(equipmentComponent + 0x1D8);
        if (!IsValidPointer(equipment)) return;

        if (native::HasAnyPlayerDetailReadField(
                mask, native::kPlayerEquipmentLevelReadMask)) {
            std::int32_t helmetDefinition = 0;
            std::int32_t armorDefinition = 0;
            constexpr std::size_t kDefinitionSnapshotSize = 0xC4;
            std::array<std::byte, kDefinitionSnapshotSize>
                definitionSnapshot{};
            if (memory_ != nullptr &&
                memory_->Read(
                    equipment + 0x30,
                    definitionSnapshot.data(),
                    definitionSnapshot.size())) {
                std::memcpy(
                    &helmetDefinition,
                    definitionSnapshot.data(),
                    sizeof(helmetDefinition));
                std::memcpy(
                    &armorDefinition,
                    definitionSnapshot.data() + 0xC0,
                    sizeof(armorDefinition));
            } else {
                ReadValue(equipment + 0x30, helmetDefinition);
                ReadValue(equipment + 0xF0, armorDefinition);
            }
            state.helmetLevel = EquipmentLevel(helmetDefinition);
            state.armorLevel = EquipmentLevel(armorDefinition);
        }
        if (native::HasPlayerDetailReadField(
                mask,
                native::PlayerDetailReadField::HelmetDurability)) {
            std::array<float, 2> durability{};
            if (memory_ != nullptr &&
                memory_->Read(
                    equipment + 0x48,
                    durability.data(),
                    sizeof(durability))) {
                state.helmet = durability[0];
                state.maxHelmet = durability[1];
            } else {
                ReadValue(equipment + 0x48, state.helmet);
                ReadValue(equipment + 0x4C, state.maxHelmet);
            }
        }
        if (native::HasPlayerDetailReadField(
                mask,
                native::PlayerDetailReadField::ArmorDurability)) {
            std::array<float, 2> durability{};
            if (memory_ != nullptr &&
                memory_->Read(
                    equipment + 0x108,
                    durability.data(),
                    sizeof(durability))) {
                state.armor = durability[0];
                state.maxArmor = durability[1];
            } else {
                ReadValue(equipment + 0x108, state.armor);
                ReadValue(equipment + 0x10C, state.maxArmor);
            }
        }
        if (native::HasPlayerDetailReadField(
                mask,
                native::PlayerDetailReadField::HelmetDurability)) {
            if (!std::isfinite(state.helmet) || !std::isfinite(state.maxHelmet) ||
                state.maxHelmet <= 0.0f || state.maxHelmet > 100000.0f) {
                state.helmet = 0.0f;
                state.maxHelmet = 0.0f;
            } else {
                state.helmet = std::clamp(state.helmet, 0.0f, state.maxHelmet);
            }
        }
        if (native::HasPlayerDetailReadField(
                mask,
                native::PlayerDetailReadField::ArmorDurability)) {
            if (!std::isfinite(state.armor) || !std::isfinite(state.maxArmor) ||
                state.maxArmor <= 0.0f || state.maxArmor > 100000.0f) {
                state.armor = 0.0f;
                state.maxArmor = 100.0f;
            } else {
                state.armor = std::clamp(state.armor, 0.0f, state.maxArmor);
            }
        }
    }

    std::string ReadPlayerName(std::uintptr_t playerState) {
        const std::uintptr_t nameAddress = ReadPointer(playerState + 0x470);
        if (!IsValidPointer(nameAddress)) return {};
        std::array<char16_t, 32> buffer{};
        if (!memory_->Read(nameAddress, buffer.data(), buffer.size() * sizeof(char16_t))) return {};
        std::size_t length = 0;
        while (length < buffer.size() && buffer[length] != 0) ++length;
        return Utf16ToUtf8(std::u16string(buffer.data(), length));
    }

    std::string DecodeName(std::int32_t index, std::uintptr_t namePool) {
        const auto cached = nameCache_.find(index);
        if (cached != nameCache_.end()) return cached->second;
        if (index < 0 || !IsValidPointer(namePool)) return {};

        const std::uint32_t unsignedIndex = static_cast<std::uint32_t>(index);
        const std::uint32_t blockOffset = (unsignedIndex >> 15U) & 0x1FFF8U;
        const std::uint32_t entryOffset = (unsignedIndex * 2U) & 0x7FFFEU;
        const std::uintptr_t chunk = ReadPointer(namePool + blockOffset + 0x38);
        if (!IsValidPointer(chunk) ||
            entryOffset > kMaximumRemoteAddress - chunk) return {};
        const std::uintptr_t entry = chunk + entryOffset;
        std::uint16_t header = 0;
        if (!ReadValue(entry, header)) return {};
        const std::size_t length = header >> 6U;
        if (length == 0 || length > kMaximumNameLength) return {};

        std::string bytes(length, '\0');
        if (!memory_->Read(entry + 2, bytes.data(), bytes.size())) return {};
        if ((header & 1U) != 0U) {
            const std::uint8_t key = AlternatingNameKey(length, entry);
            for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
                bytes[offset] = static_cast<char>(
                    static_cast<std::uint8_t>(bytes[offset]) ^ key);
            }
        } else {
            const std::uint8_t key = SequentialNameKey(length);
            for (char& value : bytes) {
                value = static_cast<char>(static_cast<std::uint8_t>(value) ^ key);
            }
        }

        const std::size_t terminator = bytes.find('\0');
        if (terminator != std::string::npos) bytes.resize(terminator);
        if (bytes.empty() || std::any_of(bytes.begin(), bytes.end(), [](char value) {
                const unsigned char byte = static_cast<unsigned char>(value);
                return byte < 0x20U || byte > 0x7EU;
            })) {
            bytes.clear();
        }
        if (!bytes.empty()) {
            if (nameCache_.size() > 8192) {
                nameCache_.clear();
                classTraitsCache_.clear();
            }
            nameCache_[index] = bytes;
        }
        return bytes;
    }

    FNameClassTraits ResolveClassTraits(
        std::int32_t index,
        std::string_view className) {
        if (className.empty()) return {};
        const auto cached = classTraitsCache_.find(index);
        if (cached != classTraitsCache_.end()) return cached->second;
        const FNameClassTraits traits = ClassifyFName(className);
        classTraitsCache_.emplace(index, traits);
        return traits;
    }

    static std::uint8_t AlternatingNameKey(std::size_t length,
                                           std::uintptr_t entry) {
        const std::uint32_t size = static_cast<std::uint32_t>(length);
        switch (size % 9U) {
            case 0: return static_cast<std::uint8_t>(((size & 0x1FU) + size + 128U) | 0x7FU);
            case 1: return static_cast<std::uint8_t>(((size ^ 0xDFU) + size + 128U) | 0x7FU);
            case 2: return static_cast<std::uint8_t>(((size ^ 0xCFU) + size + 128U) | 0x7FU);
            case 3: return static_cast<std::uint8_t>((33U * size + 128U) | 0x7FU);
            case 4: return static_cast<std::uint8_t>(
                (((entry >> 6U) + ((entry >> 8U) & 0xFFU) + 128U) | 0x7FU));
            case 5: return static_cast<std::uint8_t>((3U * size + 133U) | 0x7FU);
            case 6: return static_cast<std::uint8_t>((((4U * size) | 5U) + 128U) | 0x7FU);
            case 7: return static_cast<std::uint8_t>(
                ((((entry >> 10U) | 7U) + (entry >> 6U) + 128U) | 0x7FU));
            case 8: return static_cast<std::uint8_t>(((size ^ 0xCU) + size + 128U) | 0x7FU);
            default: return 0;
        }
    }

    static std::uint8_t SequentialNameKey(std::size_t length) {
        const std::uint32_t size = static_cast<std::uint32_t>(length);
        switch (size % 9U) {
            case 0: return static_cast<std::uint8_t>((((size & 0x1FU) + size) | 0x7FU) ^ 0x80U);
            case 1: return static_cast<std::uint8_t>((((size ^ 0xDFU) + size) | 0x7FU) ^ 0x80U);
            case 2: return static_cast<std::uint8_t>((((size | 0xCFU) + size) | 0x7FU) ^ 0x80U);
            case 3: return static_cast<std::uint8_t>(((33U * size) | 0x7FU) ^ 0x80U);
            case 4: return static_cast<std::uint8_t>(((size + (size >> 2U)) | 0x7FU) ^ 0x80U);
            case 5: return static_cast<std::uint8_t>(((3U * size + 5U) | 0x7FU) ^ 0x80U);
            case 6: return static_cast<std::uint8_t>(((((4U * size) | 5U) + size) | 0x7FU) ^ 0x80U);
            case 7: return static_cast<std::uint8_t>(((((size >> 4U) | 7U) + size) | 0x7FU) ^ 0x80U);
            case 8: return static_cast<std::uint8_t>((((size ^ 0xCU) + size) | 0x7FU) ^ 0x80U);
            default: return 0;
        }
    }

    native::GeometryVisibility EvaluateBoneVisibility(
        const Vec3& origin,
        BoneFrame& frame) const {
        bool anyVisible = false;
        bool anyOccluded = false;
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            if (!frame.worldValid[index]) continue;
            const native::GeometryVisibility visibility =
                TraceGeometry(origin, frame.world[index]);
            frame.visibility[index] = visibility;
            anyVisible = anyVisible ||
                visibility == native::GeometryVisibility::Visible;
            anyOccluded = anyOccluded ||
                visibility == native::GeometryVisibility::Occluded;
        }
        if (anyVisible) return native::GeometryVisibility::Visible;
        if (anyOccluded) return native::GeometryVisibility::Occluded;
        return native::GeometryVisibility::Unavailable;
    }

    bool RebuildResolvedComponentTransform(
        std::uintptr_t root,
        Transform& transform) {
        transform = Transform{};
        if (!IsValidPointer(root)) return false;

        Vec3 position{};
        native::ComponentEulerAngles euler{};
        Vec3 scale{};
        native::ComponentPositionFlag positionFlag = 0;
        if (layout_.componentPositionFlagOffset != 0) {
            if (!ReadValue(
                moduleBase_ + layout_.componentPositionFlagOffset,
                positionFlag)) {
                return false;
            }
        }
        const native::ResolvedComponentFieldAddresses addresses =
            native::ResolveComponentFieldAddresses(root, positionFlag);
        if (!ReadValue(addresses.position, position) ||
            !ReadValue(addresses.euler, euler) ||
            !ReadValue(addresses.scale, scale)) {
            return false;
        }

        const native::ResolvedComponentTransform rebuilt =
            native::BuildResolvedComponentTransform(
                native::ComponentVector3{
                    position.x,
                    position.y,
                    position.z,
                },
                euler,
                native::ComponentVector3{
                    scale.x,
                    scale.y,
                    scale.z,
                });
        transform.rotation = Quaternion{
            rebuilt.rotation.x,
            rebuilt.rotation.y,
            rebuilt.rotation.z,
            rebuilt.rotation.w,
        };
        transform.translation = Vec3{
            rebuilt.translation.x,
            rebuilt.translation.y,
            rebuilt.translation.z,
        };
        transform.scale = Vec3{
            rebuilt.scale.x,
            rebuilt.scale.y,
            rebuilt.scale.z,
        };
        return IsValidTransform(transform);
    }

    bool ReadBoneFrame(const RuntimeActorRecord& actorRecord,
                       const native::PreparedProjection& prepared,
                       bool antiFlicker,
                       const Vec3* resolvedPosition,
                       BoneFrame& frame,
                       BoneFrameReadStatus* readStatus = nullptr) {
        platform::PerformanceTraceScope trace(
            platform::PerformancePhase::BoneRead);
        constexpr auto kCacheLifetime = std::chrono::milliseconds(300);
        frame = BoneFrame{};
        if (readStatus != nullptr) {
            *readStatus = BoneFrameReadStatus::InvalidActor;
        }
        const std::uintptr_t actor = actorRecord.actor;
        if (!IsValidPointer(actor)) return false;
        const auto now = std::chrono::steady_clock::now();
        const bool useCache = antiFlicker;
        auto cached = boneCache_.find(actor);
        std::uintptr_t ordinaryMesh = actorRecord.ordinaryMesh;
        if (!IsValidPointer(ordinaryMesh) && actorRecord.resolverRecord) {
            ordinaryMesh = ReadPointer(
                actor + native::kOrdinaryActorMeshOffset);
        }
        const native::BoneFrameRecordSource boneRecord{
            actorRecord.root,
            actorRecord.mesh,
            actorRecord.encryptedRecord,
            actorRecord.resolverRecord,
        };
        const bool resolvedBoneTransformEnabled =
            native::IsResolvedBoneTransformEnabled(
                algorithmPositionRequested_,
                hardwareBreakpointRequested_);
        const native::BoneFrameSourceSelection preferredSource =
            native::SelectPreferredBoneFrameSource(
                boneRecord,
                actorRecord.ordinaryRoot,
                ordinaryMesh,
                resolvedBoneTransformEnabled);
        const native::BoneFrameSourceSelection fallbackSource =
            native::SelectFallbackBoneFrameSource(
                boneRecord,
                actorRecord.ordinaryRoot,
                ordinaryMesh,
                resolvedBoneTransformEnabled);
        if (!preferredSource && !fallbackSource) {
            if (readStatus != nullptr) {
                *readStatus = BoneFrameReadStatus::NoSource;
            }
            return false;
        }
        bool cacheFresh = useCache && cached != boneCache_.end() &&
            now - cached->second.lastUpdatedAt <= kCacheLifetime &&
            native::IsBoneFrameCacheSourceCompatible(
                boneRecord,
                actorRecord.ordinaryRoot,
                ordinaryMesh,
                resolvedBoneTransformEnabled,
                native::BoneFrameCacheSource{
                    cached->second.root,
                    cached->second.mesh,
                    cached->second.encryptedRecord,
                });

        struct BoneSourceFrame {
            native::BoneFrameSourceSelection source{};
            std::uintptr_t boneArray = 0;
            bool resolvedTranslation = false;
            BoneFrameReadStatus failureStatus =
                BoneFrameReadStatus::NoSource;
            std::array<Vec3, kBoneIndices.size()> world{};
            std::array<bool, kBoneIndices.size()> valid{};
            std::size_t validCount = 0;
            bool usable = false;
        };

        const auto hasUsableLink = [](const auto& valid) {
            for (const BoneLink& link : kBoneLinks) {
                if (link.first < valid.size() && link.second < valid.size() &&
                    valid[link.first] && valid[link.second]) {
                    return true;
                }
            }
            return false;
        };

        const auto readBoneArray =
            [this, &hasUsableLink](
                std::uintptr_t candidateBoneArray,
                const Matrix4& componentMatrix,
                const native::ResolvedComponentTransform* resolvedComponent,
                const Vec3& alignment,
                bool alignmentReady) {
                BoneSourceFrame result{};
                result.boneArray = candidateBoneArray;
                result.failureStatus = BoneFrameReadStatus::NoBoneArray;
                if (!IsValidPointer(candidateBoneArray)) return result;
                result.failureStatus = BoneFrameReadStatus::NoTransforms;

                std::array<Transform, kPlayerBoneTransformCount> transforms{};
                std::array<
                    std::byte,
                    kPlayerBoneTransformCount * kBoneTransformStride> raw{};
                const bool bulkReady = memory_ != nullptr &&
                    raw.size() <=
                        kMaximumRemoteAddress - candidateBoneArray &&
                    memory_->Read(
                        candidateBoneArray, raw.data(), raw.size());
                if (bulkReady) {
                    for (std::size_t index = 0;
                         index < transforms.size();
                         ++index) {
                        std::memcpy(
                            &transforms[index],
                            raw.data() + index * kBoneTransformStride,
                            sizeof(Transform));
                    }
                }

                for (std::size_t index = 0;
                     index < kBoneIndices.size();
                     ++index) {
                    const std::size_t boneIndex =
                        static_cast<std::size_t>(kBoneIndices[index]);
                    Transform transform{};
                    bool transformReady = bulkReady &&
                        boneIndex < transforms.size() &&
                        (resolvedComponent != nullptr
                            ? IsFinite(transforms[boneIndex].translation)
                            : IsValidTransform(transforms[boneIndex]));
                    if (transformReady) {
                        transform = transforms[boneIndex];
                    } else {
                        const std::uintptr_t address = candidateBoneArray +
                            static_cast<std::uintptr_t>(boneIndex) *
                                kBoneTransformStride;
                        transformReady = ReadValue(address, transform) &&
                            (resolvedComponent != nullptr
                                ? IsFinite(transform.translation)
                                : IsValidTransform(transform));
                    }
                    if (!transformReady) continue;

                    Vec3 world{};
                    if (resolvedComponent != nullptr) {
                        const native::ComponentVector3 resolvedWorld =
                            native::TransformResolvedBoneTranslation(
                                native::ComponentVector3{
                                    transform.translation.x,
                                    transform.translation.y,
                                    transform.translation.z,
                                },
                                *resolvedComponent);
                        world = Vec3{
                            resolvedWorld.x,
                            resolvedWorld.y,
                            resolvedWorld.z,
                        };
                    } else {
                        world = MatrixTranslation(Multiply(
                            TransformToMatrix(transform), componentMatrix));
                    }
                    if (alignmentReady) {
                        world.x += alignment.x;
                        world.y += alignment.y;
                        world.z += alignment.z;
                    }
                    if (index == 0) world.z += 7.0f;
                    if (!IsFinite(world)) continue;
                    result.world[index] = world;
                    result.valid[index] = true;
                    ++result.validCount;
                }
                result.usable = hasUsableLink(result.valid);
                return result;
            };

        const auto readBoneSource =
            [this,
             &readBoneArray,
             resolvedPosition](
                const native::BoneFrameSourceSelection& source) {
                BoneSourceFrame result{};
                result.source = source;
                if (!source || !IsValidPointer(source.mesh)) {
                    return result;
                }
                result.failureStatus = BoneFrameReadStatus::NoComponent;

                Transform componentTransform{};
                const Vec3* standardizedPosition = resolvedPosition;
                const bool componentReady = source.rebuildResolvedTransform
                    ? RebuildResolvedComponentTransform(
                          source.root,
                          componentTransform)
                    : ReadValue(source.mesh + 0x210, componentTransform);
                if (!componentReady) return result;

                result.failureStatus = BoneFrameReadStatus::NoBoneArray;

                const Matrix4 componentMatrix =
                    TransformToMatrix(componentTransform);
                const native::ResolvedComponentTransform resolvedComponent{
                    native::ComponentQuaternion{
                        componentTransform.rotation.x,
                        componentTransform.rotation.y,
                        componentTransform.rotation.z,
                        componentTransform.rotation.w,
                    },
                    native::ComponentVector3{
                        componentTransform.translation.x,
                        componentTransform.translation.y,
                        componentTransform.translation.z,
                    },
                    native::ComponentVector3{
                        componentTransform.scale.x,
                        componentTransform.scale.y,
                        componentTransform.scale.z,
                    },
                };
                const native::ResolvedComponentTransform*
                    resolvedComponentPointer =
                        source.rebuildResolvedTransform
                        ? &resolvedComponent
                        : nullptr;
                Vec3 alignment{};
                bool alignmentReady = false;
                if (standardizedPosition != nullptr &&
                    IsFinite(*standardizedPosition)) {
                    if (source.rebuildResolvedTransform) {
                        const native::ComponentVector3
                            resolvedAlignment =
                                native::BuildResolvedBoneAlignment(
                                    native::ComponentVector3{
                                        standardizedPosition->x,
                                        standardizedPosition->y,
                                        standardizedPosition->z,
                                    },
                                    resolvedComponent.translation);
                        alignment = Vec3{
                            resolvedAlignment.x,
                            resolvedAlignment.y,
                            resolvedAlignment.z,
                        };
                    } else {
                        alignment = Subtract(
                            *standardizedPosition,
                            MatrixTranslation(componentMatrix));
                    }
                    alignmentReady = IsFinite(alignment);
                }

                const std::uintptr_t primaryBoneArray =
                    ReadPointer(source.mesh + 0x730);
                result = readBoneArray(
                    primaryBoneArray,
                    componentMatrix,
                    resolvedComponentPointer,
                    alignment,
                    alignmentReady);
                result.source = source;
                result.resolvedTranslation =
                    source.rebuildResolvedTransform || alignmentReady;

                if (native::ShouldReadSecondaryBoneArray(
                        source.rebuildResolvedTransform,
                        IsValidPointer(primaryBoneArray),
                        result.validCount,
                        kBoneIndices.size())) {
                    const std::uintptr_t secondaryBoneArray =
                        ReadPointer(source.mesh + 0x740);
                    if (IsValidPointer(secondaryBoneArray) &&
                        secondaryBoneArray != primaryBoneArray) {
                        BoneSourceFrame secondary = readBoneArray(
                            secondaryBoneArray,
                            componentMatrix,
                            resolvedComponentPointer,
                            alignment,
                            alignmentReady);
                        secondary.source = source;
                        secondary.resolvedTranslation =
                            source.rebuildResolvedTransform || alignmentReady;
                        if (native::PreferBoneFrameCandidate(
                                result.validCount,
                                result.usable,
                                secondary.validCount,
                                secondary.usable)) {
                            result = std::move(secondary);
                        }
                    }
                }
                return result;
            };

        BoneSourceFrame current = readBoneSource(preferredSource);
        if (current.validCount < kBoneIndices.size() && fallbackSource) {
            BoneSourceFrame fallback = readBoneSource(fallbackSource);
            if (native::PreferBoneFrameCandidate(
                    current.validCount,
                    current.usable,
                    fallback.validCount,
                    fallback.usable)) {
                current = std::move(fallback);
            }
        }

        if (useCache && current.validCount != 0) {
            BoneCacheEntry& entry = boneCache_[actor];
            const native::BoneFrameCacheSource entrySource{
                entry.root,
                entry.mesh,
                entry.encryptedRecord,
            };
            if (native::ShouldResetBoneFrameCache(
                    current.source,
                    current.boneArray,
                    current.resolvedTranslation,
                    entrySource,
                    entry.boneArray,
                    entry.resolvedTranslation)) {
                entry = BoneCacheEntry{};
            }
            entry.root = current.source.root;
            entry.mesh = current.source.mesh;
            entry.boneArray = current.boneArray;
            entry.encryptedRecord =
                current.source.rebuildResolvedTransform;
            entry.resolvedTranslation = current.resolvedTranslation;
            for (std::size_t index = 0;
                 index < current.valid.size();
                 ++index) {
                if (!current.valid[index]) continue;
                entry.world[index] = current.world[index];
                entry.valid[index] = true;
                entry.boneUpdatedAt[index] = now;
            }
            entry.lastUpdatedAt = now;
            cached = boneCache_.find(actor);
            cacheFresh = true;
        }

        std::array<Vec3, kBoneIndices.size()> world = current.world;
        std::array<bool, kBoneIndices.size()> worldValid = current.valid;
        if (cacheFresh && cached != boneCache_.end()) {
            for (std::size_t index = 0;
                 index < worldValid.size();
                 ++index) {
                if (worldValid[index] || !cached->second.valid[index] ||
                    now - cached->second.boneUpdatedAt[index] >
                        kCacheLifetime) {
                    continue;
                }
                world[index] = cached->second.world[index];
                worldValid[index] = true;
            }
        }
        const bool anyWorld = std::any_of(
            worldValid.begin(),
            worldValid.end(),
            [](bool valid) { return valid; });
        if (!anyWorld) {
            if (readStatus != nullptr) {
                *readStatus = current.failureStatus;
            }
            return false;
        }

        bool anyProjected = false;
        for (std::size_t index = 0; index < world.size(); ++index) {
            if (!worldValid[index] || !IsFinite(world[index])) continue;
            frame.world[index] = world[index];
            frame.worldValid[index] = true;
            Vec2 screen{};
            if (!ProjectToScreen(
                    world[index],
                    prepared,
                    screen) ||
                !IsInsideScreen(
                    screen,
                    options_.screenWidth,
                    options_.screenHeight,
                    80.0f)) {
                continue;
            }
            frame.screen[index] = screen;
            frame.valid[index] = true;
            anyProjected = true;
        }
        if (readStatus != nullptr) {
            *readStatus = anyProjected
                ? BoneFrameReadStatus::Ready
                : BoneFrameReadStatus::Offscreen;
        }
        return anyProjected;
    }

    static bool TryBuildBoneBounds(const BoneFrame& frame,
                                   ScreenRect& bounds) {
        bounds = ScreenRect{};
        std::array<
            native::PlayerBoneScreenPoint,
            native::kPlayerBoundsBoneCount> points{};
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            points[index] = native::PlayerBoneScreenPoint{
                frame.screen[index].x,
                frame.screen[index].y,
                frame.valid[index],
            };
        }
        native::PlayerScreenBounds resolved{};
        if (!native::CalculatePlayerScreenBounds(points, resolved)) {
            return false;
        }
        bounds = ScreenRect{
            resolved.left,
            resolved.top,
            resolved.right,
            resolved.bottom,
        };
        return true;
    }

    static void BuildSkeletonVisual(const BoneFrame& frame,
                                    bool colorByVisibility,
                                    SkeletonVisual& skeleton) {
        skeleton.joints.resize(kBoneIndices.size());
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            if (!frame.valid[index]) continue;
            skeleton.joints[index] = BoneJoint{
                ImVec2(frame.screen[index].x, frame.screen[index].y),
                true,
                frame.visibility[index]};
        }
        skeleton.links.assign(kBoneLinks.begin(), kBoneLinks.end());
        skeleton.colorByVisibility = colorByVisibility;
    }

    bool ReadStableCameraView(
        std::uintptr_t world,
        std::uintptr_t cameraManager,
        std::uint64_t frameSequence,
        CameraView& view) {
        CameraView first{};
        CameraView second{};
        const bool firstValid =
            ReadValue(cameraManager + 0x3590, first) &&
            IsFinite(first);
        const bool secondValid =
            ReadValue(cameraManager + 0x3590, second) &&
            IsFinite(second);
        if (!firstValid && !secondValid) {
            if (!native::IsProjectionViewCacheCompatible(
                    lastViewValid_,
                    lastViewWorld_,
                    lastViewCameraManager_,
                    world,
                    cameraManager)) {
                return false;
            }
            view = lastView_;
            return true;
        }

        CameraView candidate = secondValid ? second : first;
        float stableFov = 0.0f;
        if (!native::ResolveStableProjectionFov(
                world,
                cameraManager,
                frameSequence,
                firstValid,
                first.fieldOfView,
                secondValid,
                second.fieldOfView,
                viewFovState_,
                stableFov)) {
            return false;
        }
        candidate.fieldOfView = stableFov;
        view = candidate;
        lastView_ = candidate;
        lastViewWorld_ = world;
        lastViewCameraManager_ = cameraManager;
        lastViewValid_ = true;
        return true;
    }

    void RefreshCameraView(
        FrameContext& context,
        std::uint64_t frameSequence) {
        CameraView refreshed{};
        if (ReadStableCameraView(
                context.world,
                context.cameraManager,
                frameSequence,
                refreshed)) {
            context.view = refreshed;
            context.preparedProjection = PrepareProjection(
                refreshed, options_.screenWidth, options_.screenHeight);
        }
    }

    bool ReprojectBoneFrame(const BoneFrame& source,
                            const native::PreparedProjection& prepared,
                            BoneFrame& projected) const {
        projected = BoneFrame{};
        bool anyProjected = false;
        for (std::size_t index = 0;
             index < projected.world.size();
             ++index) {
            if (!source.worldValid[index]) continue;
            projected.world[index] = source.world[index];
            projected.worldValid[index] = true;
            Vec2 screen{};
            if (!ProjectToScreen(
                    source.world[index],
                    prepared,
                    screen) ||
                !IsInsideScreen(
                    screen,
                    options_.screenWidth,
                    options_.screenHeight,
                    80.0f)) {
                continue;
            }
            projected.screen[index] = screen;
            projected.valid[index] = true;
            projected.visibility[index] = source.visibility[index];
            anyProjected = true;
        }
        return anyProjected;
    }

    void ReprojectPlayers(
        const native::PreparedProjection& prepared,
        const std::vector<PlayerProjectionSource>& sources,
        GameFrame& frame) const {
        std::size_t writeIndex = 0;
        for (const PlayerProjectionSource& source : sources) {
            if (source.playerIndex >= frame.players.size()) continue;
            Vec2 bottom{};
            Vec2 top{};
            const bool bottomProjected = ProjectToScreen(
                source.bottom,
                prepared,
                bottom);
            const bool topProjected = ProjectToScreen(
                source.top,
                prepared,
                top);

            BoneFrame bones{};
            const bool bonesReady = source.bonesReady &&
                ReprojectBoneFrame(source.bones, prepared, bones);
            ScreenRect boneBounds{};
            const bool boneBoundsReady = bonesReady &&
                TryBuildBoneBounds(bones, boneBounds);
            native::PlayerScreenBounds anchorBounds{};
            const bool anchorBoundsReady =
                native::CalculatePlayerAnchorBounds(
                    native::PlayerBoneScreenPoint{
                        bottom.x, bottom.y, bottomProjected},
                    native::PlayerBoneScreenPoint{
                        top.x, top.y, topProjected},
                    anchorBounds);
            const native::PlayerScreenBounds resolvedBoneBounds{
                boneBounds.left,
                boneBounds.top,
                boneBounds.right,
                boneBounds.bottom,
            };
            native::PlayerScreenBounds resolved{};
            if (!native::SelectPlayerScreenBounds(
                    boneBoundsReady,
                    resolvedBoneBounds,
                    anchorBoundsReady,
                    anchorBounds,
                    static_cast<float>(options_.screenWidth),
                    static_cast<float>(options_.screenHeight),
                    resolved)) {
                continue;
            }
            const float height = resolved.bottom - resolved.top;
            if (!std::isfinite(height) || height < 8.0f) {
                continue;
            }

            PlayerVisual& visual = frame.players[source.playerIndex];
            visual.bounds = ScreenRect{
                resolved.left,
                resolved.top,
                resolved.right,
                resolved.bottom,
            };
            if (visual.drawSkeleton) {
                const bool colorByVisibility =
                    visual.skeleton.colorByVisibility;
                visual.skeleton.joints.clear();
                visual.skeleton.links.clear();
                visual.skeleton.selectedJoint = -1;
                if (bonesReady) {
                    BuildSkeletonVisual(
                        bones, colorByVisibility, visual.skeleton);
                }
                if (visual.skeleton.joints.empty()) {
                    visual.drawSkeleton = false;
                }
            }
            if (writeIndex != source.playerIndex) {
                frame.players[writeIndex] = std::move(visual);
            }
            ++writeIndex;
        }
        frame.players.resize(writeIndex);
    }

    void ReprojectAimCandidates(
        const native::PreparedProjection& prepared,
        const ui::AimSettings& settings,
        const ui::AimTuning& tuning,
        std::vector<AimCandidate>& candidates) const {
        const float centerX =
            static_cast<float>(options_.screenWidth) * 0.5f;
        const float centerY =
            static_cast<float>(options_.screenHeight) * 0.5f;
        candidates.erase(
            std::remove_if(
                candidates.begin(),
                candidates.end(),
                [&](AimCandidate& candidate) {
                    Vec2 screen{};
                    if (!ProjectToScreen(
                            candidate.world,
                            prepared,
                            screen)) {
                        return true;
                    }
                    candidate.screen = screen;
                    candidate.screenDistancePixels = std::hypot(
                        screen.x - centerX,
                        screen.y - centerY);
                    candidate.selectionDistancePixels =
                        candidate.screenDistancePixels;
                    return !std::isfinite(
                               candidate.screenDistancePixels) ||
                        (settings.enforceFov &&
                         candidate.screenDistancePixels >
                             tuning.rangePixels);
                }),
            candidates.end());
    }

    void BuildModelGeometry(
        const CameraView& view,
        const native::PreparedProjection& prepared,
        GameFrame& frame) const {
        if (!geometrySnapshotReady_) return;

        Vec3 forward{};
        Vec3 right{};
        Vec3 up{};
        CameraAxes(view, forward, right, up);
        (void)right;
        (void)up;
        const Vec3 target{
            view.location.x + forward.x * 100000.0f,
            view.location.y + forward.y * 100000.0f,
            view.location.z + forward.z * 100000.0f,
        };
        const native::GeometryRaycastHit hit = geometryRuntime_.Raycast(
            ToGeometryPoint(view.location), ToGeometryPoint(target));
        if (!hit || hit.mesh->indices.size() < 3 ||
            hit.mesh->vertices.size() < 3) {
            return;
        }

        constexpr std::size_t kMaximumRenderedTriangles = 2500;
        GeometryModelVisual model{};
        model.tone = SemanticTone::Ally;
        model.segments.reserve(kMaximumRenderedTriangles * 3);
        const std::size_t triangleCount = hit.mesh->indices.size() / 3;

        struct CachedVertexProjection {
            Vec2 screen{};
            std::uint32_t generation = 0;
            bool valid = false;
        };
        thread_local std::vector<CachedVertexProjection> projectedVertices;
        thread_local std::uint32_t projectionGeneration = 0;
        if (projectedVertices.size() < hit.mesh->vertices.size()) {
            projectedVertices.resize(hit.mesh->vertices.size());
        }
        ++projectionGeneration;
        if (projectionGeneration == 0) {
            for (CachedVertexProjection& cached : projectedVertices) {
                cached.generation = 0;
            }
            projectionGeneration = 1;
        }
        const auto projectVertex =
            [&](std::uint32_t vertexIndex, Vec2& screen) {
                CachedVertexProjection& cached =
                    projectedVertices[vertexIndex];
                if (cached.generation != projectionGeneration) {
                    const auto& world = hit.mesh->vertices[vertexIndex];
                    cached.valid = ProjectToScreen(
                        Vec3{world.x, world.y, world.z},
                        prepared,
                        cached.screen);
                    cached.generation = projectionGeneration;
                }
                if (!cached.valid) return false;
                screen = cached.screen;
                return true;
            };

        const auto appendTriangle = [&](std::size_t triangleIndex) {
            if (triangleIndex >= triangleCount ||
                model.segments.size() >= kMaximumRenderedTriangles * 3) {
                return;
            }
            const std::size_t base = triangleIndex * 3;
            const std::uint32_t firstIndex = hit.mesh->indices[base];
            const std::uint32_t secondIndex = hit.mesh->indices[base + 1];
            const std::uint32_t thirdIndex = hit.mesh->indices[base + 2];
            if (firstIndex >= hit.mesh->vertices.size() ||
                secondIndex >= hit.mesh->vertices.size() ||
                thirdIndex >= hit.mesh->vertices.size()) {
                return;
            }
            Vec2 first{};
            Vec2 second{};
            Vec2 third{};
            if (!projectVertex(firstIndex, first) ||
                !projectVertex(secondIndex, second) ||
                !projectVertex(thirdIndex, third)) {
                return;
            }
            constexpr float kProjectionMargin = 160.0f;
            if (!IsInsideScreen(
                    first,
                    options_.screenWidth,
                    options_.screenHeight,
                    kProjectionMargin) &&
                !IsInsideScreen(
                    second,
                    options_.screenWidth,
                    options_.screenHeight,
                    kProjectionMargin) &&
                !IsInsideScreen(
                    third,
                    options_.screenWidth,
                    options_.screenHeight,
                    kProjectionMargin)) {
                return;
            }
            model.segments.push_back(GeometrySegmentVisual{
                ImVec2(first.x, first.y), ImVec2(second.x, second.y)});
            model.segments.push_back(GeometrySegmentVisual{
                ImVec2(second.x, second.y), ImVec2(third.x, third.y)});
            model.segments.push_back(GeometrySegmentVisual{
                ImVec2(third.x, third.y), ImVec2(first.x, first.y)});
        };

        const std::size_t hitTriangle =
            static_cast<std::size_t>(hit.triangleIndex);
        if (hitTriangle < triangleCount) appendTriangle(hitTriangle);
        const std::size_t stride = triangleCount <= kMaximumRenderedTriangles
            ? 1
            : (triangleCount - 1) / kMaximumRenderedTriangles + 1;
        for (std::size_t triangle = 0;
             triangle < triangleCount &&
             model.segments.size() < kMaximumRenderedTriangles * 3;
             triangle += stride) {
            if (triangle == hitTriangle) continue;
            appendTriangle(triangle);
        }
        if (!model.segments.empty()) {
            frame.modelGeometry = std::move(model);
        }
    }

    static std::size_t WeaponProfileIndex(std::uint64_t weaponId) {
        return WeaponProfileFor(weaponId);
    }

    static ui::AimTuning ResolveAimTuning(const ui::AimSettings& settings,
                                           std::uint64_t weaponId) {
        ui::AimTuning tuning = settings.weaponProfilesEnabled
            ? settings.weaponProfiles[WeaponProfileIndex(weaponId)]
            : settings.defaults;
        const auto finiteOr = [](float value, float fallback) {
            return std::isfinite(value) ? value : fallback;
        };
        tuning.rangePixels = std::clamp(finiteOr(tuning.rangePixels, 150.0f), 0.0f, 4000.0f);
        tuning.hipDistanceMeters = std::clamp(
            finiteOr(tuning.hipDistanceMeters, 50.0f), 0.0f, 1000.0f);
        tuning.adsDistanceMeters = std::clamp(
            finiteOr(tuning.adsDistanceMeters, 150.0f), 0.0f, 1000.0f);
        tuning.hipSpeed = std::clamp(finiteOr(tuning.hipSpeed, 30.0f), 1.0f, 100.0f);
        tuning.adsSpeed = std::clamp(finiteOr(tuning.adsSpeed, 30.0f), 1.0f, 100.0f);
        tuning.horizontalSpeed = std::clamp(
            finiteOr(tuning.horizontalSpeed, 70.0f), 0.0f, 400.0f);
        tuning.verticalSpeed = std::clamp(
            finiteOr(tuning.verticalSpeed, 70.0f), 0.0f, 400.0f);
        tuning.prediction = std::clamp(finiteOr(tuning.prediction, 1.0f), 0.0f, 2.0f);
        tuning.recoil = std::clamp(finiteOr(tuning.recoil, 0.1f), 0.0f, 2.0f);
        tuning.smoothing = std::clamp(finiteOr(tuning.smoothing, 20.0f), 0.01f, 200.0f);
        tuning.hipBone = std::clamp(tuning.hipBone, 0, 7);
        tuning.adsBone = std::clamp(tuning.adsBone, 0, 7);
        return tuning;
    }

    static bool AimTriggered(const ui::AimSettings& settings,
                             const FrameContext& context) {
        switch (settings.triggerMode) {
            case 0: return context.firing;
            case 1: return context.zooming;
            case 2: return context.firing || context.zooming;
            default: return false;
        }
    }

    static float RangeTargetAimHeight(int mode) noexcept {
        constexpr std::array<float, 8> heights{
            75.0f,
            55.0f,
            5.0f,
            40.0f,
            -20.0f,
            -70.0f,
            30.0f,
            30.0f,
        };
        return mode >= 0 && mode < static_cast<int>(heights.size())
            ? heights[static_cast<std::size_t>(mode)]
            : heights[6];
    }

    bool SelectRangeTargetAimPoint(
        const Vec3& root,
        int mode,
        const ui::AimSettings& settings,
        const Vec3& traceOrigin,
        const native::PreparedProjection& prepared,
        Vec3& world,
        Vec2& screen) const {
        if (!IsFinite(root)) return false;
        world = root;
        world.z += RangeTargetAimHeight(mode);
        if (!IsFinite(world)) return false;
        const native::GeometryVisibility visibility =
            TraceGeometry(traceOrigin, world);
        if (settings.missMode &&
            visibility != native::GeometryVisibility::Visible) {
            return false;
        }
        if (settings.requireVisibility &&
            visibility == native::GeometryVisibility::Occluded) {
            return false;
        }
        return ProjectToScreen(
            world,
            prepared,
            screen);
    }

    bool SelectAimPoint(const BoneFrame& frame,
                        int mode,
                        const ui::AimSettings& settings,
                        const Vec3& traceOrigin,
                        std::uint64_t identity,
                        std::uint64_t sequence,
                        int preferredBone,
                        Vec3& world,
                        Vec2& screen,
        int& boneIndex) const {
        const auto selectable = [&](std::size_t index) {
            if (index >= frame.valid.size() || !frame.valid[index]) {
                return false;
            }
            if (settings.missMode &&
                frame.visibility[index] !=
                    native::GeometryVisibility::Visible) {
                return false;
            }
            return !settings.requireVisibility ||
                frame.visibility[index] !=
                    native::GeometryVisibility::Occluded;
        };
        const auto select = [&](int index) {
            if (index < 0 || index >= static_cast<int>(frame.valid.size()) ||
                !selectable(static_cast<std::size_t>(index))) {
                return false;
            }
            const std::size_t selected = static_cast<std::size_t>(index);
            world = frame.world[selected];
            screen = frame.screen[selected];
            boneIndex = index;
            return true;
        };
        const auto selectPair = [&](int first, int second) {
            const bool firstValid = selectable(static_cast<std::size_t>(first));
            const bool secondValid = selectable(static_cast<std::size_t>(second));
            if (firstValid && secondValid) {
                world = Vec3{
                    (frame.world[first].x + frame.world[second].x) * 0.5f,
                    (frame.world[first].y + frame.world[second].y) * 0.5f,
                    (frame.world[first].z + frame.world[second].z) * 0.5f,
                };
                screen = Vec2{
                    (frame.screen[first].x + frame.screen[second].x) * 0.5f,
                    (frame.screen[first].y + frame.screen[second].y) * 0.5f,
                };
                if (settings.missMode &&
                    TraceGeometry(traceOrigin, world) !=
                        native::GeometryVisibility::Visible) {
                    return select(first) || select(second);
                }
                boneIndex = first;
                return true;
            }
            return select(first) || select(second);
        };

        if (settings.missMode && settings.coverMode == 0) {
            const aim::CoverPointSelection selection =
                aim::SelectCoverPoint(
                    settings.coverMode,
                    mode,
                    frame.valid,
                    frame.visibility);
            return selection.selected &&
                select(static_cast<int>(selection.pointIndex));
        }

        switch (mode) {
            case 0: if (select(0)) return true; break;
            case 1: if (select(1)) return true; break;
            case 2: if (select(2)) return true; break;
            case 3: if (selectPair(9, 10)) return true; break;
            case 4: if (selectPair(11, 12)) return true; break;
            case 5: if (selectPair(13, 14)) return true; break;
            case 6: {
                if (preferredBone >= 0 && select(preferredBone)) return true;
                constexpr std::array<int, 15> regions{
                    0, 1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8};
                float totalWeight = 0.0f;
                for (std::size_t index = 0; index < frame.valid.size(); ++index) {
                    if (!selectable(index)) continue;
                    const float weight = settings.randomBoneWeights[
                        static_cast<std::size_t>(regions[index])];
                    if (std::isfinite(weight) && weight > 0.0f) totalWeight += weight;
                }
                if (totalWeight > 0.0f) {
                    std::uint64_t hash = identity ^ ((sequence / 120U) + 0x9E3779B97F4A7C15ULL);
                    hash ^= hash >> 30U;
                    hash *= 0xBF58476D1CE4E5B9ULL;
                    hash ^= hash >> 27U;
                    hash *= 0x94D049BB133111EBULL;
                    hash ^= hash >> 31U;
                    float choice = static_cast<float>(hash % 1000000ULL) /
                        1000000.0f * totalWeight;
                    for (std::size_t index = 0; index < frame.valid.size(); ++index) {
                        if (!selectable(index)) continue;
                        const float weight = settings.randomBoneWeights[
                            static_cast<std::size_t>(regions[index])];
                        if (!std::isfinite(weight) || weight <= 0.0f) continue;
                        if (choice <= weight) return select(static_cast<int>(index));
                        choice -= weight;
                    }
                }
                break;
            }
            case 7: {
                if (preferredBone >= 0 && select(preferredBone)) return true;
                const float centerX = static_cast<float>(options_.screenWidth) * 0.5f;
                const float centerY = static_cast<float>(options_.screenHeight) * 0.5f;
                float bestDistance = std::numeric_limits<float>::max();
                int bestIndex = -1;
                for (std::size_t index = 0; index < frame.valid.size(); ++index) {
                    if (!selectable(index)) continue;
                    const float dx = frame.screen[index].x - centerX;
                    const float dy = frame.screen[index].y - centerY;
                    const float distance = dx * dx + dy * dy;
                    if (distance < bestDistance) {
                        bestDistance = distance;
                        bestIndex = static_cast<int>(index);
                    }
                }
                if (bestIndex >= 0) return select(bestIndex);
                break;
            }
            default: break;
        }
        for (std::size_t index = 0; index < frame.valid.size(); ++index) {
            if (select(static_cast<int>(index))) return true;
        }
        return false;
    }

#if LENGJING_ENABLE_PROJECTILE_TRACKING
    bool ReadTrackingMeshTypeFromMesh(
        std::uintptr_t mesh,
        std::int32_t& meshType) {
        meshType = 0;
        if (!IsValidPointer(mesh)) return false;
        if (ReadValue(mesh + 0x738, meshType) &&
            meshType >= 1 && meshType <= 100) {
            return true;
        }
        meshType = 0;
        return ReadValue(mesh + 0x748, meshType) &&
            meshType >= 1 && meshType <= 100;
    }

    bool ReadTrackingMeshType(
        const RuntimeActorRecord& record,
        std::int32_t& meshType) {
        return native::ReadActorRecordSourceWithFallback(
            record,
            [&] {
                return ReadTrackingMeshTypeFromMesh(
                    record.mesh, meshType);
            },
            [&] {
                return ReadTrackingMeshTypeFromMesh(
                    record.ordinaryMesh, meshType);
            });
    }

    bool PassTrackingAcquisitionHealth(
        std::uintptr_t actor,
        bool ignoreDowned) {
        HealthState health{};
        std::uintptr_t healthSet = 0;
        if (!ReadCoreHealth(actor, health, &healthSet)) {
            return false;
        }
        if (health.health <= 0.0f) {
            ReadDownedState(actor, healthSet, health);
        }
        return health.health > 0.0f ||
            (health.downed && !ignoreDowned);
    }

    bool PassTrackingCategory(
        std::uintptr_t actor,
        const ui::AimSettings& settings) {
        std::uintptr_t extra = 0;
        ReadValue(actor + 0x2788, extra);
        if (extra != 0) {
            std::uintptr_t stateRoot = 0;
            if (extra <=
                std::numeric_limits<std::uintptr_t>::max() - 0x1030) {
                ReadValue(extra + 0x1030, stateRoot);
            }
            std::uint8_t state = 0;
            if (stateRoot != 0 &&
                stateRoot <=
                    std::numeric_limits<std::uintptr_t>::max() - 0x2A1) {
                ReadValue(stateRoot + 0x2A1, state);
            }
            if (state == 0) return settings.playerDeadBox;
        }
        return settings.robotDeadBox;
    }

    void CollectTrackingCandidateFromSample(
        const RuntimeActorRecord& record,
        const FrameContext& context,
        const ui::AimSettings& settings,
        const ui::AimTuning& tuning,
        bool rangeTargetClass,
        std::uintptr_t playerState,
        const Vec3& position,
        float distanceMeters,
        const BoneFrame& boneFrame,
        bool boneFrameReady,
        std::uint64_t sequence,
        std::unordered_set<std::uintptr_t>& seenActors,
        std::vector<AimCandidate>& candidates) {
        const std::uint64_t identity =
            native::ResolvePlayerIdentity(record.actor, playerState);
        const int aimMode = context.zooming
            ? tuning.adsBone
            : tuning.hipBone;
        const int preferredBone = settings.persistentLock &&
                identity == lockedTrackingIdentity_
            ? lockedTrackingBone_
            : -1;
        Vec3 targetPoint{};
        Vec2 targetScreen{};
        int selectedBone = -1;
        bool aimPointReady = boneFrameReady && SelectAimPoint(
            boneFrame,
            aimMode,
            settings,
            context.view.location,
            identity,
            sequence,
            preferredBone,
            targetPoint,
            targetScreen,
            selectedBone);
        if (!aimPointReady && rangeTargetClass) {
            aimPointReady = SelectRangeTargetAimPoint(
                position,
                aimMode,
                settings,
                context.view.location,
                context.preparedProjection,
                targetPoint,
                targetScreen);
        }
        if (!aimPointReady) return;

        const float centerX =
            static_cast<float>(options_.screenWidth) * 0.5f;
        const float centerY =
            static_cast<float>(options_.screenHeight) * 0.5f;
        const float selectionDistance = std::hypot(
            targetScreen.x - centerX,
            targetScreen.y - centerY);
        const float distanceLimit = context.zooming
            ? tuning.adsDistanceMeters
            : tuning.hipDistanceMeters;
        if (!std::isfinite(selectionDistance) ||
            (settings.enforceFov &&
             selectionDistance > tuning.rangePixels) ||
            (settings.enforceDistance && distanceMeters > distanceLimit) ||
            !seenActors.insert(record.actor).second) {
            return;
        }

        Vec3 velocity{};
        const std::uintptr_t movement = ReadPointer(record.actor + 0x3D8);
        if (IsValidPointer(movement)) {
            ReadValue(movement + 0x2B0, velocity);
        }
        if (!IsFinite(velocity)) velocity = Vec3{};
        candidates.push_back(AimCandidate{
            identity,
            record.actor,
            record.root,
            record.mesh,
            targetPoint,
            velocity,
            targetScreen,
            selectionDistance,
            distanceMeters,
            selectionDistance,
            selectedBone,
            record.encryptedRecord,
            false,
            rangeTargetClass,
            false,
            true,
        });
    }

    void CollectTrackingAcquisitionCandidate(
        const RuntimeActorRecord& sourceRecord,
        const FrameContext& context,
        const ui::AimSettings& settings,
        const ui::AimTuning& tuning,
        bool antiFlicker,
        bool battlefieldMode,
        std::uint64_t sequence,
        std::unordered_set<std::uintptr_t>& seenActors,
        std::vector<AimCandidate>& candidates) {
        RuntimeActorRecord record = sourceRecord;
        native::FillOrdinaryActorPointers(
            record,
            [&](std::uintptr_t address) { return ReadPointer(address); });
        if (!record.resolverRecord ||
            !IsValidPointer(record.actor) ||
            ((!IsValidPointer(record.root) ||
              !IsValidPointer(record.mesh)) &&
             (!record.ordinarySource ||
              !IsValidPointer(record.ordinaryRoot) ||
              !IsValidPointer(record.ordinaryMesh)))) {
            return;
        }

        std::uintptr_t actorClass = 0;
        if (!ReadValue(record.actor, actorClass) ||
            actorClass < moduleBase_) {
            return;
        }
        const std::uintptr_t classOffset = actorClass - moduleBase_;
        const bool rangeTargetClass = classOffset == 0x1A3D6A20ULL;
        const bool broadClass = std::find(
            kTrackingClassOffsets.begin(),
            kTrackingClassOffsets.end(),
            classOffset) != kTrackingClassOffsets.end();
        if (!broadClass) return;

        std::uint8_t excludedState = 0;
        ReadValue(record.actor + 0xDD8, excludedState);
        if (excludedState != 0) {
            return;
        }
        std::uint8_t targetState = classOffset == 0x1A3D6A20ULL
            ? 1
            : 0;
        if (targetState == 0) {
            const std::uintptr_t targetStateRoot =
                ReadPointer(record.actor + 0x1030);
            if (IsValidPointer(targetStateRoot)) {
                ReadValue(targetStateRoot + 0x2A1, targetState);
            }
        }
        const std::uintptr_t playerState =
            ReadPointer(record.actor + 0x390);
        if (!rangeTargetClass) {
            const bool botClass = !IsValidPointer(playerState);
            const bool useSecondaryTeam =
                context.warfare || battlefieldMode;
            std::int32_t primaryTeam = native::kUnknownPlayerTeam;
            std::int32_t secondaryTeam = native::kUnknownPlayerTeam;
            if (botClass) {
                primaryTeam = 0;
                secondaryTeam = 0;
            } else {
                const bool primaryTeamRead =
                    ReadValue(playerState + 0x658, primaryTeam);
                primaryTeam = native::ResolvePlayerTeam(
                    primaryTeamRead, primaryTeam, false);
                if (useSecondaryTeam) {
                    const bool secondaryTeamRead =
                        ReadValue(playerState + 0x65C, secondaryTeam);
                    secondaryTeam = native::ResolvePlayerTeam(
                        secondaryTeamRead, secondaryTeam, false);
                }
            }
            const std::int32_t targetTeam =
                useSecondaryTeam ? secondaryTeam : primaryTeam;
            if (native::IsPlayerTeammate(
                    context.localTeam,
                    targetTeam,
                    useSecondaryTeam,
                    context.localPrimaryTeam,
                    primaryTeam,
                    context.localSecondaryTeam,
                    secondaryTeam)) {
                return;
            }
        }
        std::uint8_t playerStateGate = 0;
        if (IsValidPointer(playerState)) {
            ReadValue(playerState + 0x4C0, playerStateGate);
        }
        if (playerStateGate != 0 && targetState == 0) return;

        std::int32_t meshType = 0;
        if (!ReadTrackingMeshType(record, meshType)) return;

        Vec3 position{};
        native::CharacterPositionSource positionSource =
            native::CharacterPositionSource::None;
        if (!ReadCharacterPosition(
                record,
                std::string_view{},
                native::PositionReadMode::Direct,
                antiFlicker,
                position,
                &positionSource)) {
            return;
        }
        const bool playerClass = std::find(
            kTrackingPlayerClassOffsets.begin(),
            kTrackingPlayerClassOffsets.end(),
            classOffset) != kTrackingPlayerClassOffsets.end();
        if (!playerClass) return;
        const Vec3 delta = Subtract(position, context.localPosition);
        const float distanceMeters = Length(delta) * 0.01f;
        if (!std::isfinite(distanceMeters) || distanceMeters < 0.0f) {
            return;
        }
        if (!rangeTargetClass &&
            !PassTrackingCategory(record.actor, settings)) {
            return;
        }
        if (!PassTrackingAcquisitionHealth(
                record.actor, settings.ignoreDowned)) {
            return;
        }

        const std::uint64_t identity =
            native::ResolvePlayerIdentity(record.actor, playerState);
        const int aimMode = context.zooming
            ? tuning.adsBone
            : tuning.hipBone;
        const int preferredBone = settings.persistentLock &&
                identity == lockedTrackingIdentity_
            ? lockedTrackingBone_
            : -1;
        BoneFrame boneFrame{};
        const bool boneFrameReady = ReadBoneFrame(
            record,
            context.preparedProjection,
            antiFlicker,
            native::ShouldAlignBoneFrameToCharacterPosition(positionSource)
                ? &position
                : nullptr,
            boneFrame);
        if (boneFrameReady &&
            (settings.missMode || settings.requireVisibility)) {
            static_cast<void>(EvaluateBoneVisibility(
                context.view.location, boneFrame));
        }
        Vec3 targetPoint{};
        Vec2 targetScreen{};
        int selectedBone = -1;
        bool aimPointReady = boneFrameReady && SelectAimPoint(
            boneFrame,
            aimMode,
            settings,
            context.view.location,
            identity,
            sequence,
            preferredBone,
            targetPoint,
            targetScreen,
            selectedBone);
        if (!aimPointReady && rangeTargetClass) {
            aimPointReady = SelectRangeTargetAimPoint(
                position,
                aimMode,
                settings,
                context.view.location,
                context.preparedProjection,
                targetPoint,
                targetScreen);
        }
        if (!aimPointReady) return;

        Vec3 velocity{};
        const std::uintptr_t movement = ReadPointer(record.actor + 0x3D8);
        if (IsValidPointer(movement)) {
            ReadValue(movement + 0x2B0, velocity);
        }
        if (!IsFinite(velocity)) velocity = Vec3{};

        const float centerX =
            static_cast<float>(options_.screenWidth) * 0.5f;
        const float centerY =
            static_cast<float>(options_.screenHeight) * 0.5f;
        const float selectionDistance = std::hypot(
            targetScreen.x - centerX, targetScreen.y - centerY);
        const float distanceLimit = context.zooming
            ? tuning.adsDistanceMeters
            : tuning.hipDistanceMeters;
        if (!std::isfinite(selectionDistance) ||
            (settings.enforceFov &&
             selectionDistance > tuning.rangePixels) ||
            (settings.enforceDistance &&
             distanceMeters > distanceLimit) ||
            !seenActors.insert(record.actor).second) {
            return;
        }
        candidates.push_back(AimCandidate{
            identity,
            record.actor,
            record.root,
            record.mesh,
            targetPoint,
            velocity,
            targetScreen,
            selectionDistance,
            distanceMeters,
            selectionDistance,
            selectedBone,
            record.encryptedRecord,
            false,
            rangeTargetClass,
            false,
            true,
        });
    }

    static std::optional<std::int32_t> TruncateTrackingCounter(float value) {
        if (!std::isfinite(value) ||
            value < static_cast<float>(std::numeric_limits<std::int32_t>::min()) ||
            value > static_cast<float>(std::numeric_limits<std::int32_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int32_t>(value);
    }

    bool PassTrackingHitPercentage(const ui::AimSettings& settings,
                                   const FrameContext& context) {
        const int percentage = std::clamp(settings.hitPercentage, 0, 100);
        if (percentage <= 0) return false;
        if (percentage >= 100) return true;
        std::uintptr_t first = 0;
        ReadValue(context.localPawn + 0x1030, first);
        std::uintptr_t second = 0;
        if (first <=
            std::numeric_limits<std::uintptr_t>::max() - 0x4D8) {
            ReadValue(first + 0x4D8, second);
        }
        std::uintptr_t state = 0;
        if (second <=
            std::numeric_limits<std::uintptr_t>::max() - 0x1220) {
            ReadValue(second + 0x1220, state);
        }
        float totalValue = 0.0f;
        float currentValue = 0.0f;
        if (state <=
            std::numeric_limits<std::uintptr_t>::max() - 0x58) {
            ReadValue(state + 0x58, totalValue);
            ReadValue(state + 0x48, currentValue);
        }
        const std::optional<std::int32_t> total =
            TruncateTrackingCounter(totalValue);
        const std::optional<std::int32_t> current =
            TruncateTrackingCounter(currentValue);
        if (!total.has_value() || !current.has_value()) return false;
        const std::int32_t selected =
            aim::HitSelectionCache::SelectedCountForPercentage(
                percentage, *total);
        const aim::HitSelectionDecision decision =
            trackingHitSelection_.Evaluate(
                selected,
                *current,
                *total,
                [](std::uint32_t seed) {
                    std::srand(seed);
                    return static_cast<std::uint32_t>(std::rand());
                });
        return decision.accepted;
    }

    bool ResolveTrackingOrigin(
        const FrameContext& context,
        Vec3& origin) {
        origin = Vec3{};
        std::uintptr_t state = ReadPointer(context.localPawn + 0x1030);
        if (!IsValidPointer(state)) return false;
        state = ReadPointer(state + 0x4D8);
        if (!IsValidPointer(state)) return false;
        state = ReadPointer(state + 0x180);
        return IsValidPointer(state) &&
            ReadValue(state + 0x220, origin) && IsFinite(origin);
    }
#endif

#if LENGJING_ENABLE_PROJECTILE_TRACKING
    void PublishAimFrame(const ui::AimSettings& settings,
                         const ui::AimTuning& tuning,
                         const FrameContext& context,
                         const std::vector<AimCandidate>& candidates,
                         GameFrame& frame) {
        const aim::AimModeActivation aimModes =
            aim::ResolveAimModeActivation(
                aimEnabled_.load(std::memory_order_acquire),
                WeaponAllowsAim(context.weaponId),
                settings.enabled,
                settings.trajectoryTracking);
        const bool selfAimEnabled = aimModes.selfAim;
        const bool trackingEnabled = aimModes.tracking;
        bool trajectoryHookReady = false;
        if (trackingEnabled && memory_ != nullptr) {
            trajectoryHookReady = trajectoryHook_.EnsureInstalled(
                *memory_, processId_, moduleBase_);
        } else {
            static_cast<void>(trajectoryHook_.Disable());
        }
        if (!selfAimEnabled && !trackingEnabled) {
            aimController_.ClearTarget();
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
            lockedTrackingIdentity_ = 0;
            lockedTrackingBone_ = -1;
            return;
        }
        if (!selfAimEnabled) {
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
        }
        if (!trackingEnabled) {
            lockedTrackingIdentity_ = 0;
            lockedTrackingBone_ = -1;
        }
        const bool touchInput =
            options_.inputMode == ui::AimInputMode::WriteTouch ||
            options_.inputMode == ui::AimInputMode::KernelTouch;
        if (selfAimEnabled && settings.showTouchArea && touchInput) {
            frame.touchRegion = TouchRegionVisual{
                ImVec2(settings.touchX, settings.touchY),
                settings.touchRange,
            };
        }

        const bool triggered = AimTriggered(settings, context);
        const auto betterCandidate = [&settings](
            const AimCandidate& left,
            const AimCandidate& right) {
            return settings.targetAlgorithm == 0
                ? left.screenDistancePixels < right.screenDistancePixels
                : left.worldDistanceMeters < right.worldDistanceMeters;
        };
        const auto selectCandidate = [this,
                                      &candidates,
                                      &settings,
                                      &betterCandidate,
                                      triggered](
            bool trackingCandidate,
            std::uint64_t lockedIdentity) -> const AimCandidate* {
            const bool lockedTargetRequested = triggered &&
                settings.persistentLock && lockedIdentity != 0;
            const AimCandidate* selectedCandidate = nullptr;
            for (const AimCandidate& candidate : candidates) {
                if (lockedTargetRequested &&
                    candidate.identity != lockedIdentity) {
                    continue;
                }
                if (trackingCandidate) {
                    if (!candidate.trackingEligible) continue;
                } else if (!candidate.selfAimEligible) {
                    continue;
                }
                if (selectedCandidate == nullptr ||
                    betterCandidate(candidate, *selectedCandidate)) {
                    selectedCandidate = &candidate;
                }
            }
            return selectedCandidate;
        };
        const AimCandidate* selfAimTarget = selfAimEnabled
            ? selectCandidate(false, lockedAimIdentity_)
            : nullptr;
        const AimCandidate* trackingTarget = trackingEnabled
            ? selectCandidate(true, lockedTrackingIdentity_)
            : nullptr;
        const aim::AimOutputAvailability outputs =
            aim::ResolveAimOutputAvailability(
                aimModes,
                selfAimTarget != nullptr,
                trackingTarget != nullptr);
        const AimCandidate* selected = selfAimTarget != nullptr
            ? selfAimTarget
            : trackingTarget;

        AimGuide guide{};
        guide.center = ImVec2(
            static_cast<float>(options_.screenWidth) * 0.5f,
            static_cast<float>(options_.screenHeight) * 0.5f);
        guide.radius = tuning.rangePixels;
        guide.drawCircle = settings.drawRange;
        guide.drawTargetRay = aim::ShouldDrawAimTargetRay(
            settings.drawTargetRay,
            selected != nullptr,
            selected != nullptr
                ? selected->screenDistancePixels
                : 0.0f,
            tuning.rangePixels);
        if (selected != nullptr) {
            guide.target = ImVec2(selected->screen.x, selected->screen.y);
            guide.targetValid = true;
            guide.selectedBone = selected->boneIndex;
            if (selected->boneIndex >= 0) {
                for (PlayerVisual& player : frame.players) {
                    if (player.identity != selected->identity) continue;
                    player.skeleton.selectedJoint = selected->boneIndex;
                    break;
                }
            }
        }

        if (!triggered || !outputs.Any()) {
            static_cast<void>(trajectoryHook_.Disable());
            aimController_.ClearTarget();
            if (!triggered || !settings.persistentLock) {
                lockedAimIdentity_ = 0;
                lockedAimBone_ = -1;
                lockedTrackingIdentity_ = 0;
                lockedTrackingBone_ = -1;
            }
            if (guide.drawCircle || guide.drawTargetRay) frame.aimGuide = guide;
            return;
        }

        if (settings.persistentLock) {
            if (selfAimTarget != nullptr) {
                lockedAimIdentity_ = selfAimTarget->identity;
                lockedAimBone_ = selfAimTarget->boneIndex;
            }
            if (trackingTarget != nullptr) {
                lockedTrackingIdentity_ = trackingTarget->identity;
                lockedTrackingBone_ = trackingTarget->boneIndex;
            }
            guide.locked = true;
        } else {
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
            lockedTrackingIdentity_ = 0;
            lockedTrackingBone_ = -1;
        }

        aim::TargetSnapshot snapshot{};
        snapshot.valid = true;
        snapshot.identity = selected->identity;
        snapshot.world = aim::Vec3{selected->world.x, selected->world.y, selected->world.z};
        snapshot.velocity = aim::Vec3{
            selected->velocity.x, selected->velocity.y, selected->velocity.z};
        snapshot.screenDistancePixels = selected->screenDistancePixels;
        snapshot.worldDistanceMeters = selected->worldDistanceMeters;
        snapshot.projectileSpeedCmPerSecond = ResolveProjectileSpeed(context);
        snapshot.firing = context.firing;
        snapshot.zooming = context.zooming;
        snapshot.curvedMotion = settings.curvedMotion;
        snapshot.enforceFov = settings.enforceFov;
        snapshot.enforceDistance = settings.enforceDistance;
        snapshot.triggerMode = settings.triggerMode;
        snapshot.orientation = options_.orientation;
        snapshot.touchRange = settings.touchRange;
        snapshot.touchCenterX = settings.touchX;
        snapshot.touchCenterY = settings.touchY;
        snapshot.tuning = tuning;
        snapshot.view.location = aim::Vec3{
            context.view.location.x,
            context.view.location.y,
            context.view.location.z};
        snapshot.view.pitch = context.view.rotation.pitch;
        snapshot.view.yaw = context.view.rotation.yaw;
        snapshot.view.roll = context.view.rotation.roll;
        snapshot.view.fieldOfView = context.view.fieldOfView;
        snapshot.view.halfWidth = static_cast<float>(options_.screenWidth) * 0.5f;
        snapshot.view.halfHeight = static_cast<float>(options_.screenHeight) * 0.5f;

        const auto applyCandidate = [](aim::TargetSnapshot& targetSnapshot,
                                       const AimCandidate& candidate) {
            targetSnapshot.identity = candidate.identity;
            targetSnapshot.world = aim::Vec3{
                candidate.world.x,
                candidate.world.y,
                candidate.world.z,
            };
            targetSnapshot.velocity = aim::Vec3{
                candidate.velocity.x,
                candidate.velocity.y,
                candidate.velocity.z,
            };
            targetSnapshot.screenDistancePixels =
                candidate.screenDistancePixels;
            targetSnapshot.worldDistanceMeters =
                candidate.worldDistanceMeters;
        };

        if (outputs.tracking) {
            const bool trackingAllowed =
                PassTrackingHitPercentage(settings, context);
            if (trackingAllowed && trajectoryHookReady &&
                context.firingOriginValid) {
                aim::TargetSnapshot trackingSnapshot = snapshot;
                applyCandidate(trackingSnapshot, *trackingTarget);
                const aim::Vec3 predicted =
                    aim::PredictInterceptPoint(trackingSnapshot);
                const aim::TrackingCommand command =
                    aim::TrackingCalculator::CalculateAngles(
                        true,
                        aim::TrackingPoint{
                            context.firingOrigin.x,
                            context.firingOrigin.y,
                            context.firingOrigin.z,
                        },
                        aim::TrackingPoint{
                            predicted.x,
                            predicted.y,
                            predicted.z,
                        });
                if (command.flag == 0 ||
                    !trajectoryHook_.Publish(command)) {
                    static_cast<void>(trajectoryHook_.Disable());
                }
            } else {
                static_cast<void>(trajectoryHook_.Disable());
            }
        } else if (trackingEnabled) {
            static_cast<void>(trajectoryHook_.Disable());
        }

        if (outputs.selfAim) {
            aim::TargetSnapshot selfAimSnapshot = snapshot;
            applyCandidate(selfAimSnapshot, *selfAimTarget);
            aimController_.Publish(selfAimSnapshot);
        } else {
            aimController_.ClearTarget();
        }
        if (guide.drawCircle || guide.drawTargetRay) frame.aimGuide = guide;
    }
#else
    void PublishAimFrame(const ui::AimSettings& settings,
                         const ui::AimTuning& tuning,
                         const FrameContext& context,
                         const std::vector<AimCandidate>& candidates,
                         GameFrame& frame) {
        const aim::AimModeActivation aimModes =
            aim::ResolveAimModeActivation(
                aimEnabled_.load(std::memory_order_acquire),
                WeaponAllowsAim(context.weaponId),
                settings.enabled,
                false);
        if (!aimModes.selfAim) {
            aimController_.ClearTarget();
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
            return;
        }

        const bool touchInput =
            options_.inputMode == ui::AimInputMode::WriteTouch ||
            options_.inputMode == ui::AimInputMode::KernelTouch;
        if (settings.showTouchArea && touchInput) {
            frame.touchRegion = TouchRegionVisual{
                ImVec2(settings.touchX, settings.touchY),
                settings.touchRange,
            };
        }

        const bool triggered = AimTriggered(settings, context);
        const bool lockedTargetRequested = triggered &&
            settings.persistentLock && lockedAimIdentity_ != 0;
        const AimCandidate* selected = nullptr;
        for (const AimCandidate& candidate : candidates) {
            if (!candidate.selfAimEligible) continue;
            if (lockedTargetRequested &&
                candidate.identity != lockedAimIdentity_) {
                continue;
            }
            const bool better = selected == nullptr ||
                (settings.targetAlgorithm == 0
                    ? candidate.screenDistancePixels <
                        selected->screenDistancePixels
                    : candidate.worldDistanceMeters <
                        selected->worldDistanceMeters);
            if (better) selected = &candidate;
        }

        AimGuide guide{};
        guide.center = ImVec2(
            static_cast<float>(options_.screenWidth) * 0.5f,
            static_cast<float>(options_.screenHeight) * 0.5f);
        guide.radius = tuning.rangePixels;
        guide.drawCircle = settings.drawRange;
        guide.drawTargetRay = aim::ShouldDrawAimTargetRay(
            settings.drawTargetRay,
            selected != nullptr,
            selected != nullptr
                ? selected->screenDistancePixels
                : 0.0f,
            tuning.rangePixels);
        if (selected != nullptr) {
            guide.target = ImVec2(selected->screen.x, selected->screen.y);
            guide.targetValid = true;
            guide.selectedBone = selected->boneIndex;
            if (selected->boneIndex >= 0) {
                for (PlayerVisual& player : frame.players) {
                    if (player.identity != selected->identity) continue;
                    player.skeleton.selectedJoint = selected->boneIndex;
                    break;
                }
            }
        }

        if (!triggered || selected == nullptr) {
            aimController_.ClearTarget();
            if (!triggered || !settings.persistentLock) {
                lockedAimIdentity_ = 0;
                lockedAimBone_ = -1;
            }
            if (guide.drawCircle || guide.drawTargetRay) {
                frame.aimGuide = guide;
            }
            return;
        }

        if (settings.persistentLock) {
            lockedAimIdentity_ = selected->identity;
            lockedAimBone_ = selected->boneIndex;
            guide.locked = true;
        } else {
            lockedAimIdentity_ = 0;
            lockedAimBone_ = -1;
        }

        aim::TargetSnapshot snapshot{};
        snapshot.valid = true;
        snapshot.identity = selected->identity;
        snapshot.world = aim::Vec3{
            selected->world.x, selected->world.y, selected->world.z};
        snapshot.velocity = aim::Vec3{
            selected->velocity.x,
            selected->velocity.y,
            selected->velocity.z};
        snapshot.screenDistancePixels = selected->screenDistancePixels;
        snapshot.worldDistanceMeters = selected->worldDistanceMeters;
        snapshot.projectileSpeedCmPerSecond = ResolveProjectileSpeed(context);
        snapshot.firing = context.firing;
        snapshot.zooming = context.zooming;
        snapshot.curvedMotion = settings.curvedMotion;
        snapshot.enforceFov = settings.enforceFov;
        snapshot.enforceDistance = settings.enforceDistance;
        snapshot.triggerMode = settings.triggerMode;
        snapshot.orientation = options_.orientation;
        snapshot.touchRange = settings.touchRange;
        snapshot.touchCenterX = settings.touchX;
        snapshot.touchCenterY = settings.touchY;
        snapshot.tuning = tuning;
        snapshot.view.location = aim::Vec3{
            context.view.location.x,
            context.view.location.y,
            context.view.location.z};
        snapshot.view.pitch = context.view.rotation.pitch;
        snapshot.view.yaw = context.view.rotation.yaw;
        snapshot.view.roll = context.view.rotation.roll;
        snapshot.view.fieldOfView = context.view.fieldOfView;
        snapshot.view.halfWidth =
            static_cast<float>(options_.screenWidth) * 0.5f;
        snapshot.view.halfHeight =
            static_cast<float>(options_.screenHeight) * 0.5f;
        aimController_.Publish(snapshot);
        if (guide.drawCircle || guide.drawTargetRay) frame.aimGuide = guide;
    }
#endif

    void AddRadarBlip(const FrameContext& context,
                      const Vec3& position,
                      bool bot,
                      std::string_view className,
                      float headingRadians,
                      bool headingValid,
                      SemanticTone tone,
                      float forwardX,
                      float forwardY,
                      float rightX,
                      float rightY,
                      RadarVisual& radar) {
        const Vec3 delta = Subtract(position, context.localPosition);
        const float rangeCentimeters = std::max(1.0f, radar.maxDistanceMeters) * 100.0f;
        RadarBlip blip{};
        blip.normalizedPosition = ImVec2(
            (delta.x * rightX + delta.y * rightY) / rangeCentimeters,
            -(delta.x * forwardX + delta.y * forwardY) / rangeCentimeters);
        blip.headingRadians = headingRadians;
        blip.headingValid = headingValid;
        blip.kind = bot ? RadarBlipKind::Bot : RadarBlipKind::Player;
        blip.tone = tone;
        if (className.find("Boss") != std::string_view::npos &&
            tone != SemanticTone::Accent &&
            tone != SemanticTone::Muted) {
            blip.tone = SemanticTone::Danger;
        }
        radar.blips.push_back(std::move(blip));
    }

    float ResolveProjectileSpeed(const FrameContext& context) {
        if (!IsValidPointer(context.weaponRoot)) {
            projectileSpeedReader_.Invalidate();
            return ProjectileSpeedFor(context.weaponId);
        }
        auto readBytes = [this](std::uintptr_t address,
                                void* destination,
                                std::size_t size) {
            return memory_ != nullptr && IsValidReadAddress(address) &&
                size <= kMaximumRemoteAddress - address &&
                memory_->Read(address, destination, size);
        };
        auto resolveName = [this](std::uint32_t index) {
            return DecodeName(static_cast<std::int32_t>(index),
                              moduleBase_ + layout_.namePoolOffset);
        };
        const std::optional<float> speed = projectileSpeedReader_.Read(
            context.weaponRoot, readBytes, resolveName);
        return speed.value_or(ProjectileSpeedFor(context.weaponId));
    }

    void PruneThreatState(
        const std::unordered_set<std::uintptr_t>& seenThreats) {
        const auto prune = [&seenThreats](auto& states) {
            for (auto iterator = states.begin(); iterator != states.end();) {
                if (seenThreats.find(iterator->first) == seenThreats.end()) {
                    iterator = states.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        };
        prune(projectileTrails_);
        prune(threatFirstSeen_);
    }

    void PruneBoneCache(std::chrono::steady_clock::time_point now) {
        constexpr auto kRetention = std::chrono::seconds(1);
        for (auto iterator = boneCache_.begin(); iterator != boneCache_.end();) {
            if (now - iterator->second.lastUpdatedAt > kRetention) {
                iterator = boneCache_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void PrunePositionCache(std::chrono::steady_clock::time_point now) {
        constexpr auto kRetention = std::chrono::milliseconds(300);
        const auto prune = [now](auto& cache, auto retention) {
            for (auto iterator = cache.begin(); iterator != cache.end();) {
                if (now - iterator->second.updatedAt > retention) {
                    iterator = cache.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        };
        prune(positionCache_, kRetention);
        for (auto iterator = decodedPositionCache_.begin();
             iterator != decodedPositionCache_.end();) {
            const auto observedAt = iterator->second.observedAt;
            if (observedAt.time_since_epoch().count() == 0 ||
                now < observedAt || now - observedAt >
                    native::kDecodedPositionRetention) {
                iterator = decodedPositionCache_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        for (auto iterator = decodedPositionPending_.begin();
             iterator != decodedPositionPending_.end();) {
            const auto& sample = iterator->second.sample;
            if (sample.count == 0 || sample.lastAt.time_since_epoch().count() == 0 ||
                now < sample.lastAt || now - sample.lastAt >
                    native::kAlgorithmPositionPendingMaximumAge) {
                iterator = decodedPositionPending_.erase(iterator);
            } else {
                ++iterator;
            }
        }
    }

    void ResetWorldState() {
        world_ = 0;
        positionCache_.clear();
        decodedPositionCache_.clear();
        decodedPositionPending_.clear();
        boneCache_.clear();
        nameCache_.clear();
        classTraitsCache_.clear();
        itemMetadata_.clear();
        worldObjectFrameCache_.Clear();
        worldObjectActors_.clear();
        worldObjectDiscoveryPolicy_.Invalidate();
        worldObjectContentPolicy_.Invalidate();
        projectileTrails_.clear();
        threatFirstSeen_.clear();
        aimWarningStates_.clear();
        hudMapCache_.Clear();
        characterPositions_.Clear();
        algorithmPositionRuntime_.Invalidate();
        algorithmReplayPagePolicy_.Invalidate();
        algorithmExecutionContextRefreshPolicy_.Invalidate();
        positionReadMode_ = native::PositionReadMode::Standard;
        projectileSpeedReader_.Invalidate();
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        trackingHitSelection_.Reset();
#endif
        lockedAimIdentity_ = 0;
        lockedAimBone_ = -1;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        lockedTrackingIdentity_ = 0;
        lockedTrackingBone_ = -1;
#endif
        aimController_.ClearTarget();
        lastViewValid_ = false;
        lastView_ = CameraView{};
        lastViewWorld_ = 0;
        lastViewCameraManager_ = 0;
        viewFovState_ = native::ProjectionFovStabilityState{};
    }

    native::AlgorithmExecutionContextRefreshKey
    CurrentAlgorithmExecutionContextRefreshKey(
        bool coordinatePoolSelected) const noexcept {
        const native::CoordinatePoolRuntimeProbe poolProbe =
            coordinatePoolSelected
            ? ActiveCoordinatePoolProbe()
            : native::CoordinatePoolRuntimeProbe{};
        return native::AlgorithmExecutionContextRefreshKey{
            static_cast<std::int32_t>(processId_),
            moduleBase_,
            coordinatePoolSelected ? poolProbe.guestEntry : algorithmGuestPc_,
            coordinatePoolSelected ? 0U : algorithmEntryInstruction_,
            coordinatePoolSelected,
        };
    }

    void RefreshAlgorithmExecutionContext() {
        const bool coordinatePoolSelected =
            UsesAnyCoordinatePoolRuntime();
        if (!coordinatePoolSelected || memory_ == nullptr ||
            (!coordinatePoolSelected && !algorithmEntryReady_)) {
            if (!coordinatePoolSelected ||
                (!coordinatePoolSelected && !algorithmEntryReady_)) {
                algorithmExecutionContext_ = {};
                algorithmExecutionContextReady_ = false;
                algorithmExecutionContextRefreshPolicy_.Invalidate();
            }
            coordinatePoolReady_ = false;
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const native::AlgorithmExecutionContextRefreshKey refreshKey =
            CurrentAlgorithmExecutionContextRefreshKey(
                coordinatePoolSelected);
        bool executionContextRefreshed = false;
        if (!algorithmExecutionContextReady_ ||
            algorithmExecutionContextRefreshPolicy_.ShouldRefresh(
                refreshKey, now)) {
            executionContextRefreshed = true;
            native::ProcessExecutionContext refreshed{};
            if (!memory_->ReadProcessExecutionContext(refreshed)) {
                if (algorithmExecutionContextReady_) {
                    algorithmPositionRuntime_.Invalidate();
                    algorithmReplayPagePolicy_.Invalidate();
                }
                algorithmExecutionContext_ = {};
                algorithmExecutionContextReady_ = false;
                algorithmExecutionContextRefreshPolicy_.MarkFailed();
            } else {
                if (!algorithmExecutionContextReady_ ||
                    algorithmExecutionContext_.generation !=
                        refreshed.generation ||
                    algorithmExecutionContext_.threadId !=
                        refreshed.threadId ||
                    algorithmExecutionContext_.threadStartTimeTicks !=
                        refreshed.threadStartTimeTicks) {
                    algorithmPositionRuntime_.Invalidate();
                    algorithmReplayPagePolicy_.Invalidate();
                }
                algorithmExecutionContext_ = refreshed;
                algorithmExecutionContextReady_ = refreshed.IsUsable();
                algorithmExecutionContextRefreshPolicy_.MarkSucceeded(
                    CurrentAlgorithmExecutionContextRefreshKey(
                        coordinatePoolSelected),
                    now);
            }
        }
        if (coordinatePoolSelected && algorithmExecutionContextReady_) {
            coordinatePoolFrame_ =
                coordinatePoolFrame_ ==
                        std::numeric_limits<std::uint64_t>::max()
                    ? 1
                    : coordinatePoolFrame_ + 1;
            coordinatePoolReady_ = UsesCoordinateDecrypt2Runtime()
                ? coordinateDecrypt2Runtime_.Refresh(
                      *memory_,
                      processId_,
                      moduleBase_,
                      algorithmExecutionContext_,
                      coordinatePoolFrame_)
                : coordinatePoolRuntime_.Refresh(
                      *memory_,
                      processId_,
                      moduleBase_,
                      algorithmExecutionContext_,
                      coordinatePoolFrame_);
            const native::CoordinatePoolRuntimeProbe poolProbe =
                ActiveCoordinatePoolProbe();
            const bool hasIdentity = poolProbe.bridge != 0 &&
                poolProbe.context != 0 && poolProbe.guestEntry != 0;
            const bool identityChanged = hasIdentity &&
                coordinatePoolEntry_ != 0 &&
                (coordinatePoolBridge_ != poolProbe.bridge ||
                 coordinatePoolContext_ != poolProbe.context ||
                 coordinatePoolEntry_ != poolProbe.guestEntry);
            if (identityChanged) {
                decodedPositionCache_.clear();
                decodedPositionPending_.clear();
                characterPositions_.Clear();
            }
            if (hasIdentity) {
                coordinatePoolBridge_ = poolProbe.bridge;
                coordinatePoolContext_ = poolProbe.context;
                coordinatePoolEntry_ = poolProbe.guestEntry;
            }
        } else {
            coordinatePoolReady_ = false;
        }
        if (IsCoordinateTraceEnabled() &&
            (executionContextRefreshed ||
             ShouldWriteCoordinateFrameTrace(coordinateTraceFrame_))) {
            const native::ProcessExecutionContextDiagnostic diagnostic =
                memory_->ExecutionContextDiagnostic();
            const native::CoordinatePoolRuntimeProbe poolProbe =
                ActiveCoordinatePoolProbe();
            if (!UsesCoordinateDecrypt2Runtime()) {
                std::fprintf(
                    stderr,
                    "[coordinate-context-trace] frame=%llu refreshed=%d "
                    "ready=%d source=%u cd=%u sys=%d device_sys=%d "
                    "ptrace_sys=%d device_requests=%zu operands=%d "
                    "tid=%d thread_start=%llu generation=%llu tpidr=%llx "
                    "key_available=%d oracle_available=%d pool_ready=%d "
                    "pool_stage=%u pool_error=%u pool_sys=%d "
                    "analysis_find=%u analysis_detail=%u "
                    "analysis_madds=%u analysis_ring_madds=%u "
                    "analysis_candidates=%u analysis_insn=%u "
                    "decode_limit=%u analysis_mode=%u analysis_passes=%u "
                    "primary_error=%u primary_find=%u primary_detail=%u "
                    "method_load=%u failed_method=%llx "
                    "read_stage=%u read_failure=%u read_path=%u "
                    "bridge=%llx context=%llx raw_entry=%llx "
                    "resolved_entry=%llx branch_status=%u branch_hops=%u "
                    "branch_insn=%08x branch_terminal=%08x pool=%llx "
                    "attempts=%llu successes=%llu\n",
                    static_cast<unsigned long long>(coordinateTraceFrame_),
                    executionContextRefreshed ? 1 : 0,
                    algorithmExecutionContextReady_ ? 1 : 0,
                    static_cast<unsigned int>(diagnostic.source),
                    static_cast<unsigned int>(diagnostic.error),
                    diagnostic.systemError,
                    diagnostic.deviceStatus,
                    diagnostic.ptraceStatus,
                    diagnostic.deviceRequestCount,
                    diagnostic.pacgaOperandsResolved ? 1 : 0,
                    algorithmExecutionContext_.threadId,
                    static_cast<unsigned long long>(
                        algorithmExecutionContext_.threadStartTimeTicks),
                    static_cast<unsigned long long>(
                        algorithmExecutionContext_.generation),
                    static_cast<unsigned long long>(
                        algorithmExecutionContext_.tpidrEl0),
                    algorithmExecutionContext_.HasPacgaKey() ? 1 : 0,
                    algorithmExecutionContext_.pacgaOracle.available ? 1 : 0,
                    coordinatePoolReady_ ? 1 : 0,
                    static_cast<unsigned int>(poolProbe.stage),
                    static_cast<unsigned int>(poolProbe.error),
                    poolProbe.systemError,
                    static_cast<unsigned int>(poolProbe.analysisFindStage),
                    static_cast<unsigned int>(poolProbe.analysisFindDetail),
                    static_cast<unsigned int>(poolProbe.analysisMaddCount),
                    static_cast<unsigned int>(
                        poolProbe.analysisRingMaddCount),
                    static_cast<unsigned int>(
                        poolProbe.analysisCandidateCount),
                    static_cast<unsigned int>(
                        poolProbe.analysisFailureInstruction),
                    static_cast<unsigned int>(
                        poolProbe.analysisDecodeInstructionLimit),
                    static_cast<unsigned int>(poolProbe.analysisMode),
                    static_cast<unsigned int>(poolProbe.analysisPasses),
                    static_cast<unsigned int>(
                        poolProbe.primaryAnalysisError),
                    static_cast<unsigned int>(
                        poolProbe.primaryAnalysisFindStage),
                    static_cast<unsigned int>(
                        poolProbe.primaryAnalysisFindDetail),
                    static_cast<unsigned int>(
                        poolProbe.analysisMethodLoadResult),
                    static_cast<unsigned long long>(poolProbe.failedMethod),
                    static_cast<unsigned int>(poolProbe.read.stage),
                    static_cast<unsigned int>(poolProbe.read.failure),
                    static_cast<unsigned int>(poolProbe.read.lastPath),
                    static_cast<unsigned long long>(poolProbe.bridge),
                    static_cast<unsigned long long>(poolProbe.context),
                    static_cast<unsigned long long>(poolProbe.guestEntry),
                    static_cast<unsigned long long>(poolProbe.resolvedEntry),
                    static_cast<unsigned int>(poolProbe.entryBranchStatus),
                    static_cast<unsigned int>(poolProbe.entryBranchHops),
                    static_cast<unsigned int>(poolProbe.entryInstruction),
                    static_cast<unsigned int>(
                        poolProbe.entryTerminalInstruction),
                    static_cast<unsigned long long>(
                        poolProbe.poolPointer.normalizedValue),
                    static_cast<unsigned long long>(poolProbe.attempts),
                    static_cast<unsigned long long>(poolProbe.successes));
                std::fflush(stderr);
                return;
            }
            std::fprintf(
                stderr,
                "[coordinate-context-trace] frame=%llu refreshed=%d "
                "ready=%d source=%u cd=%u sys=%d device_sys=%d "
                "ptrace_sys=%d device_requests=%zu operands=%d "
                "tid=%d thread_start=%llu generation=%llu tpidr=%llx "
                "key_available=%d oracle_available=%d pool_ready=%d "
                "pool_stage=%u pool_error=%u pool_sys=%d "
                "analysis_find=%u analysis_detail=%u "
                "analysis_madds=%u analysis_ring_madds=%u "
                "analysis_candidates=%u analysis_insn=%u decode_limit=%u "
                "read_stage=%u read_failure=%u read_path=%u "
                "bridge=%llx context=%llx raw_entry=%llx "
                "resolved_entry=%llx branch_status=%u branch_hops=%u "
                "branch_insn=%08x branch_terminal=%08x pool=%llx "
                "attempts=%llu successes=%llu\n",
                static_cast<unsigned long long>(coordinateTraceFrame_),
                executionContextRefreshed ? 1 : 0,
                algorithmExecutionContextReady_ ? 1 : 0,
                static_cast<unsigned int>(diagnostic.source),
                static_cast<unsigned int>(diagnostic.error),
                diagnostic.systemError,
                diagnostic.deviceStatus,
                diagnostic.ptraceStatus,
                diagnostic.deviceRequestCount,
                diagnostic.pacgaOperandsResolved ? 1 : 0,
                algorithmExecutionContext_.threadId,
                static_cast<unsigned long long>(
                    algorithmExecutionContext_.threadStartTimeTicks),
                static_cast<unsigned long long>(
                    algorithmExecutionContext_.generation),
                static_cast<unsigned long long>(
                    algorithmExecutionContext_.tpidrEl0),
                algorithmExecutionContext_.HasPacgaKey() ? 1 : 0,
                algorithmExecutionContext_.pacgaOracle.available ? 1 : 0,
                coordinatePoolReady_ ? 1 : 0,
                static_cast<unsigned int>(poolProbe.stage),
                static_cast<unsigned int>(poolProbe.error),
                poolProbe.systemError,
                static_cast<unsigned int>(poolProbe.analysisFindStage),
                static_cast<unsigned int>(poolProbe.analysisFindDetail),
                static_cast<unsigned int>(poolProbe.analysisMaddCount),
                static_cast<unsigned int>(
                    poolProbe.analysisRingMaddCount),
                static_cast<unsigned int>(
                    poolProbe.analysisCandidateCount),
                static_cast<unsigned int>(
                    poolProbe.analysisFailureInstruction),
                static_cast<unsigned int>(
                    poolProbe.analysisDecodeInstructionLimit),
                static_cast<unsigned int>(poolProbe.read.stage),
                static_cast<unsigned int>(poolProbe.read.failure),
                static_cast<unsigned int>(poolProbe.read.lastPath),
                static_cast<unsigned long long>(poolProbe.bridge),
                static_cast<unsigned long long>(poolProbe.context),
                static_cast<unsigned long long>(poolProbe.guestEntry),
                static_cast<unsigned long long>(poolProbe.resolvedEntry),
                static_cast<unsigned int>(poolProbe.entryBranchStatus),
                static_cast<unsigned int>(poolProbe.entryBranchHops),
                static_cast<unsigned int>(poolProbe.entryInstruction),
                static_cast<unsigned int>(
                    poolProbe.entryTerminalInstruction),
                static_cast<unsigned long long>(
                    poolProbe.poolPointer.normalizedValue),
                static_cast<unsigned long long>(poolProbe.attempts),
                static_cast<unsigned long long>(poolProbe.successes));
            std::fflush(stderr);
        }
    }

    void RunForcedCoordinateProbe() {
        const std::uintptr_t component =
            ForcedCoordinateProbeComponent();
        if (component == 0 || memory_ == nullptr ||
            !algorithmEntryReady_ || !algorithmExecutionContextReady_) {
            return;
        }
        native::AlgorithmPosition position{};
        const native::AlgorithmPositionRuntimeResult result =
            algorithmPositionRuntime_.ExecuteAtGuestPcResult(
                *memory_,
                algorithmGuestPc_,
                component,
                algorithmExecutionContext_,
                position,
                false);
        const native::AlgorithmPositionRuntimeProbe replayProbe =
            algorithmPositionRuntime_.Probe();
        const char* state = result ==
                native::AlgorithmPositionRuntimeResult::Pending
            ? "pending"
            : (result == native::AlgorithmPositionRuntimeResult::Ready
                   ? "ready"
                   : "failed");
        std::fprintf(
            stderr,
            "[coordinate-forced-probe] component=%llx state=%s "
            "request=%llu completed=%llu runtime_error=%u fault=%llx "
            "final_pc=%llx tpidr=%llx ctr=%llx cntfrq=%llx "
            "counter_first=%llx counter_last=%llx "
            "mrs_ctr=%llu mrs_tpidr=%llu mrs_cntfrq=%llu "
            "mrs_counter=%llu pac_count=%llu xyz=(%.3f,%.3f,%.3f)\n",
            static_cast<unsigned long long>(component),
            state,
            static_cast<unsigned long long>(replayProbe.requestId),
            static_cast<unsigned long long>(
                replayProbe.completedRequestId),
            static_cast<unsigned int>(replayProbe.error),
            static_cast<unsigned long long>(replayProbe.faultAddress),
            static_cast<unsigned long long>(replayProbe.finalPc),
            static_cast<unsigned long long>(replayProbe.tpidrEl0),
            static_cast<unsigned long long>(replayProbe.ctrEl0),
            static_cast<unsigned long long>(replayProbe.cntfrqEl0),
            static_cast<unsigned long long>(replayProbe.counterFirst),
            static_cast<unsigned long long>(replayProbe.counterLast),
            static_cast<unsigned long long>(replayProbe.ctrReadCount),
            static_cast<unsigned long long>(replayProbe.tpidrReadCount),
            static_cast<unsigned long long>(replayProbe.cntfrqReadCount),
            static_cast<unsigned long long>(replayProbe.counterReadCount),
            static_cast<unsigned long long>(replayProbe.pacgaCount),
            position.x,
            position.y,
            position.z);
        if (result == native::AlgorithmPositionRuntimeResult::Failed &&
            replayProbe.instructionTraceCount != 0 &&
            replayProbe.completedRequestId !=
                algorithmLastForcedTraceRequest_) {
            algorithmLastForcedTraceRequest_ =
                replayProbe.completedRequestId;
            std::fprintf(
                stderr,
                "[coordinate-forced-path] request=%llu count=%zu pcs=",
                static_cast<unsigned long long>(
                    replayProbe.completedRequestId),
                replayProbe.instructionTraceCount);
            for (std::size_t index = 0;
                 index < replayProbe.instructionTraceCount;
                 ++index) {
                std::fprintf(
                    stderr,
                    "%s%llx",
                    index == 0 ? "" : ",",
                    static_cast<unsigned long long>(
                        replayProbe.instructionTrace[index]));
            }
            std::fputc('\n', stderr);
        }
        std::fflush(stderr);
    }

    void RefreshAlgorithmEntry(bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            algorithmEntryValidationAt_.time_since_epoch().count() != 0 &&
            now - algorithmEntryValidationAt_ < std::chrono::seconds(1)) {
            return;
        }
        algorithmEntryValidationAt_ = now;

        native::CoordinateReplayEntrySnapshot snapshot{};
        native::CoordinateReplayEntryDiagnostic diagnostic{};
        const bool candidateReady = memory_ != nullptr &&
            memory_->ResolveCoordinateReplayEntry(
                moduleBase_, snapshot, diagnostic);
        const std::uintptr_t candidatePc = candidateReady
            ? snapshot.entry
            : 0;
        const std::uint32_t candidateInstruction = candidateReady
            ? snapshot.instruction
            : 0;
        const bool changed = candidateReady != algorithmEntryReady_ ||
            candidatePc != algorithmGuestPc_ ||
            (candidateReady &&
             candidateInstruction != algorithmEntryInstruction_);
        if (changed) {
            algorithmPositionRuntime_.Invalidate();
            algorithmReplayPagePolicy_.Invalidate();
            if (!UsesAnyCoordinatePoolRuntime()) {
                algorithmExecutionContextRefreshPolicy_.Invalidate();
                algorithmExecutionContext_ = {};
                algorithmExecutionContextReady_ = false;
            }
        }
        algorithmGuestPc_ = candidatePc;
        algorithmEntryInstruction_ = candidateReady
            ? candidateInstruction
            : 0;
        algorithmEntryReady_ = candidateReady;
        coordinateReplayEntrySnapshot_ = snapshot;
        coordinateReplayEntryDiagnostic_ = diagnostic;
        if (IsCoordinateTraceEnabled()) {
            std::fprintf(
                stderr,
                "[coordinate-entry-trace] frame=%llu ready=%d bridge=%llx "
                "entry=%llx map_start=%llx map_end=%llx instruction=%08x "
                "cd=%u sys=%d read_stage=%u read_failure=%u read_path=%u "
                "read_at=%llx read_n=%zu\n",
                static_cast<unsigned long long>(coordinateTraceFrame_),
                candidateReady ? 1 : 0,
                static_cast<unsigned long long>(snapshot.bridge),
                static_cast<unsigned long long>(snapshot.entry),
                static_cast<unsigned long long>(snapshot.mappingStart),
                static_cast<unsigned long long>(snapshot.mappingEnd),
                static_cast<unsigned int>(snapshot.instruction),
                static_cast<unsigned int>(diagnostic.error),
                diagnostic.systemError,
                static_cast<unsigned int>(diagnostic.read.stage),
                static_cast<unsigned int>(diagnostic.read.failure),
                static_cast<unsigned int>(diagnostic.read.lastPath),
                static_cast<unsigned long long>(diagnostic.read.address),
                diagnostic.read.size);
            std::fflush(stderr);
        }
    }

    void RecordCoordinateFrameFailure(const CoordinateFailure& failure) {
        if (failure.error == CoordinateDecryptError::None) return;
        if (algorithmFrameFailure_.error == CoordinateDecryptError::None ||
            failure.read.HasFailure() ||
            !algorithmFrameFailure_.read.HasFailure()) {
            algorithmFrameFailure_ = failure;
        }
    }

    void FinalizeCoordinateExecutionHealth(
        bool infrastructureProbeFailed) {
        EvaluateAlgorithmExecutionHealth();
        if (infrastructureProbeFailed) return;
        algorithmReplayBackoffPolicy_.ObserveFrame(
            static_cast<std::size_t>(algorithmFrameAttemptCount_),
            static_cast<std::size_t>(algorithmFrameSuccessCount_),
            std::chrono::steady_clock::now());
    }

    void EvaluateAlgorithmExecutionHealth() {
        if (!UsesAnyCoordinatePoolRuntime() || !CoordinateEntryReady() ||
            !algorithmExecutionContextReady_ ||
            algorithmFrameAttemptCount_ == 0) {
            if (!UsesAnyCoordinatePoolRuntime() ||
                !algorithmExecutionContextReady_) {
                algorithmFailureSince_ = {};
            }
            return;
        }
        if (native::IsCoordinateFrameHealthy(
                static_cast<std::size_t>(algorithmFrameAttemptCount_),
                static_cast<std::size_t>(algorithmFrameSuccessCount_),
                algorithmFrameAgedDecodedFailure_)) {
            algorithmFailureSince_ = {};
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (algorithmFailureSince_.time_since_epoch().count() == 0) {
            algorithmFailureSince_ = now;
            return;
        }
        if (!native::HasCoordinateFailureRecoveryElapsed(
                algorithmFailureSince_,
                now,
                algorithmFrameAgedDecodedFailure_) ||
            memory_ == nullptr) {
            return;
        }
        if (!memory_->RejectProcessExecutionContext()) {
            return;
        }
        algorithmFailureSince_ = {};
        algorithmExecutionContext_ = {};
        algorithmExecutionContextReady_ = false;
        algorithmExecutionContextRefreshPolicy_.Invalidate();
        coordinatePoolReady_ = false;
        algorithmEntryValidationAt_ = {};
        algorithmPositionRuntime_.Invalidate();
        algorithmReplayPagePolicy_.Invalidate();
    }

    bool UsesCoordinatePoolRuntime() const noexcept {
        return algorithmPositionRequested_;
    }

    bool UsesCoordinateDecrypt2Runtime() const noexcept {
        return hardwareBreakpointRequested_;
    }

    bool UsesAnyCoordinatePoolRuntime() const noexcept {
        return UsesCoordinatePoolRuntime() ||
            UsesCoordinateDecrypt2Runtime();
    }

    native::CoordinatePoolRuntimeProbe
    ActiveCoordinatePoolProbe() const noexcept {
        if (UsesCoordinateDecrypt2Runtime()) {
            return coordinateDecrypt2Runtime_.Probe();
        }
        return UsesCoordinatePoolRuntime()
            ? coordinatePoolRuntime_.Probe()
            : native::CoordinatePoolRuntimeProbe{};
    }

    bool CoordinateEntryReady() const {
        if (UsesAnyCoordinatePoolRuntime()) {
            const native::CoordinatePoolRuntimeProbe poolProbe =
                ActiveCoordinatePoolProbe();
            return poolProbe.guestEntry != 0 && poolProbe.codeBase != 0 &&
                poolProbe.codeSize != 0;
        }
        return algorithmEntryReady_;
    }

    CoordinateFailure CurrentCoordinateFailure() const {
        if (!UsesAnyCoordinatePoolRuntime()) return {};

        const bool coordinatePoolSelected =
            UsesAnyCoordinatePoolRuntime();
        const native::CoordinatePoolRuntimeProbe poolProbe =
            coordinatePoolSelected
            ? ActiveCoordinatePoolProbe()
            : native::CoordinatePoolRuntimeProbe{};
        const CoordinateDecryptError poolError = coordinatePoolSelected
            ? CoordinatePoolError(poolProbe.error, poolProbe.read)
            : CoordinateDecryptError::None;
        const native::ProcessExecutionContextDiagnostic contextDiagnostic =
            memory_ != nullptr
            ? memory_->ExecutionContextDiagnostic()
            : native::ProcessExecutionContextDiagnostic{};

        if (!CoordinateEntryReady()) {
            if (coordinatePoolSelected) {
                return {
                    poolError != CoordinateDecryptError::None
                        ? poolError
                        : CoordinateDecryptError::EntryResolveFailed,
                    poolProbe.systemError,
                    poolProbe.read,
                };
            }
            return {
                coordinateReplayEntryDiagnostic_.error !=
                        CoordinateDecryptError::None
                    ? coordinateReplayEntryDiagnostic_.error
                    : CoordinateDecryptError::EntryResolveFailed,
                coordinateReplayEntryDiagnostic_.systemError,
                coordinateReplayEntryDiagnostic_.read,
            };
        }
        if (!algorithmExecutionContextReady_) {
            return {
                contextDiagnostic.error != CoordinateDecryptError::None
                    ? contextDiagnostic.error
                    : CoordinateDecryptError::ContextDataInvalid,
                contextDiagnostic.systemError,
            };
        }
        if (native::ShouldReportCoordinateFrameOutputError(
                static_cast<std::size_t>(algorithmFrameAttemptCount_),
                static_cast<std::size_t>(algorithmFrameSuccessCount_),
                algorithmFrameOutputError_ != CoordinateDecryptError::None)) {
            return {algorithmFrameOutputError_, 0};
        }
        if (native::IsCoordinateFrameHealthy(
                static_cast<std::size_t>(algorithmFrameAttemptCount_),
                static_cast<std::size_t>(algorithmFrameSuccessCount_),
                algorithmFrameAgedDecodedFailure_)) {
            return {};
        }
        if (algorithmFrameFailure_.error != CoordinateDecryptError::None) {
            return algorithmFrameFailure_;
        }
        if (coordinatePoolSelected && poolError != CoordinateDecryptError::None) {
            return {poolError, poolProbe.systemError, poolProbe.read};
        }

        if (algorithmFrameAttemptCount_ != 0) {
            if (coordinatePoolSelected) {
                return {CoordinateDecryptError::PositionReadFailed, 0};
            }
            const native::AlgorithmPositionRuntimeProbe replayProbe =
                algorithmPositionRuntime_.Probe();
            const CoordinateDecryptError replayError =
                AlgorithmPositionError(replayProbe.error);
            return {
                replayError != CoordinateDecryptError::None
                    ? replayError
                    : CoordinateDecryptError::ReplayExecutionFailed,
                replayProbe.unicornError != 0
                    ? replayProbe.unicornError
                    : replayProbe.read.systemError,
                replayProbe.read,
            };
        }
        return {};
    }

    void ApplyCoordinateDiagnostic(
        std::string& diagnostic,
        const RuntimeProbe& probe) const {
        if (!probe.coordinateRequested ||
            probe.coordinateError == CoordinateDecryptError::None) {
            return;
        }
        diagnostic = FormatCoordinateDecryptDiagnostic(
            probe.coordinateError,
            probe.coordinateSystemError,
            probe.coordinateRead,
            probe.coordinatePoolPointer,
            probe.coordinateEntry);
    }

    void UpdateCoordinateProbe(RuntimeProbe& probe) const {
        const bool coordinatePoolSelected =
            UsesAnyCoordinatePoolRuntime();
        const native::CoordinatePoolRuntimeProbe poolProbe =
            coordinatePoolSelected
            ? ActiveCoordinatePoolProbe()
            : native::CoordinatePoolRuntimeProbe{};
        {
            const CoordinateFailure failure = CurrentCoordinateFailure();
            probe.coordinateRequested =
                UsesAnyCoordinatePoolRuntime();
            probe.coordinateEntryReady = CoordinateEntryReady();
            probe.coordinateContextReady = algorithmExecutionContextReady_;
            probe.coordinateThreadId = algorithmExecutionContext_.threadId;
            probe.coordinateGuestPc = coordinatePoolSelected
                ? poolProbe.guestEntry
                : algorithmGuestPc_;
            probe.coordinateContextGeneration =
                algorithmExecutionContext_.generation;
            probe.coordinateAttempts = coordinatePoolSelected
                ? poolProbe.attempts
                : algorithmAttemptCount_;
            probe.coordinateSuccesses = coordinatePoolSelected
                ? poolProbe.successes
                : algorithmSuccessCount_;
            probe.coordinateError = failure.error;
            probe.coordinateSystemError = failure.systemError;
            probe.coordinateRead = failure.read;
            probe.coordinatePoolPointer = poolProbe.poolPointer;
            probe.coordinateEntry = coordinatePoolSelected
                ? CoordinateEntryDiagnostic{
                      poolProbe.guestEntry,
                      poolProbe.executableMappingStart,
                      poolProbe.executableMappingEnd,
                      poolProbe.failedMethod,
                      poolProbe.executableMappingFragments,
                  }
                : CoordinateEntryDiagnostic{
                      coordinateReplayEntrySnapshot_.entry,
                      coordinateReplayEntrySnapshot_.mappingStart,
                      coordinateReplayEntrySnapshot_.mappingEnd,
                      0,
                      0,
                  };
        }
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        probe.algorithmCoordinateRequested = algorithmDecryptRequested_;
        probe.algorithmCoordinateActive =
            native::ShouldReadAlgorithmCoordinate(
                algorithmPositionRequested_,
                algorithmDecryptRequested_);
        probe.algorithmCoordinateTableReady =
            algorithmCoordinateTableReady_;
        probe.algorithmCoordinateRuntimeReady =
            algorithmCoordinateRuntimeReady_;
        probe.algorithmCoordinateRefreshes =
            algorithmCoordinateRefreshCount_;
        probe.algorithmCoordinateResolveAttempts =
            algorithmCoordinateResolveAttemptCount_;
        probe.algorithmCoordinateResolveSuccesses =
            algorithmCoordinateResolveSuccessCount_;
        probe.algorithmCoordinateAttempts =
            algorithmCoordinateAttemptCount_;
        probe.algorithmCoordinateSuccesses =
            algorithmCoordinateSuccessCount_;
        probe.algorithmCoordinateObjectAttempts =
            algorithmCoordinateObjectAttemptCount_;
        probe.algorithmCoordinateObjectSuccesses =
            algorithmCoordinateObjectSuccessCount_;
        probe.algorithmCoordinateTableAttempts =
            algorithmCoordinateTableAttemptCount_;
        probe.algorithmCoordinateTableSuccesses =
            algorithmCoordinateTableSuccessCount_;
        probe.algorithmCoordinateFallbacks =
            algorithmCoordinateFallbackCount_;
        probe.algorithmCoordinateSource = algorithmCoordinateFrameSource_;
        if (algorithmCoordinateFrameSource_ ==
            native::AlgorithmCoordinateSource::RecordTable) {
            probe.algorithmCoordinate = algorithmCoordinateFrameSuccess_;
        } else if (algorithmCoordinateFrameFailure_.error !=
                   native::AlgorithmCoordinateReadError::None) {
            probe.algorithmCoordinate = algorithmCoordinateFrameFailure_;
        } else {
            probe.algorithmCoordinate = algorithmCoordinateTableDiagnostic_;
        }
        if (algorithmCoordinateFrameSource_ ==
            native::AlgorithmCoordinateSource::RuntimeObject) {
            probe.algorithmCoordinateRuntime =
                algorithmCoordinateFrameRuntimeSuccess_;
        } else if (algorithmCoordinateFrameRuntimeFailure_.error !=
                   native::RuntimeCoordinateCodecError::None) {
            probe.algorithmCoordinateRuntime =
                algorithmCoordinateFrameRuntimeFailure_;
        } else {
            probe.algorithmCoordinateRuntime =
                algorithmCoordinateRuntimeDiagnostic_;
        }
#endif
    }

    bool CloseLocked() noexcept {
        opened_ = false;
        aimController_.Stop();
        geometryRuntime_.Stop();
        ReleaseGeometryTransport();
        geometrySnapshotReady_ = false;
        geometryRefreshEpoch_ = 0;
        geometryValidationEpoch_ = 0;
        geometryValidationPending_ = false;
        geometryRetryAfter_ = {};
        geometryWorld_ = 0;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
        bool hookStopped = false;
        for (int attempt = 0; attempt < 3 && !hookStopped; ++attempt) {
            hookStopped = trajectoryHook_.Shutdown();
        }
        if (!hookStopped) {
            aimEnabled_.store(false, std::memory_order_release);
            return false;
        }
#endif
#if 0
        if (!StopHardwareBreakpointRuntime()) {
            aimEnabled_.store(false, std::memory_order_release);
            return false;
        }
#endif
        algorithmPositionRuntime_.Reset();
        coordinatePoolRuntime_.Reset();
        coordinateDecrypt2Runtime_.Reset();
        if (memory_ != nullptr) memory_->Close();
        memory_.reset();
        algorithmExecutionContext_ = {};
        algorithmExecutionContextReady_ = false;
        coordinateReplayEntrySnapshot_ = {};
        coordinateReplayEntryDiagnostic_ = {};
        algorithmGuestPc_ = 0;
        algorithmEntryInstruction_ = 0;
        algorithmEntryReady_ = false;
        algorithmEntryValidationAt_ = {};
        algorithmFailureSince_ = {};
        algorithmReplayBackoffPolicy_.Reset();
        algorithmReplayPagePolicy_.Invalidate();
        algorithmExecutionContextRefreshPolicy_.Invalidate();
        algorithmAttemptCount_ = 0;
        algorithmSuccessCount_ = 0;
        algorithmFrameAttemptCount_ = 0;
        algorithmFrameSuccessCount_ = 0;
        algorithmFrameOutputError_ = CoordinateDecryptError::None;
        algorithmFrameFailure_ = {};
        algorithmFrameAgedDecodedFailure_ = false;
        decodedPositionPending_.clear();
        algorithmPositionRequested_ = false;
        coordinateDecrypt2Index_ = 0;
        hardwareBreakpointRequested_ = false;
#if 0
        hardwareBreakpointRetryAfter_ = {};
        hardwareBreakpointFailure_ = {};
#endif
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
        algorithmDecryptRequested_ = false;
        algorithmCoordinateSnapshot_.clear();
        algorithmCoordinateTableReady_ = false;
        algorithmCoordinateTableDiagnostic_ = {};
        algorithmCoordinateFrameFailure_ = {};
        algorithmCoordinateFrameSuccess_ = {};
        runtimeCoordinateCodec_.Reset();
        algorithmCoordinateRuntimeReady_ = false;
        algorithmCoordinateRuntimeDiagnostic_ = {};
        algorithmCoordinateFrameRuntimeFailure_ = {};
        algorithmCoordinateFrameRuntimeSuccess_ = {};
        algorithmCoordinateFrameSource_ =
            native::AlgorithmCoordinateSource::None;
        algorithmCoordinateObjectCache_.clear();
        algorithmCoordinateRefreshCount_ = 0;
        algorithmCoordinateResolveAttemptCount_ = 0;
        algorithmCoordinateResolveSuccessCount_ = 0;
        algorithmCoordinateAttemptCount_ = 0;
        algorithmCoordinateSuccessCount_ = 0;
        algorithmCoordinateObjectAttemptCount_ = 0;
        algorithmCoordinateObjectSuccessCount_ = 0;
        algorithmCoordinateTableAttemptCount_ = 0;
        algorithmCoordinateTableSuccessCount_ = 0;
        algorithmCoordinateFallbackCount_ = 0;
        algorithmCoordinateFrameAttemptCount_ = 0;
        algorithmCoordinateFrameSuccessCount_ = 0;
#endif
        algorithmPositionConfig_ = {};
        coordinatePoolFallback_ = false;
        coordinatePoolReady_ = false;
        coordinatePoolFrame_ = 0;
        coordinatePoolBridge_ = 0;
        coordinatePoolContext_ = 0;
        coordinatePoolEntry_ = 0;
        processId_ = -1;
        moduleBase_ = 0;
        moduleBuildId_.clear();
        layout_ = VersionLayout{};
        options_ = RuntimeOptions{};
        customItemPath_.clear();
        customItems_.Clear();
        InvalidateActorRecordSnapshot();
        ResetWorldState();
        aimEnabled_.store(false, std::memory_order_release);
        return true;
    }

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    struct AlgorithmCoordinateObjectCacheEntry {
        std::uintptr_t owner = 0;
        Vec3 raw{};
        Vec3 position{};
        native::RuntimeCoordinateCodecDiagnostic diagnostic{};
        bool valid = false;
    };
#endif

    std::mutex mutex_;
    RuntimeOptions options_{};
    VersionLayout layout_{};
    std::unique_ptr<native::MemoryTransport> memory_;
    std::unique_ptr<native::MemoryTransport> geometryMemory_;
    std::shared_ptr<GeometryReadRoute> geometryReadRoute_;
    bool geometryTransportDedicated_ = false;
    native::AlgorithmPositionRuntime algorithmPositionRuntime_{};
    native::CoordinatePoolRuntime coordinatePoolRuntime_{};
    native::CoordinateDecrypt2Runtime coordinateDecrypt2Runtime_{};
#if 0
    native::HardwareBreakpointCoordinateRuntime
        hardwareBreakpointRuntime_{};
#endif
    native::AlgorithmExecutionContextRefreshPolicy
        algorithmExecutionContextRefreshPolicy_{};
    native::AlgorithmReplayBackoffPolicy algorithmReplayBackoffPolicy_{};
    native::AlgorithmReplayPagePolicy algorithmReplayPagePolicy_{};
    native::AlgorithmPositionRuntimeConfig algorithmPositionConfig_{};
    native::ProcessExecutionContext algorithmExecutionContext_{};
    std::uintptr_t algorithmLastInstructionTracePc_ = 0;
    std::uintptr_t algorithmLastInstructionTraceFault_ = 0;
    std::uint64_t algorithmLastForcedTraceRequest_ = 0;
    native::CoordinateReplayEntrySnapshot coordinateReplayEntrySnapshot_{};
    native::CoordinateReplayEntryDiagnostic coordinateReplayEntryDiagnostic_{};
    std::uintptr_t algorithmGuestPc_ = 0;
    std::uint32_t algorithmEntryInstruction_ = 0;
    bool algorithmExecutionContextReady_ = false;
    bool algorithmEntryReady_ = false;
    bool algorithmReplayAllowedThisFrame_ = true;
    bool algorithmPositionRequested_ = false;
    std::uint32_t coordinateDecrypt2Index_ = 0;
    bool hardwareBreakpointRequested_ = false;
#if 0
    std::chrono::steady_clock::time_point
        hardwareBreakpointRetryAfter_{};
    CoordinateFailure hardwareBreakpointFailure_{};
#endif
    std::string moduleBuildId_;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    bool algorithmDecryptRequested_ = false;
    bool algorithmCoordinateTableReady_ = false;
    bool algorithmCoordinateRuntimeReady_ = false;
#endif
    bool coordinatePoolReady_ = false;
    bool coordinatePoolFallback_ = false;
    std::uintptr_t coordinatePoolBridge_ = 0;
    std::uintptr_t coordinatePoolContext_ = 0;
    std::uintptr_t coordinatePoolEntry_ = 0;
    std::uint64_t algorithmAttemptCount_ = 0;
    std::uint64_t algorithmSuccessCount_ = 0;
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    std::uint64_t algorithmCoordinateRefreshCount_ = 0;
    std::uint64_t algorithmCoordinateResolveAttemptCount_ = 0;
    std::uint64_t algorithmCoordinateResolveSuccessCount_ = 0;
    std::uint64_t algorithmCoordinateAttemptCount_ = 0;
    std::uint64_t algorithmCoordinateSuccessCount_ = 0;
    std::uint64_t algorithmCoordinateObjectAttemptCount_ = 0;
    std::uint64_t algorithmCoordinateObjectSuccessCount_ = 0;
    std::uint64_t algorithmCoordinateTableAttemptCount_ = 0;
    std::uint64_t algorithmCoordinateTableSuccessCount_ = 0;
    std::uint64_t algorithmCoordinateFallbackCount_ = 0;
    std::uint64_t algorithmCoordinateFrameAttemptCount_ = 0;
    std::uint64_t algorithmCoordinateFrameSuccessCount_ = 0;
    native::AlgorithmCoordinateDiagnostic algorithmCoordinateTableDiagnostic_{};
    native::AlgorithmCoordinateDiagnostic algorithmCoordinateFrameFailure_{};
    native::AlgorithmCoordinateDiagnostic algorithmCoordinateFrameSuccess_{};
    native::RuntimeCoordinateCodecDiagnostic
        algorithmCoordinateRuntimeDiagnostic_{};
    native::RuntimeCoordinateCodecDiagnostic
        algorithmCoordinateFrameRuntimeFailure_{};
    native::RuntimeCoordinateCodecDiagnostic
        algorithmCoordinateFrameRuntimeSuccess_{};
    native::AlgorithmCoordinateSource algorithmCoordinateFrameSource_ =
        native::AlgorithmCoordinateSource::None;
#endif
    std::uint64_t algorithmFrameAttemptCount_ = 0;
    std::uint64_t algorithmFrameSuccessCount_ = 0;
    CoordinateDecryptError algorithmFrameOutputError_ =
        CoordinateDecryptError::None;
    CoordinateFailure algorithmFrameFailure_{};
    bool algorithmFrameAgedDecodedFailure_ = false;
    std::uint64_t coordinatePoolFrame_ = 0;
    std::chrono::steady_clock::time_point algorithmEntryValidationAt_{};
    std::chrono::steady_clock::time_point algorithmFailureSince_{};
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    native::TrajectoryHook trajectoryHook_{};
#endif
    pid_t processId_ = -1;
    std::uintptr_t moduleBase_ = 0;
    std::uintptr_t world_ = 0;
    CameraView lastView_{};
    std::uintptr_t lastViewWorld_ = 0;
    std::uintptr_t lastViewCameraManager_ = 0;
    bool lastViewValid_ = false;
    native::ProjectionFovStabilityState viewFovState_{};
    bool opened_ = false;
    std::atomic_bool aimEnabled_{false};
    ActorRecordSnapshot actorRecordSnapshot_{};
    native::ActorRecordRefreshPolicy actorRecordRefreshPolicy_{
        std::chrono::milliseconds(100)};
    std::unordered_map<std::uintptr_t, PositionCacheEntry> positionCache_;
    std::unordered_map<std::uintptr_t, DecodedPositionCacheEntry>
        decodedPositionCache_;
    std::unordered_map<std::uintptr_t, DecodedPositionPendingEntry>
        decodedPositionPending_;
    std::unordered_map<std::uintptr_t, CoordinateTraceRecord>
        coordinateTraceRecords_;
    std::uint64_t coordinateTraceFrame_ = 0;
    std::unordered_map<std::uintptr_t, BoneCacheEntry> boneCache_;
    std::chrono::steady_clock::time_point lastBoneAuditLogAt_{};
    std::unordered_map<std::int32_t, std::string> nameCache_;
    std::unordered_map<std::int32_t, FNameClassTraits> classTraitsCache_;
    std::unordered_map<std::uint64_t, std::pair<int, int>> itemMetadata_;
    WorldObjectFrameCache worldObjectFrameCache_{};
    std::vector<CachedWorldActor> worldObjectActors_;
    native::WorldObjectRefreshPolicy worldObjectDiscoveryPolicy_{
        std::chrono::seconds(10)};
    native::WorldObjectRefreshPolicy worldObjectContentPolicy_{
        std::chrono::milliseconds(500)};
    std::unordered_map<std::uintptr_t, std::vector<Vec3>> projectileTrails_;
    std::unordered_map<
        std::uintptr_t,
        std::chrono::steady_clock::time_point> threatFirstSeen_;
    std::unordered_map<std::uintptr_t, AimWarningState> aimWarningStates_;
    native::HudMapCache hudMapCache_{};
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    native::AlgorithmCoordinateReader algorithmCoordinateReader_{};
    native::RuntimeCoordinateCodec runtimeCoordinateCodec_{};
    std::vector<native::AlgorithmCoordinateRecord>
        algorithmCoordinateSnapshot_;
    std::unordered_map<std::uintptr_t, AlgorithmCoordinateObjectCacheEntry>
        algorithmCoordinateObjectCache_;
#endif
    native::CharacterPositionResolver characterPositions_{};
    native::PositionReadMode positionReadMode_ = native::PositionReadMode::Standard;
    native::ProjectileSpeedReader projectileSpeedReader_{};
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    aim::HitSelectionCache trackingHitSelection_{};
#endif
    native::GeometryRuntime geometryRuntime_{};
    bool geometrySnapshotReady_ = false;
    bool geometryValidationPending_ = false;
    std::uint64_t geometryRefreshEpoch_ = 0;
    std::uint64_t geometryValidationEpoch_ = 0;
    std::chrono::steady_clock::time_point geometryRetryAfter_{};
    std::uintptr_t geometryWorld_ = 0;
    std::uint64_t lockedAimIdentity_ = 0;
    int lockedAimBone_ = -1;
#if LENGJING_ENABLE_PROJECTILE_TRACKING
    std::uint64_t lockedTrackingIdentity_ = 0;
    int lockedTrackingBone_ = -1;
#endif
    aim::AimController aimController_;
    data::CustomItemCatalog customItems_;
    std::string customItemPath_;
};

std::unique_ptr<GameBackend> CreateNativeGameBackend() {
    return std::make_unique<NativeGameBackend>();
}

}  // namespace lengjing::game
