#include "game/native/HardwareBreakpointCoordinateRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lengjing::game::native {
namespace {

constexpr std::uintptr_t kTargetLinkOffset = 0x10;
constexpr std::uintptr_t kManagerOffset = 0x1B8;
constexpr std::uintptr_t kIdArrayOffset = 0xF98;
constexpr std::uintptr_t kCountOffset = 0xFA0;
constexpr std::uintptr_t kMeshStreamCoordinateOffset = 0x80;
constexpr std::uintptr_t kMeshStreamIdOffset = 0x2D8;
constexpr std::uint64_t kMeshStreamFreshPolls = 60;
constexpr std::size_t kCoordinateRecordStride = 0x40;
constexpr std::size_t kCoordinateXOffset = 0x30;
constexpr std::size_t kCoordinateYOffset = 0x34;
constexpr std::size_t kCoordinateZOffset = 0x38;
constexpr std::int32_t kMinimumCoordinateCount = 15;
constexpr std::int32_t kMaximumCoordinateCount = 16384;
constexpr float kCoordinateZAdjustment = 80.0f;
constexpr float kFallbackCoordinateMagnitudeLimit = 1.0e6f;
constexpr float kFallbackCoordinateMagnitudeMinimum = 1.0e-3f;
constexpr float kFallbackStrongMagnitudeMinimum = 1.0f;
constexpr std::size_t kFallbackMinimumRequiredCoordinates = 16;
constexpr std::size_t kFallbackMinimumPlausiblePercent = 45;
constexpr std::size_t kFallbackCandidatesPerPoll = 3;
constexpr std::size_t kStableCandidatesPerPoll = 6;
constexpr std::uint64_t kFallbackCandidateRetestPolls = 300;
constexpr std::uint64_t kFallbackPublishedCandidateStalePolls = 2;
constexpr std::uint8_t kFallbackCandidateQualificationCount = 2;
constexpr std::uint8_t kFallbackCandidateShortRetryLimit = 2;
constexpr std::uint8_t kOrderedRefreshFailureLimit = 3;
constexpr std::uint64_t kCandidateSourceFreshPolls = 300;
constexpr double kCandidateSourceDistanceLimit = 500.0;
constexpr std::size_t kCandidateSourceMinimumDistinctIds = 1;
constexpr std::uintptr_t kPageOffsetMask = 0xFFF;
constexpr std::uintptr_t kPointerPayloadMask =
    UINT64_C(0x00FFFFFFFFFFFFFF);
constexpr std::uintptr_t kRejectedCandidateMask = UINT64_C(0xFFFF0000);
constexpr std::uintptr_t kRejectedCandidateValue = UINT64_C(0xDEAD0000);
constexpr std::uintptr_t kObservedPointerLower =
    UINT64_C(0x5FEEE000FF);
constexpr std::uintptr_t kObservedPointerUpper =
    UINT64_C(0x8000000330);
constexpr std::uintptr_t kTaggedPointerMinimum =
    UINT64_C(0xB400000000000000);
constexpr std::uintptr_t kTaggedPointerPayloadMask =
    UINT64_C(0x00000FFFFFFFFFFF);

static_assert(sizeof(HardwareBreakpointCoordinate) == 12);
static_assert(kCoordinateZOffset + sizeof(float) <=
              kCoordinateRecordStride);

struct CoordinateBits {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t z = 0;

    bool operator==(const CoordinateBits& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct CoordinateBitsHash {
    std::size_t operator()(const CoordinateBits& value) const noexcept {
        std::size_t hash = value.x;
        hash ^= static_cast<std::size_t>(value.y) +
            UINT64_C(0x9E3779B97F4A7C15) + (hash << 6U) + (hash >> 2U);
        hash ^= static_cast<std::size_t>(value.z) +
            UINT64_C(0x9E3779B97F4A7C15) + (hash << 6U) + (hash >> 2U);
        return hash;
    }
};

bool AddOffset(std::uintptr_t base,
               std::uintptr_t offset,
               std::uintptr_t& address) noexcept {
    if (base == 0 ||
        base > std::numeric_limits<std::uintptr_t>::max() - offset) {
        return false;
    }
    address = base + offset;
    return true;
}

bool IsReadableRange(std::uintptr_t address, std::size_t size) noexcept {
    return address != 0 && size != 0 &&
        size - 1 <=
            std::numeric_limits<std::uintptr_t>::max() - address;
}

bool ReadMemory(const HardwareBreakpointCoordinateCallbacks& callbacks,
                std::uintptr_t address,
                void* destination,
                std::size_t size) noexcept {
    if (destination == nullptr || !IsReadableRange(address, size)) {
        return false;
    }
    try {
        return callbacks.readMemory(address, destination, size);
    } catch (...) {
        return false;
    }
}

bool UsesOrderedRecordTable(
    HardwareBreakpointCoordinateProfile profile) noexcept {
    return profile ==
        HardwareBreakpointCoordinateProfile::OrderedRecordTable;
}

bool UsesMeshStream(
    HardwareBreakpointCoordinateProfile profile) noexcept {
    return profile == HardwareBreakpointCoordinateProfile::MeshStream;
}

bool UsesStableRecordTable(
    HardwareBreakpointCoordinateProfile profile) noexcept {
    return profile ==
        HardwareBreakpointCoordinateProfile::StableRecordTable;
}

bool IsObservedPointer(std::uintptr_t pointer) noexcept {
    return pointer > kObservedPointerLower &&
        pointer < kObservedPointerUpper;
}

bool NormalizeObservedPointer(std::uintptr_t raw,
                              std::uintptr_t& pointer) noexcept {
    pointer = raw >= kTaggedPointerMinimum
        ? raw & kTaggedPointerPayloadMask
        : raw;
    return IsObservedPointer(pointer);
}

bool IsValidCoordinate(
    const HardwareBreakpointCoordinate& coordinate) noexcept {
    const bool finite = std::isfinite(coordinate.x) &&
        std::isfinite(coordinate.y) &&
        std::isfinite(coordinate.z);
    if (!finite) return false;
    return coordinate.x != 0.0f ||
        coordinate.y != 0.0f ||
        coordinate.z != 0.0f;
}

bool IsPlausibleFallbackCoordinate(
    const HardwareBreakpointCoordinate& input,
    const HardwareBreakpointCoordinate& output) noexcept {
    if (!std::isfinite(input.x) || !std::isfinite(input.y) ||
        !std::isfinite(input.z) || !std::isfinite(output.x) ||
        !std::isfinite(output.y) || !std::isfinite(output.z)) {
        return false;
    }
    if (std::fabs(output.x) > kFallbackCoordinateMagnitudeLimit ||
        std::fabs(output.y) > kFallbackCoordinateMagnitudeLimit ||
        std::fabs(output.z) > kFallbackCoordinateMagnitudeLimit) {
        return false;
    }
    return std::fabs(input.x) >=
            kFallbackCoordinateMagnitudeMinimum ||
        std::fabs(input.y) >= kFallbackCoordinateMagnitudeMinimum ||
        std::fabs(input.z) >= kFallbackCoordinateMagnitudeMinimum;
}

bool IsStrongFallbackCoordinate(
    const HardwareBreakpointCoordinate& input) noexcept {
    return std::fabs(input.x) >= kFallbackStrongMagnitudeMinimum ||
        std::fabs(input.y) >= kFallbackStrongMagnitudeMinimum ||
        std::fabs(input.z) >= kFallbackStrongMagnitudeMinimum;
}

CoordinateBits GetCoordinateBits(
    const HardwareBreakpointCoordinate& coordinate) noexcept {
    CoordinateBits bits{};
    std::memcpy(&bits.x, &coordinate.x, sizeof(bits.x));
    std::memcpy(&bits.y, &coordinate.y, sizeof(bits.y));
    std::memcpy(&bits.z, &coordinate.z, sizeof(bits.z));
    return bits;
}

std::uintptr_t MostFrequentCandidate(
    const std::array<std::uintptr_t, 10>& candidates,
    std::size_t count) noexcept {
    if (count == 0 || count > candidates.size()) return 0;

    std::uintptr_t best = 0;
    std::size_t bestCount = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const std::uintptr_t candidate = candidates[index];
        if (candidate == 0) continue;
        std::size_t occurrences = 0;
        for (std::size_t other = 0; other < count; ++other) {
            if (candidates[other] == candidate) ++occurrences;
        }
        if (occurrences > bestCount) {
            best = candidate;
            bestCount = occurrences;
        }
    }
    return best;
}

void TraceOrderedManager(
    std::uint64_t pollCount,
    const char* stage,
    std::uintptr_t targetRoot,
    std::uintptr_t first,
    std::uintptr_t second,
    std::uintptr_t rawManager,
    std::uintptr_t manager) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    if (pollCount > 5 && (pollCount % 300) != 0) return;
    std::fprintf(
        stderr,
        "[hwbp-manager] poll=%llu stage=%s target=%llx first=%llx "
        "second=%llx raw_manager=%llx manager=%llx\n",
        static_cast<unsigned long long>(pollCount),
        stage,
        static_cast<unsigned long long>(targetRoot),
        static_cast<unsigned long long>(first),
        static_cast<unsigned long long>(second),
        static_cast<unsigned long long>(rawManager),
        static_cast<unsigned long long>(manager));
    std::fflush(stderr);
#else
    static_cast<void>(pollCount);
    static_cast<void>(stage);
    static_cast<void>(targetRoot);
    static_cast<void>(first);
    static_cast<void>(second);
    static_cast<void>(rawManager);
    static_cast<void>(manager);
#endif
}

void TraceOrderedSample(
    std::uint64_t pollCount,
    pid_t tid,
    std::uint64_t hitCount,
    std::uintptr_t x20,
    std::uintptr_t x21,
    std::uintptr_t x23,
    std::uintptr_t candidate,
    bool accepted) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    if (pollCount > 5 && (pollCount % 30) != 0) return;
    std::fprintf(
        stderr,
        "[hwbp-sample] poll=%llu tid=%d hits=%llu x20=%llx x21=%llx "
        "x23=%llx candidate=%llx accepted=%d\n",
        static_cast<unsigned long long>(pollCount),
        static_cast<int>(tid),
        static_cast<unsigned long long>(hitCount),
        static_cast<unsigned long long>(x20),
        static_cast<unsigned long long>(x21),
        static_cast<unsigned long long>(x23),
        static_cast<unsigned long long>(candidate),
        accepted ? 1 : 0);
    std::fflush(stderr);
#else
    static_cast<void>(pollCount);
    static_cast<void>(tid);
    static_cast<void>(hitCount);
    static_cast<void>(x20);
    static_cast<void>(x21);
    static_cast<void>(x23);
    static_cast<void>(candidate);
    static_cast<void>(accepted);
#endif
}

void TraceOrderedSelection(
    std::uint64_t pollCount,
    std::uintptr_t selected,
    std::size_t candidateCount) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    if (pollCount > 5 && (pollCount % 300) != 0) return;
    std::fprintf(
        stderr,
        "[hwbp-selection] poll=%llu selected=%llx samples=%zu\n",
        static_cast<unsigned long long>(pollCount),
        static_cast<unsigned long long>(selected),
        candidateCount);
    std::fflush(stderr);
#else
    static_cast<void>(pollCount);
    static_cast<void>(selected);
    static_cast<void>(candidateCount);
#endif
}

void TraceOrderedQuality(
    std::uint64_t pollCount,
    std::uintptr_t candidate,
    std::size_t nonzeroIds,
    std::size_t exact,
    std::size_t plausible,
    std::size_t evidence,
    std::size_t distinct,
    std::size_t required,
    bool qualified) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    if (pollCount > 5 && (pollCount % 30) != 0) return;
    std::fprintf(
        stderr,
        "[hwbp-quality] poll=%llu candidate=%llx ids=%zu exact=%zu "
        "plausible=%zu evidence=%zu distinct=%zu required=%zu "
        "qualified=%d\n",
        static_cast<unsigned long long>(pollCount),
        static_cast<unsigned long long>(candidate),
        nonzeroIds,
        exact,
        plausible,
        evidence,
        distinct,
        required,
        qualified ? 1 : 0);
    std::fflush(stderr);
#else
    static_cast<void>(pollCount);
    static_cast<void>(candidate);
    static_cast<void>(nonzeroIds);
    static_cast<void>(exact);
    static_cast<void>(plausible);
    static_cast<void>(evidence);
    static_cast<void>(distinct);
    static_cast<void>(required);
    static_cast<void>(qualified);
#endif
}

void TraceSourceQuality(
    std::uint64_t pollCount,
    std::uintptr_t candidate,
    std::size_t stored,
    std::size_t fresh,
    std::size_t expired,
    std::size_t addressFailures,
    std::size_t readFailures,
    std::size_t invalid,
    std::size_t usable,
    std::size_t idMisses,
    std::size_t present,
    std::size_t distanceFailures,
    std::size_t matched,
    std::size_t distinctIds,
    double closestDistance,
    bool qualified) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    std::fprintf(
        stderr,
        "[hwbp-source-quality] poll=%llu candidate=%llx stored=%zu "
        "fresh=%zu expired=%zu address_fail=%zu read_fail=%zu "
        "invalid=%zu usable=%zu id_miss=%zu present=%zu "
        "distance_fail=%zu matched=%zu distinct_ids=%zu closest=%.1f "
        "qualified=%d\n",
        static_cast<unsigned long long>(pollCount),
        static_cast<unsigned long long>(candidate),
        stored,
        fresh,
        expired,
        addressFailures,
        readFailures,
        invalid,
        usable,
        idMisses,
        present,
        distanceFailures,
        matched,
        distinctIds,
        closestDistance,
        qualified ? 1 : 0);
    std::fflush(stderr);
#else
    static_cast<void>(pollCount);
    static_cast<void>(candidate);
    static_cast<void>(stored);
    static_cast<void>(fresh);
    static_cast<void>(expired);
    static_cast<void>(addressFailures);
    static_cast<void>(readFailures);
    static_cast<void>(invalid);
    static_cast<void>(usable);
    static_cast<void>(idMisses);
    static_cast<void>(present);
    static_cast<void>(distanceFailures);
    static_cast<void>(matched);
    static_cast<void>(distinctIds);
    static_cast<void>(closestDistance);
    static_cast<void>(qualified);
#endif
}

void TraceOrderedTable(
    std::uint64_t pollCount,
    const char* stage,
    std::uintptr_t world,
    std::uintptr_t targetRoot,
    std::uintptr_t recordsBase,
    std::uintptr_t manager,
    std::uintptr_t rawIdArray,
    std::uintptr_t idArray,
    std::uint32_t rawCount,
    std::int64_t logicalCount,
    std::size_t refreshed,
    std::size_t published,
    bool force = false) noexcept {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    if (!force && pollCount > 5 &&
        (pollCount % 300) != 0) {
        return;
    }
    std::fprintf(
        stderr,
        "[hwbp-table] poll=%llu stage=%s world=%llx target=%llx "
        "records=%llx "
        "manager=%llx raw_ids=%llx ids=%llx raw_count=%u "
        "logical_count=%lld refreshed=%zu published=%zu\n",
        static_cast<unsigned long long>(pollCount),
        stage,
        static_cast<unsigned long long>(world),
        static_cast<unsigned long long>(targetRoot),
        static_cast<unsigned long long>(recordsBase),
        static_cast<unsigned long long>(manager),
        static_cast<unsigned long long>(rawIdArray),
        static_cast<unsigned long long>(idArray),
        static_cast<unsigned int>(rawCount),
        static_cast<long long>(logicalCount),
        refreshed,
        published);
    std::fflush(stderr);
#else
    static_cast<void>(pollCount);
    static_cast<void>(stage);
    static_cast<void>(world);
    static_cast<void>(targetRoot);
    static_cast<void>(recordsBase);
    static_cast<void>(manager);
    static_cast<void>(rawIdArray);
    static_cast<void>(idArray);
    static_cast<void>(rawCount);
    static_cast<void>(logicalCount);
    static_cast<void>(refreshed);
    static_cast<void>(published);
    static_cast<void>(force);
#endif
}

}  // namespace

HardwareBreakpointCoordinateRuntime::~HardwareBreakpointCoordinateRuntime() {
    static_cast<void>(Stop());
}

bool HardwareBreakpointCoordinateRuntime::Start(
    std::uintptr_t breakpointAddress,
    HardwareBreakpointCoordinateCallbacks callbacks,
    HardwareBreakpointCoordinateProfile profile) noexcept {
    if (breakpointAddress == 0 || (breakpointAddress & 3U) != 0 ||
        !callbacks ||
        ((UsesMeshStream(profile) || UsesStableRecordTable(profile)) &&
         !callbacks.readRecords)) {
        return false;
    }
    if (active_ && breakpointAddress_ == breakpointAddress &&
        profile_ == profile && !needsReconfigure_) {
        return true;
    }
    if (active_ && !Stop()) return false;

    bool configured = false;
    try {
        configured = callbacks.configureBreakpoint(breakpointAddress);
    } catch (...) {
        configured = false;
    }
    if (!configured) {
        try {
            static_cast<void>(callbacks.removeBreakpoints());
        } catch (...) {
        }
        return false;
    }

    callbacks_ = std::move(callbacks);
    breakpointAddress_ = breakpointAddress;
    profile_ = profile;
    needsReconfigure_ = false;
    active_ = true;
    ClearSamplingState();
    return true;
}

bool HardwareBreakpointCoordinateRuntime::Reconfigure(
    std::uintptr_t breakpointAddress,
    HardwareBreakpointCoordinateCallbacks callbacks,
    HardwareBreakpointCoordinateProfile profile) noexcept {
    if (!active_ || breakpointAddress == 0 ||
        (breakpointAddress & 3U) != 0 || !callbacks ||
        ((UsesMeshStream(profile) || UsesStableRecordTable(profile)) &&
         !callbacks.readRecords)) {
        return false;
    }

    ClearSamplingState();
    bool removed = false;
    try {
        removed = callbacks_.removeBreakpoints();
    } catch (...) {
        removed = false;
    }
    if (!removed) {
        needsReconfigure_ = true;
        return false;
    }

    bool configured = false;
    try {
        configured = callbacks.configureBreakpoint(breakpointAddress);
    } catch (...) {
        configured = false;
    }
    if (!configured) {
        needsReconfigure_ = true;
        return false;
    }

    callbacks_ = std::move(callbacks);
    breakpointAddress_ = breakpointAddress;
    profile_ = profile;
    needsReconfigure_ = false;
    return true;
}

bool HardwareBreakpointCoordinateRuntime::Stop() noexcept {
    if (!active_) {
        callbacks_ = {};
        breakpointAddress_ = 0;
        ClearSamplingState();
        profile_ = HardwareBreakpointCoordinateProfile::Existing;
        return true;
    }

    bool removed = false;
    try {
        removed = callbacks_.removeBreakpoints();
    } catch (...) {
        removed = false;
    }
    if (!removed) return false;

    active_ = false;
    callbacks_ = {};
    breakpointAddress_ = 0;
    ClearSamplingState();
    profile_ = HardwareBreakpointCoordinateProfile::Existing;
    return true;
}

bool HardwareBreakpointCoordinateRuntime::Poll(
    std::uintptr_t world) noexcept {
    return Poll(world, world, 0);
}

bool HardwareBreakpointCoordinateRuntime::Poll(
    std::uintptr_t world,
    std::uintptr_t manager) noexcept {
    return Poll(world, world, manager);
}

bool HardwareBreakpointCoordinateRuntime::Poll(
    std::uintptr_t world,
    std::uintptr_t targetRoot,
    std::uintptr_t manager) noexcept {
    if (!active_ || world == 0) return false;
    ++pollCount_;
    ResetWorld(world);
    if (UsesMeshStream(profile_)) {
        static_cast<void>(targetRoot);
        static_cast<void>(manager);
        const bool sampled = SampleMeshStream();
        ExpireMeshStream();
        return sampled;
    }
    if (UsesStableRecordTable(profile_)) {
        static_cast<void>(targetRoot);
        static_cast<void>(manager);
        if (!SampleRecordsBase() ||
            (recordsBase_ == 0 && pendingRecordsBase_ == 0)) {
            return false;
        }

        std::uintptr_t observedManager = 0;
        if (!ReadObservedManager(world, observedManager)) return false;
        return RefreshOrderedFallbackCoordinateTable(
            world, world, observedManager);
    }
    if (targetRoot == 0) return false;

    const bool orderedRecordTable = UsesOrderedRecordTable(profile_);
    const bool orderedFallback =
        orderedRecordTable && !callbacks_.readCandidate;
    const auto failBeforeOrderedRefresh = [&]() noexcept {
        if (orderedFallback && recordsBase_ != 0) {
            if (orderedRefreshFailureCount_ <
                kOrderedRefreshFailureLimit) {
                ++orderedRefreshFailureCount_;
            }
            if (orderedRefreshFailureCount_ >=
                kOrderedRefreshFailureLimit) {
                const std::uintptr_t expiredBase = recordsBase_;
                ClearPublishedState();
                for (CandidateObservation& observation :
                     candidateObservations_) {
                    if (observation.value == expiredBase) {
                        observation.lastEvaluatedPoll = 0;
                        observation.qualifiedStreak = 0;
                        break;
                    }
                }
            }
        }
        return false;
    };
    if (orderedRecordTable && samplingTargetRoot_ != 0 &&
        samplingTargetRoot_ != targetRoot) {
        ClearWorldState();
        samplingTargetRoot_ = targetRoot;
    }
    if (!SampleRecordsBase()) return failBeforeOrderedRefresh();
    if ((!orderedRecordTable && recordsBase_ == 0) ||
        (orderedRecordTable && !orderedFallback &&
         pendingRecordsBase_ == 0) ||
        (orderedFallback && recordsBase_ == 0 &&
         pendingRecordsBase_ == 0)) {
        return false;
    }

    std::uintptr_t observedManager = 0;
    if (!ReadObservedManager(targetRoot, observedManager)) {
        return failBeforeOrderedRefresh();
    }
    if (orderedRecordTable) {
        const bool generationChanged =
            samplingTargetRoot_ != 0 &&
            samplingManager_ != 0 &&
            samplingManager_ != observedManager;
        if (generationChanged) {
            ClearWorldState();
            samplingTargetRoot_ = targetRoot;
            samplingManager_ = observedManager;
            return false;
        }
        samplingTargetRoot_ = targetRoot;
        samplingManager_ = observedManager;
    }
    if (manager != 0 && manager != observedManager) {
        return false;
    }
    return RefreshCoordinateTable(world, targetRoot, observedManager);
}

bool HardwareBreakpointCoordinateRuntime::Lookup(
    std::uint32_t id,
    std::uintptr_t world,
    HardwareBreakpointCoordinate& coordinate) noexcept {
    coordinate = {};
    if (UsesMeshStream(profile_)) return false;
    if (!active_ || id == 0 || world == 0 || world != world_) {
        return false;
    }
    const auto found = coordinates_.find(id);
    if (found == coordinates_.end()) return false;
    coordinate = found->second;
    return true;
}

bool HardwareBreakpointCoordinateRuntime::Lookup(
    std::uint32_t id,
    std::uintptr_t mesh,
    std::uintptr_t world,
    HardwareBreakpointCoordinate& coordinate) noexcept {
    if (UsesMeshStream(profile_)) {
        coordinate = {};
        if (!active_ || id == 0 || mesh == 0 || world == 0 ||
            world != world_) {
            return false;
        }
        const auto found = meshStreamCoordinates_.find(id);
        if (found == meshStreamCoordinates_.end()) return false;
        const std::uintptr_t normalizedMesh =
            mesh & kPointerPayloadMask;
        if (normalizedMesh == 0 ||
            found->second.mesh != normalizedMesh ||
            pollCount_ < found->second.lastSeenPoll ||
            pollCount_ - found->second.lastSeenPoll >=
                kMeshStreamFreshPolls) {
            return false;
        }
        coordinate = found->second.value;
        return true;
    }
    return Lookup(id, world, coordinate);
}

void HardwareBreakpointCoordinateRuntime::ResetWorld(
    std::uintptr_t world) noexcept {
    if (world_ == world) return;
    ClearWorldState();
    world_ = world;
}

bool HardwareBreakpointCoordinateRuntime::IsActive() const noexcept {
    return active_;
}

std::uintptr_t
HardwareBreakpointCoordinateRuntime::BreakpointAddress() const noexcept {
    return breakpointAddress_;
}

std::uintptr_t
HardwareBreakpointCoordinateRuntime::RecordsBase() const noexcept {
    return recordsBase_;
}

std::size_t
HardwareBreakpointCoordinateRuntime::PublishedCoordinateCount() const noexcept {
    return UsesMeshStream(profile_)
        ? meshStreamCoordinates_.size()
        : coordinates_.size();
}

std::uint64_t HardwareBreakpointCoordinateRuntime::PollCount() const noexcept {
    return pollCount_;
}

std::uint64_t
HardwareBreakpointCoordinateRuntime::AcceptedSampleCount() const noexcept {
    return acceptedSampleCount_;
}

bool HardwareBreakpointCoordinateRuntime::NeedsReconfigure() const noexcept {
    return active_ && needsReconfigure_;
}

bool HardwareBreakpointCoordinateRuntime::SampleMeshStream() noexcept {
    if (!callbacks_.readRecords) return false;

    std::size_t recordsRead = 0;
    std::size_t totalRecords = 0;
    std::uintptr_t hitAddress = 0;
    bool read = false;
    try {
        read = callbacks_.readRecords(
            records_.data(), records_.size(), recordsRead, hitAddress,
            totalRecords);
    } catch (...) {
        read = false;
    }
    if (!read || recordsRead > records_.size() ||
        totalRecords > records_.size()) {
        return false;
    }
    if (hitAddress != breakpointAddress_ ||
        (lastHitAddress_ != 0 && lastHitAddress_ != hitAddress)) {
        needsReconfigure_ = true;
        ClearConfigurationState();
        ClearWorldState();
        return false;
    }
    if (totalRecords < lastTotalRecords_) {
        ClearWorldState();
    }

    needsReconfigure_ = false;
    lastHitAddress_ = hitAddress;
    lastTotalRecords_ = totalRecords;
    const auto previousSeenRecords = seenRecords_;
    std::array<SeenRecord, kExecutionBreakpointRecordLimit>
        nextSeenRecords{};

    for (std::size_t index = 0; index < recordsRead; ++index) {
        const ExecutionBreakpointRecord& record = records_[index];
        if (record.tid <= 0 || record.hitCount == 0 ||
            record.pc != breakpointAddress_) {
            continue;
        }

        bool seen = false;
        for (const SeenRecord& previous : previousSeenRecords) {
            if (previous.valid && previous.tid == record.tid &&
                previous.hitCount == record.hitCount &&
                previous.pc == record.pc &&
                previous.x20 == record.x20 &&
                previous.x21 == record.x21) {
                seen = true;
                break;
            }
        }
        nextSeenRecords[index] = {
            record.tid,
            record.hitCount,
            record.pc,
            record.x20,
            record.x21,
            true,
        };
        if (seen) continue;

        const std::uintptr_t coordinateBase =
            record.x20 & kPointerPayloadMask;
        const std::uintptr_t mesh =
            record.x21 & kPointerPayloadMask;
        std::uintptr_t coordinateAddress = 0;
        std::uintptr_t idAddress = 0;
        if (!AddOffset(
                coordinateBase,
                kMeshStreamCoordinateOffset,
                coordinateAddress) ||
            !AddOffset(mesh, kMeshStreamIdOffset, idAddress)) {
            continue;
        }

        std::uint32_t firstId = 0;
        std::uint32_t secondId = 0;
        HardwareBreakpointCoordinate coordinate{};
        if (!ReadMemory(
                callbacks_, idAddress, &firstId, sizeof(firstId)) ||
            firstId == 0 ||
            !ReadMemory(
                callbacks_,
                coordinateAddress,
                &coordinate,
                sizeof(coordinate)) ||
            !ReadMemory(
                callbacks_, idAddress, &secondId, sizeof(secondId)) ||
            firstId != secondId || !IsValidCoordinate(coordinate)) {
            continue;
        }

        const auto previousId = meshStreamIds_.find(mesh);
        if (previousId != meshStreamIds_.end() &&
            previousId->second != firstId) {
            const auto previousCoordinate =
                meshStreamCoordinates_.find(previousId->second);
            if (previousCoordinate != meshStreamCoordinates_.end() &&
                previousCoordinate->second.mesh == mesh) {
                meshStreamCoordinates_.erase(previousCoordinate);
            }
        }
        const auto previousMesh =
            meshStreamCoordinates_.find(firstId);
        if (previousMesh != meshStreamCoordinates_.end() &&
            previousMesh->second.mesh != mesh) {
            const auto oldMesh =
                meshStreamIds_.find(previousMesh->second.mesh);
            if (oldMesh != meshStreamIds_.end() &&
                oldMesh->second == firstId) {
                meshStreamIds_.erase(oldMesh);
            }
        }
        meshStreamIds_[mesh] = firstId;
        meshStreamCoordinates_[firstId] = {
            mesh,
            coordinate,
            pollCount_,
        };
        recordsBase_ = coordinateBase;
        ++acceptedSampleCount_;
    }
    seenRecords_ = nextSeenRecords;
    return true;
}

void HardwareBreakpointCoordinateRuntime::ExpireMeshStream() noexcept {
    for (auto iterator = meshStreamCoordinates_.begin();
         iterator != meshStreamCoordinates_.end();) {
        const bool expired =
            pollCount_ < iterator->second.lastSeenPoll ||
            pollCount_ - iterator->second.lastSeenPoll >=
                kMeshStreamFreshPolls;
        if (!expired) {
            ++iterator;
            continue;
        }
        const auto mesh = meshStreamIds_.find(iterator->second.mesh);
        if (mesh != meshStreamIds_.end() &&
            mesh->second == iterator->first) {
            meshStreamIds_.erase(mesh);
        }
        iterator = meshStreamCoordinates_.erase(iterator);
    }
    if (meshStreamCoordinates_.empty()) recordsBase_ = 0;
}

bool HardwareBreakpointCoordinateRuntime::
SampleStableRecordsBase() noexcept {
    std::size_t recordsRead = 0;
    std::size_t totalRecords = 0;
    std::uintptr_t hitAddress = 0;
    bool read = false;
    try {
        read = callbacks_.readRecords(
            records_.data(), records_.size(), recordsRead, hitAddress,
            totalRecords);
    } catch (...) {
        read = false;
    }
    if (!read || recordsRead > records_.size() ||
        totalRecords > records_.size() ||
        hitAddress != breakpointAddress_) {
        return false;
    }

    if ((lastHitAddress_ != 0 &&
         lastHitAddress_ != hitAddress) ||
        totalRecords < lastTotalRecords_) {
        ClearWorldState();
    }
    needsReconfigure_ = false;
    lastHitAddress_ = hitAddress;
    lastTotalRecords_ = totalRecords;

    const auto previousSeenRecords = seenRecords_;
    std::array<SeenRecord, kExecutionBreakpointRecordLimit>
        nextSeenRecords{};

    const auto isLatestThreadRecord =
        [&](std::size_t recordIndex) noexcept {
            const ExecutionBreakpointRecord& record =
                records_[recordIndex];
            for (std::size_t index = 0; index < recordsRead; ++index) {
                if (records_[index].tid == record.tid &&
                    records_[index].hitCount > record.hitCount) {
                    return false;
                }
            }
            return true;
        };
    for (int latestPass = 0; latestPass < 2; ++latestPass) {
        for (std::size_t index = 0; index < recordsRead; ++index) {
            const ExecutionBreakpointRecord& record = records_[index];
            if (record.tid <= 0 || record.hitCount == 0 ||
                record.pc != breakpointAddress_ ||
                isLatestThreadRecord(index) != (latestPass != 0)) {
                continue;
            }

            bool seen = false;
            for (const SeenRecord& previous : previousSeenRecords) {
                if (previous.valid && previous.tid == record.tid &&
                    previous.hitCount == record.hitCount &&
                    previous.pc == record.pc) {
                    seen = true;
                    break;
                }
            }
            nextSeenRecords[index] = {
                record.tid,
                record.hitCount,
                record.pc,
                record.x20,
                record.x21,
                true,
            };
            if (seen) continue;

            const std::uintptr_t candidate =
                record.x23 & kPointerPayloadMask;
            std::uint64_t probe = 0;
            if (candidate == 0 ||
                (candidate & kRejectedCandidateMask) ==
                    kRejectedCandidateValue ||
                !ReadMemory(
                    callbacks_, candidate, &probe, sizeof(probe))) {
                continue;
            }

            candidateRing_[candidateWriteIndex_] = candidate;
            candidateWriteIndex_ =
                (candidateWriteIndex_ + 1) % candidateRing_.size();
            if (candidateCount_ < candidateRing_.size()) {
                ++candidateCount_;
            }
            ObserveCandidate(candidate, record.x20, record.x21);
            ++acceptedSampleCount_;
        }
    }
    seenRecords_ = nextSeenRecords;

    const std::uintptr_t selected =
        MostFrequentCandidate(candidateRing_, candidateCount_);
    pendingRecordsBase_ = selected;
    return true;
}

bool HardwareBreakpointCoordinateRuntime::SampleRecordsBase() noexcept {
    if (UsesStableRecordTable(profile_)) {
        return SampleStableRecordsBase();
    }
    if (callbacks_.readCandidate) {
        return SampleCandidate();
    }

    std::size_t recordsRead = 0;
    std::size_t totalRecords = 0;
    std::uintptr_t hitAddress = 0;
    bool read = false;
    try {
        read = callbacks_.readRecords(
            records_.data(), records_.size(), recordsRead, hitAddress,
            totalRecords);
    } catch (...) {
        read = false;
    }
    if (!read || recordsRead > records_.size() ||
        totalRecords > records_.size() ||
        hitAddress != breakpointAddress_) {
        if (UsesOrderedRecordTable(profile_)) {
            needsReconfigure_ = true;
            ClearConfigurationState();
        }
        return false;
    }

    if (lastHitAddress_ != 0 &&
        lastHitAddress_ != hitAddress) {
        if (UsesOrderedRecordTable(profile_)) {
            needsReconfigure_ = true;
            ClearConfigurationState();
            return false;
        }
        ClearWorldState();
    } else if (
        totalRecords < lastTotalRecords_ &&
        !UsesOrderedRecordTable(profile_)) {
        ClearWorldState();
    }
    needsReconfigure_ = false;
    lastHitAddress_ = hitAddress;
    lastTotalRecords_ = totalRecords;

    const auto previousSeenRecords = seenRecords_;
    std::array<SeenRecord, kExecutionBreakpointRecordLimit>
        nextSeenRecords{};

    for (std::size_t index = 0; index < recordsRead; ++index) {
        const ExecutionBreakpointRecord& record = records_[index];
        if (record.tid <= 0 || record.hitCount == 0 ||
            record.pc != breakpointAddress_) {
            continue;
        }

        bool seen = false;
        for (const SeenRecord& previous : previousSeenRecords) {
            if (previous.valid && previous.tid == record.tid &&
                previous.hitCount == record.hitCount &&
                (UsesOrderedRecordTable(profile_) ||
                 previous.pc == record.pc)) {
                seen = true;
                break;
            }
        }
        nextSeenRecords[index] = {
            record.tid,
            record.hitCount,
            record.pc,
            record.x20,
            record.x21,
            true,
        };
        if (seen) continue;

        const std::uintptr_t candidate =
            record.x23 & kPointerPayloadMask;
        std::uint64_t probe = 0;
        const bool accepted =
            candidate != 0 &&
            (!UsesOrderedRecordTable(profile_) ||
             IsObservedPointer(candidate)) &&
            (candidate & kRejectedCandidateMask) !=
                kRejectedCandidateValue &&
            ReadMemory(
                callbacks_, candidate, &probe, sizeof(probe));
        if (UsesOrderedRecordTable(profile_)) {
            TraceOrderedSample(
                pollCount_, record.tid, record.hitCount, record.x20,
                record.x21, record.x23, candidate, accepted);
        }
        if (!accepted) {
            continue;
        }
        if (UsesOrderedRecordTable(profile_)) {
            ObserveCandidate(candidate, record.x20, record.x21);
        }
        candidateRing_[candidateWriteIndex_] = candidate;
        candidateWriteIndex_ =
            (candidateWriteIndex_ + 1) % candidateRing_.size();
        if (candidateCount_ < candidateRing_.size()) {
            ++candidateCount_;
        }
        ++acceptedSampleCount_;
    }
    seenRecords_ = nextSeenRecords;

    const std::uintptr_t selected =
        MostFrequentCandidate(candidateRing_, candidateCount_);
    if (UsesOrderedRecordTable(profile_)) {
        TraceOrderedSelection(
            pollCount_, selected, candidateCount_);
    }
    if (UsesOrderedRecordTable(profile_)) {
        pendingRecordsBase_ = selected;
    } else if (selected != recordsBase_) {
        recordsBase_ = selected;
        coordinates_.clear();
    }
    return true;
}

bool HardwareBreakpointCoordinateRuntime::SampleCandidate() noexcept {
    std::uintptr_t rawCandidate = 0;
    HardwareBreakpointCandidateSampleResult result =
        HardwareBreakpointCandidateSampleResult::Reset;
    try {
        result = callbacks_.readCandidate(rawCandidate);
    } catch (...) {
        result = HardwareBreakpointCandidateSampleResult::Reset;
    }

    if (result == HardwareBreakpointCandidateSampleResult::Reset) {
        needsReconfigure_ = true;
        ClearConfigurationState();
        ClearWorldState();
        return false;
    }
    needsReconfigure_ = false;
    if (result == HardwareBreakpointCandidateSampleResult::Retry) {
        return true;
    }

    const std::uintptr_t candidate =
        rawCandidate & kPointerPayloadMask;
    std::uint64_t probe = 0;
    const bool accepted =
        candidate != 0 &&
        (!UsesOrderedRecordTable(profile_) ||
         IsObservedPointer(candidate)) &&
        (candidate & kRejectedCandidateMask) !=
            kRejectedCandidateValue &&
        ReadMemory(callbacks_, candidate, &probe, sizeof(probe));
    if (UsesOrderedRecordTable(profile_)) {
        TraceOrderedSample(
            pollCount_, -1, acceptedSampleCount_ + 1,
            0, 0, rawCandidate, candidate, accepted);
    }
    if (!accepted) return true;

    candidateRing_[candidateWriteIndex_] = candidate;
    candidateWriteIndex_ =
        (candidateWriteIndex_ + 1) % candidateRing_.size();
    if (candidateCount_ < candidateRing_.size()) {
        ++candidateCount_;
    }
    ++acceptedSampleCount_;

    const std::uintptr_t selected =
        MostFrequentCandidate(candidateRing_, candidateCount_);
    if (UsesOrderedRecordTable(profile_)) {
        TraceOrderedSelection(
            pollCount_, selected, candidateCount_);
    }
    if (UsesOrderedRecordTable(profile_)) {
        pendingRecordsBase_ = selected;
    } else if (selected != recordsBase_) {
        recordsBase_ = selected;
        coordinates_.clear();
    }
    return true;
}

void HardwareBreakpointCoordinateRuntime::ObserveCandidate(
    std::uintptr_t candidate,
    std::uintptr_t x20,
    std::uintptr_t x21) noexcept {
    const std::uintptr_t normalizedX20 =
        x20 & kPointerPayloadMask;
    const std::uintptr_t normalizedX21 =
        x21 & kPointerPayloadMask;
    const bool distantContext =
        (candidate & ~kPageOffsetMask) !=
            (normalizedX20 & ~kPageOffsetMask);
    const auto recordSource =
        [&](CandidateObservation& observation) noexcept {
            if (normalizedX20 == 0 || normalizedX21 == 0) return;
            for (CandidateSource& source : observation.sources) {
                if (source.coordinateBase == normalizedX20 &&
                    source.mesh == normalizedX21) {
                    source.lastSeenPoll = pollCount_;
                    return;
                }
            }
            observation.sources[observation.sourceWriteIndex] = {
                normalizedX20,
                normalizedX21,
                pollCount_,
            };
            observation.sourceWriteIndex =
                (observation.sourceWriteIndex + 1) %
                    observation.sources.size();
            if (observation.sourceCount <
                observation.sources.size()) {
                ++observation.sourceCount;
            }
        };
    CandidateObservation* empty = nullptr;
    CandidateObservation* oldest = nullptr;
    for (CandidateObservation& observation : candidateObservations_) {
        if (observation.value == candidate) {
            observation.lastSeenPoll = pollCount_;
            if (observation.occurrences !=
                std::numeric_limits<std::uint64_t>::max()) {
                ++observation.occurrences;
            }
            if (distantContext &&
                observation.distantOccurrences !=
                    std::numeric_limits<std::uint64_t>::max()) {
                ++observation.distantOccurrences;
            }
            recordSource(observation);
            return;
        }
        if (observation.value == 0 && empty == nullptr) {
            empty = &observation;
        }
        if (observation.value != recordsBase_ &&
            observation.value != pendingRecordsBase_ &&
            observation.qualifiedStreak == 0 &&
            (oldest == nullptr ||
             observation.lastSeenPoll < oldest->lastSeenPoll)) {
            oldest = &observation;
        }
    }
    CandidateObservation* destination =
        empty != nullptr ? empty : oldest;
    if (destination == nullptr) return;
    *destination = {};
    destination->value = candidate;
    destination->lastSeenPoll = pollCount_;
    destination->occurrences = 1;
    destination->distantOccurrences = distantContext ? 1U : 0U;
    recordSource(*destination);
}

bool HardwareBreakpointCoordinateRuntime::ValidateCandidateSources(
    const CandidateObservation& observation,
    const CoordinateTableSnapshot& snapshot,
    CandidateSourceValidation& validation) noexcept {
    validation = {};
    validation.storedCount = observation.sourceCount;
    validation.closestDistance =
        std::numeric_limits<double>::infinity();
    std::array<std::uint32_t, 8> matchedIds{};

    for (const CandidateSource& source : observation.sources) {
        if (source.coordinateBase == 0 || source.mesh == 0 ||
            source.lastSeenPoll == 0) {
            continue;
        }
        if (pollCount_ < source.lastSeenPoll ||
            pollCount_ - source.lastSeenPoll >
                kCandidateSourceFreshPolls) {
            ++validation.expiredCount;
            continue;
        }
        ++validation.freshCount;

        std::uintptr_t coordinateAddress = 0;
        std::uintptr_t idAddress = 0;
        if (!AddOffset(
                source.coordinateBase,
                kMeshStreamCoordinateOffset,
                coordinateAddress) ||
            !AddOffset(
                source.mesh,
                kMeshStreamIdOffset,
                idAddress)) {
            ++validation.addressFailureCount;
            continue;
        }

        std::uint32_t firstId = 0;
        std::uint32_t secondId = 0;
        HardwareBreakpointCoordinate observed{};
        if (!ReadMemory(
                callbacks_, idAddress, &firstId, sizeof(firstId)) ||
            !ReadMemory(
                callbacks_,
                coordinateAddress,
                &observed,
                sizeof(observed)) ||
            !ReadMemory(
                callbacks_, idAddress, &secondId, sizeof(secondId))) {
            ++validation.readFailureCount;
            continue;
        }
        if (firstId == 0 || firstId != secondId ||
            !IsValidCoordinate(observed)) {
            ++validation.invalidCount;
            continue;
        }
        ++validation.usableCount;

        const auto candidate = snapshot.coordinates.find(firstId);
        if (candidate == snapshot.coordinates.end()) {
            ++validation.idMissCount;
            continue;
        }
        ++validation.presentCount;
        HardwareBreakpointCoordinate adjusted = observed;
        adjusted.z += kCoordinateZAdjustment;
        if (!std::isfinite(adjusted.z)) continue;
        const double dx =
            static_cast<double>(candidate->second.x) -
            static_cast<double>(adjusted.x);
        const double dy =
            static_cast<double>(candidate->second.y) -
            static_cast<double>(adjusted.y);
        const double dz =
            static_cast<double>(candidate->second.z) -
            static_cast<double>(adjusted.z);
        const double distance = std::hypot(dx, dy, dz);
        validation.closestDistance = std::min(
            validation.closestDistance, distance);
        if (distance > kCandidateSourceDistanceLimit) {
            ++validation.distanceFailureCount;
            continue;
        }
        ++validation.matchedCount;
        bool knownId = false;
        for (std::size_t index = 0;
             index < validation.distinctIdCount;
             ++index) {
            if (matchedIds[index] == firstId) {
                knownId = true;
                break;
            }
        }
        if (!knownId &&
            validation.distinctIdCount < matchedIds.size()) {
            matchedIds[validation.distinctIdCount++] = firstId;
        }
    }

    return validation.distinctIdCount >=
            kCandidateSourceMinimumDistinctIds &&
        validation.matchedCount >=
            kCandidateSourceMinimumDistinctIds &&
        validation.usableCount != 0 &&
        validation.matchedCount * 2U >=
            validation.usableCount;
}

bool HardwareBreakpointCoordinateRuntime::ReadObservedManager(
    std::uintptr_t targetRoot,
    std::uintptr_t& manager) noexcept {
    manager = 0;
    std::uintptr_t managerAddress = 0;
    std::uintptr_t first = 0;
    std::uintptr_t second = 0;
    if (UsesOrderedRecordTable(profile_)) {
        std::uintptr_t firstAddress = 0;
        std::uintptr_t secondAddress = 0;
        if (!AddOffset(targetRoot, kTargetLinkOffset, firstAddress)) {
            TraceOrderedManager(
                pollCount_, "first_address", targetRoot, 0, 0, 0, 0);
            return false;
        }
        if (!ReadMemory(
                callbacks_, firstAddress, &first, sizeof(first))) {
            TraceOrderedManager(
                pollCount_, "first_read", targetRoot, 0, 0, 0, 0);
            return false;
        }
        if (!AddOffset(first, kTargetLinkOffset, secondAddress)) {
            TraceOrderedManager(
                pollCount_, "second_address", targetRoot, first, 0, 0, 0);
            return false;
        }
        if (!ReadMemory(
                callbacks_, secondAddress, &second, sizeof(second))) {
            TraceOrderedManager(
                pollCount_, "second_read", targetRoot, first, 0, 0, 0);
            return false;
        }
        if (!AddOffset(second, kManagerOffset, managerAddress)) {
            TraceOrderedManager(
                pollCount_, "manager_address", targetRoot, first, second,
                0, 0);
            return false;
        }
    } else if (!AddOffset(targetRoot, kManagerOffset, managerAddress)) {
        return false;
    }

    std::uintptr_t rawManager = 0;
    if (!ReadMemory(
            callbacks_,
            managerAddress,
            &rawManager,
            sizeof(rawManager))) {
        if (UsesOrderedRecordTable(profile_)) {
            TraceOrderedManager(
                pollCount_, "manager_read", targetRoot, first, second,
                0, 0);
        }
        return false;
    }
    manager = rawManager;
    if (UsesOrderedRecordTable(profile_)) {
        if (!NormalizeObservedPointer(rawManager, manager)) {
            TraceOrderedManager(
                pollCount_, "manager_rejected", targetRoot, first, second,
                rawManager, manager);
            return false;
        }
        TraceOrderedManager(
            pollCount_, "ready", targetRoot, first, second,
            rawManager, manager);
        return true;
    }
    return manager != 0;
}

bool HardwareBreakpointCoordinateRuntime::RefreshCoordinateTable(
    std::uintptr_t world,
    std::uintptr_t targetRoot,
    std::uintptr_t manager) noexcept {
    if (UsesOrderedRecordTable(profile_) && !callbacks_.readCandidate) {
        return RefreshOrderedFallbackCoordinateTable(
            world, targetRoot, manager);
    }

    const std::uintptr_t selected = UsesOrderedRecordTable(profile_)
        ? pendingRecordsBase_
        : recordsBase_;
    if (selected == 0) return false;

    CoordinateTableSnapshot snapshot{};
    if (!ReadCoordinateTable(
            world,
            targetRoot,
            manager,
            selected,
            false,
            snapshot)) {
        return false;
    }
    return PublishCoordinateTable(
        world,
        targetRoot,
        manager,
        selected,
        std::move(snapshot));
}

bool HardwareBreakpointCoordinateRuntime::
RefreshOrderedFallbackCoordinateTable(
    std::uintptr_t world,
    std::uintptr_t targetRoot,
    std::uintptr_t manager) noexcept {
    try {
    const std::uintptr_t sampledPreferredBase =
        pendingRecordsBase_;
    const bool preferSampledBase =
        UsesOrderedRecordTable(profile_);
    if (recordsBase_ != 0 &&
        (publishedTargetRoot_ != targetRoot ||
         publishedManager_ != manager)) {
        ClearWorldState();
        return false;
    }

    const std::uintptr_t currentBase = recordsBase_;
    CandidateObservation* currentObservation = nullptr;
    for (CandidateObservation& observation : candidateObservations_) {
        if (observation.value == currentBase) {
            currentObservation = &observation;
            break;
        }
    }
    bool currentReady = false;
    if (currentBase != 0) {
        CoordinateTableSnapshot current{};
        const bool coherent = ReadCoordinateTable(
            world,
            targetRoot,
            manager,
            currentBase,
            true,
            current);
        if (coherent && current.idArray != 0) {
            if (samplingIdArray_ != 0 &&
                samplingIdArray_ != current.idArray) {
                ClearWorldState();
                samplingTargetRoot_ = targetRoot;
                samplingManager_ = manager;
                samplingIdArray_ = current.idArray;
                return false;
            }
            samplingIdArray_ = current.idArray;
        }
        if (coherent && current.qualified) {
            orderedRefreshFailureCount_ = 0;
            currentReady = PublishCoordinateTable(
                world,
                targetRoot,
                manager,
                currentBase,
                std::move(current));
            if (!currentReady) return false;
        } else if (coherent && publishedIdArray_ != 0 &&
                   current.idArray != publishedIdArray_) {
            ClearWorldState();
            return false;
        } else if (orderedRefreshFailureCount_ <
                   kOrderedRefreshFailureLimit) {
            ++orderedRefreshFailureCount_;
        }
    }

    std::vector<CandidateObservation*> eligible;
    eligible.reserve(candidateObservations_.size());
    for (CandidateObservation& observation : candidateObservations_) {
        if (observation.qualifiedStreak != 0 &&
            observation.lastEvaluatedPoll + 1 < pollCount_) {
            observation.qualifiedStreak = 0;
        }
        if (observation.value == 0 ||
            observation.value == currentBase) {
            continue;
        }
        if (currentReady) {
            if (UsesStableRecordTable(profile_)) {
                const bool newlyObserved =
                    observation.lastSeenPoll >
                    observation.lastEvaluatedPoll;
                if (!newlyObserved &&
                    observation.qualifiedStreak == 0) {
                    continue;
                }
                if (observation.qualifiedStreak == 0 &&
                    (pollCount_ % 30U) != 0) {
                    continue;
                }
            } else if (sampledPreferredBase == 0 ||
                sampledPreferredBase == currentBase ||
                observation.value != sampledPreferredBase) {
                continue;
            }
        }
        const bool neverEvaluated =
            observation.lastEvaluatedPoll == 0;
        const bool retestReady =
            pollCount_ >= observation.lastEvaluatedPoll &&
            pollCount_ - observation.lastEvaluatedPoll >=
                kFallbackCandidateRetestPolls;
        const bool newlyObserved =
            observation.lastSeenPoll > observation.lastEvaluatedPoll;
        const bool shortRetry =
            observation.failureStreak != 0 &&
            observation.failureStreak <=
                kFallbackCandidateShortRetryLimit &&
            observation.lastEvaluatedPoll + 1 == pollCount_;
        if (neverEvaluated || newlyObserved ||
            observation.qualifiedStreak != 0 || shortRetry ||
            retestReady) {
            eligible.push_back(&observation);
        }
    }
    std::sort(
        eligible.begin(),
        eligible.end(),
        [sampledPreferredBase, preferSampledBase](
            const CandidateObservation* left,
            const CandidateObservation* right) {
            const bool leftNew = left->lastEvaluatedPoll == 0;
            const bool rightNew = right->lastEvaluatedPoll == 0;
            const bool leftPending = left->qualifiedStreak != 0;
            const bool rightPending = right->qualifiedStreak != 0;
            if (leftPending != rightPending) return leftPending;
            if (preferSampledBase) {
                const bool leftPreferred =
                    left->value == sampledPreferredBase;
                const bool rightPreferred =
                    right->value == sampledPreferredBase;
                if (leftPreferred != rightPreferred) return leftPreferred;
            }
            if (leftNew != rightNew) return leftNew;
            if (left->lastEvaluatedPoll !=
                right->lastEvaluatedPoll) {
                return left->lastEvaluatedPoll <
                    right->lastEvaluatedPoll;
            }
            if (preferSampledBase) {
                const bool leftDistant =
                    left->distantOccurrences != 0;
                const bool rightDistant =
                    right->distantOccurrences != 0;
                if (leftDistant != rightDistant) return leftDistant;
            }
            if (left->lastSeenPoll != right->lastSeenPoll) {
                return left->lastSeenPoll > right->lastSeenPoll;
            }
            return left->occurrences > right->occurrences;
        });

    CandidateObservation* bestObservation = nullptr;
    CoordinateTableSnapshot bestSnapshot{};
    const std::size_t candidatesPerPoll =
        UsesStableRecordTable(profile_)
        ? kStableCandidatesPerPoll
        : kFallbackCandidatesPerPoll;
    const std::size_t evaluationCount = std::min(
        eligible.size(), candidatesPerPoll);
    for (std::size_t index = 0; index < evaluationCount; ++index) {
        CandidateObservation& observation = *eligible[index];
        const std::uint64_t previousEvaluationPoll =
            observation.lastEvaluatedPoll;
        observation.lastEvaluatedPoll = pollCount_;
        CoordinateTableSnapshot candidate{};
        const bool coherent = ReadCoordinateTable(
                world,
                targetRoot,
                manager,
                observation.value,
                true,
                candidate);
        if (coherent && candidate.qualified &&
            UsesStableRecordTable(profile_)) {
            CandidateSourceValidation source{};
            const bool sourceQualified =
                ValidateCandidateSources(
                    observation,
                    candidate,
                    source);
            candidate.qualified = sourceQualified;
            TraceSourceQuality(
                pollCount_,
                observation.value,
                source.storedCount,
                source.freshCount,
                source.expiredCount,
                source.addressFailureCount,
                source.readFailureCount,
                source.invalidCount,
                source.usableCount,
                source.idMissCount,
                source.presentCount,
                source.distanceFailureCount,
                source.matchedCount,
                source.distinctIdCount,
                source.closestDistance,
                sourceQualified);
        }
        if (coherent && candidate.idArray != 0) {
            if (samplingIdArray_ != 0 &&
                samplingIdArray_ != candidate.idArray) {
                ClearWorldState();
                samplingTargetRoot_ = targetRoot;
                samplingManager_ = manager;
                samplingIdArray_ = candidate.idArray;
                return false;
            }
            samplingIdArray_ = candidate.idArray;
        }
        if (!coherent || !candidate.qualified) {
            observation.qualifiedStreak = 0;
            if (observation.failureStreak <
                std::numeric_limits<std::uint8_t>::max()) {
                ++observation.failureStreak;
            }
            continue;
        }
        observation.failureStreak = 0;
        if (observation.qualifiedStreak != 0 &&
            previousEvaluationPoll + 1 != pollCount_) {
            observation.qualifiedStreak = 0;
        }
        if (observation.qualifiedStreak <
            std::numeric_limits<std::uint8_t>::max()) {
            ++observation.qualifiedStreak;
        }
        if (observation.qualifiedStreak <
            kFallbackCandidateQualificationCount) {
            continue;
        }
        const bool candidatePreferred =
            preferSampledBase && sampledPreferredBase != 0 &&
            observation.value == sampledPreferredBase;
        const bool bestPreferred =
            preferSampledBase && bestObservation != nullptr &&
            sampledPreferredBase != 0 &&
            bestObservation->value == sampledPreferredBase;
        bool better = bestObservation == nullptr;
        if (!better && candidatePreferred != bestPreferred) {
            better = candidatePreferred;
        } else if (!better && candidatePreferred == bestPreferred &&
                   candidate.evidenceCount >
                       bestSnapshot.evidenceCount) {
            better = true;
        } else if (!better && candidatePreferred == bestPreferred &&
                   candidate.evidenceCount ==
                       bestSnapshot.evidenceCount &&
                   candidate.plausibleCount >
                       bestSnapshot.plausibleCount) {
            better = true;
        } else if (!better && candidatePreferred == bestPreferred &&
                   candidate.evidenceCount ==
                       bestSnapshot.evidenceCount &&
                   candidate.plausibleCount ==
                       bestSnapshot.plausibleCount &&
                   candidate.distinctCount >
                       bestSnapshot.distinctCount) {
            better = true;
        } else if (!better && candidatePreferred == bestPreferred &&
                   candidate.evidenceCount ==
                       bestSnapshot.evidenceCount &&
                   candidate.plausibleCount ==
                       bestSnapshot.plausibleCount &&
                   candidate.distinctCount ==
                       bestSnapshot.distinctCount &&
                   observation.occurrences >
                       bestObservation->occurrences) {
            better = true;
        }
        if (better) {
            bestObservation = &observation;
            bestSnapshot = std::move(candidate);
        }
    }

    if (bestObservation != nullptr) {
        bool replaceCurrent = currentBase == 0;
        if (!replaceCurrent && !currentReady &&
            orderedRefreshFailureCount_ >=
                kOrderedRefreshFailureLimit) {
            replaceCurrent = true;
        }
        if (!replaceCurrent && currentReady &&
            !UsesStableRecordTable(profile_)) {
            const bool candidateIsSampledPreferred =
                sampledPreferredBase != 0 &&
                sampledPreferredBase != currentBase &&
                bestObservation->value == sampledPreferredBase;
            const bool candidateRecentlyObserved =
                pollCount_ >= bestObservation->lastSeenPoll &&
                pollCount_ - bestObservation->lastSeenPoll <
                    kFallbackPublishedCandidateStalePolls;
            const bool currentCandidateStale =
                currentObservation == nullptr ||
                (pollCount_ >= currentObservation->lastSeenPoll &&
                 pollCount_ - currentObservation->lastSeenPoll >=
                     kFallbackPublishedCandidateStalePolls);
            const bool candidateIsNewer =
                currentObservation == nullptr ||
                bestObservation->lastSeenPoll >
                    currentObservation->lastSeenPoll;
            replaceCurrent = candidateRecentlyObserved &&
                (candidateIsSampledPreferred ||
                 (currentCandidateStale && candidateIsNewer));
        } else if (currentReady &&
                   UsesStableRecordTable(profile_)) {
            const std::size_t evidenceMargin = std::max(
                kFallbackMinimumRequiredCoordinates,
                publishedEvidenceCount_ / 10U);
            const std::size_t distinctMargin = std::max(
                kFallbackMinimumRequiredCoordinates,
                publishedDistinctCount_ / 10U);
            replaceCurrent =
                bestSnapshot.evidenceCount >=
                    publishedEvidenceCount_ + evidenceMargin &&
                bestSnapshot.distinctCount >=
                    publishedDistinctCount_ + distinctMargin;
        }
        if (replaceCurrent) {
            orderedRefreshFailureCount_ = 0;
            for (CandidateObservation& observation :
                 candidateObservations_) {
                observation.qualifiedStreak = 0;
            }
            return PublishCoordinateTable(
                world,
                targetRoot,
                manager,
                bestObservation->value,
                std::move(bestSnapshot));
        }
        if (currentReady) {
            for (CandidateObservation* observation : eligible) {
                if (observation->qualifiedStreak >=
                    kFallbackCandidateQualificationCount) {
                    observation->qualifiedStreak = 0;
                }
            }
        }
    }

    if (!currentReady && currentBase != 0 &&
        orderedRefreshFailureCount_ >=
            kOrderedRefreshFailureLimit) {
        ClearPublishedState();
        for (CandidateObservation& observation :
             candidateObservations_) {
            if (observation.value == currentBase) {
                observation.lastEvaluatedPoll = 0;
                observation.qualifiedStreak = 0;
                break;
            }
        }
    }
    return currentReady;
    } catch (...) {
        return false;
    }
}

bool HardwareBreakpointCoordinateRuntime::ReadCoordinateTable(
    std::uintptr_t world,
    std::uintptr_t targetRoot,
    std::uintptr_t manager,
    std::uintptr_t recordsBase,
    bool requireFallbackConfidence,
    CoordinateTableSnapshot& snapshot) noexcept {
    snapshot = {};
    const bool orderedRecordTable = UsesOrderedRecordTable(profile_);
    std::uintptr_t idArrayAddress = 0;
    std::uintptr_t countAddress = 0;
    if (!AddOffset(manager, kIdArrayOffset, idArrayAddress) ||
        !AddOffset(manager, kCountOffset, countAddress)) {
        if (orderedRecordTable) {
            TraceOrderedTable(
                pollCount_, "layout_address", world, targetRoot, recordsBase,
                manager, 0, 0, 0, -1, 0, coordinates_.size());
        }
        return false;
    }

    if (orderedRecordTable &&
        (!IsObservedPointer(manager) ||
         !IsObservedPointer(recordsBase))) {
        TraceOrderedTable(
            pollCount_, "pointer_rejected", world, targetRoot, recordsBase,
            manager, 0, 0, 0, -1, 0, coordinates_.size());
        return false;
    }

    std::uintptr_t rawIdArray = 0;
    std::uint32_t rawCount = 0;
    if (!ReadMemory(
            callbacks_, idArrayAddress, &rawIdArray,
            sizeof(rawIdArray))) {
        if (orderedRecordTable) {
            TraceOrderedTable(
                pollCount_, "ids_pointer_read", world, targetRoot,
                recordsBase,
                manager, 0, 0, 0, -1, 0, coordinates_.size());
        }
        return false;
    }
    if (!ReadMemory(
            callbacks_, countAddress, &rawCount, sizeof(rawCount))) {
        if (orderedRecordTable) {
            TraceOrderedTable(
                pollCount_, "count_read", world, targetRoot, recordsBase,
                manager, rawIdArray, 0, 0, -1, 0,
                coordinates_.size());
        }
        return false;
    }

    std::uintptr_t idArray = rawIdArray;
    if (orderedRecordTable) {
        if (!NormalizeObservedPointer(rawIdArray, idArray)) {
            TraceOrderedTable(
                pollCount_, "ids_pointer_rejected", world, targetRoot,
                recordsBase, manager, rawIdArray, idArray, rawCount,
                -1, 0, coordinates_.size());
            return false;
        }
    } else if (idArray == 0) {
        return false;
    }
    const std::int64_t logicalCount =
        static_cast<std::int64_t>(
            static_cast<std::int32_t>(rawCount));
    if (logicalCount < kMinimumCoordinateCount ||
        logicalCount > kMaximumCoordinateCount) {
        if (orderedRecordTable) {
            TraceOrderedTable(
                pollCount_, "count_rejected", world, targetRoot, recordsBase,
                manager, rawIdArray, idArray, rawCount, logicalCount,
                0, coordinates_.size());
        }
        return false;
    }
    const std::int32_t count =
        static_cast<std::int32_t>(logicalCount);
    const std::size_t itemCount = static_cast<std::size_t>(count);
    const std::size_t recordsContainerSize =
        itemCount * kCoordinateRecordStride;
    const std::size_t recordsReadSize = recordsContainerSize;
    const std::size_t idsSize = itemCount * sizeof(std::uint32_t);
    if (!IsReadableRange(recordsBase, recordsReadSize) ||
        !IsReadableRange(idArray, idsSize)) {
        if (orderedRecordTable) {
            TraceOrderedTable(
                pollCount_, "bulk_range", world, targetRoot, recordsBase,
                manager, rawIdArray, idArray, rawCount, logicalCount,
                0, coordinates_.size());
        }
        return false;
    }

    try {
        std::vector<std::uint8_t> records(recordsContainerSize);
        std::vector<std::uint32_t> ids(itemCount);
        if (!ReadMemory(
                callbacks_, recordsBase, records.data(),
                recordsReadSize)) {
            if (orderedRecordTable) {
                TraceOrderedTable(
                    pollCount_, "records_read", world, targetRoot,
                    recordsBase,
                    manager, rawIdArray, idArray, rawCount,
                    logicalCount, 0, coordinates_.size());
            }
            return false;
        }
        if (!ReadMemory(callbacks_, idArray, ids.data(), idsSize)) {
            if (orderedRecordTable) {
                TraceOrderedTable(
                    pollCount_, "ids_read", world, targetRoot, recordsBase,
                    manager, rawIdArray, idArray, rawCount,
                    logicalCount, 0, coordinates_.size());
            }
            return false;
        }

        std::uintptr_t verifiedManager = 0;
        std::uintptr_t rawVerifiedIdArray = 0;
        std::uint32_t verifiedRawCount = 0;
        std::vector<std::uint32_t> verifiedIds;
        std::vector<std::uint8_t> verifiedRecords;
        if (orderedRecordTable || requireFallbackConfidence) {
            verifiedIds.resize(itemCount);
        }
        if (orderedRecordTable || requireFallbackConfidence) {
            verifiedRecords.resize(recordsContainerSize);
        }
        if (((orderedRecordTable || requireFallbackConfidence) &&
             !ReadMemory(
                 callbacks_,
                 recordsBase,
                 verifiedRecords.data(),
                 recordsReadSize)) ||
            !ReadObservedManager(targetRoot, verifiedManager) ||
            !ReadMemory(
                callbacks_, idArrayAddress, &rawVerifiedIdArray,
                sizeof(rawVerifiedIdArray)) ||
            !ReadMemory(
                callbacks_, countAddress, &verifiedRawCount,
                sizeof(verifiedRawCount)) ||
            ((orderedRecordTable || requireFallbackConfidence) &&
             !ReadMemory(
                 callbacks_, idArray, verifiedIds.data(), idsSize))) {
            if (orderedRecordTable) {
                TraceOrderedTable(
                    pollCount_, "verification_read", world, targetRoot,
                    recordsBase, manager, rawIdArray, idArray,
                    rawCount, logicalCount, 0, coordinates_.size());
            }
            return false;
        }
        std::uintptr_t verifiedIdArray = rawVerifiedIdArray;
        if (orderedRecordTable &&
            !NormalizeObservedPointer(
                rawVerifiedIdArray, verifiedIdArray)) {
            return false;
        }
        if (verifiedManager != manager ||
            verifiedIdArray != idArray ||
            verifiedRawCount != rawCount ||
            ((orderedRecordTable || requireFallbackConfidence) &&
             verifiedIds != ids)) {
            if (orderedRecordTable) {
                TraceOrderedTable(
                    pollCount_, "verification_changed", world, targetRoot,
                    recordsBase, verifiedManager, rawVerifiedIdArray,
                    verifiedIdArray, verifiedRawCount, logicalCount, 0,
                    coordinates_.size());
            }
            return false;
        }

        std::unordered_set<std::uint32_t> nonzeroIds;
        nonzeroIds.reserve(itemCount);
        std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
            exactCandidates;
        std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
            plausibleCandidates;
        std::unordered_set<std::uint32_t> strongCandidateIds;
        auto decodeRecords =
            [&](const std::vector<std::uint8_t>& source,
                std::unordered_map<
                    std::uint32_t,
                    HardwareBreakpointCoordinate>& exact,
                std::unordered_map<
                    std::uint32_t,
                    HardwareBreakpointCoordinate>& plausible,
                std::unordered_set<std::uint32_t>& strongIds) {
                exact.clear();
                plausible.clear();
                strongIds.clear();
                exact.reserve(itemCount);
                plausible.reserve(itemCount);
                strongIds.reserve(itemCount);
                for (std::size_t index = 0;
                     index < itemCount;
                     ++index) {
                    const std::uint32_t id = ids[index];
                    if (id == 0) continue;
                    const std::size_t recordOffset =
                        index * kCoordinateRecordStride;
                    HardwareBreakpointCoordinate input{};
                    std::memcpy(
                        &input.x,
                        source.data() + recordOffset +
                            kCoordinateXOffset,
                        sizeof(input.x));
                    std::memcpy(
                        &input.y,
                        source.data() + recordOffset +
                            kCoordinateYOffset,
                        sizeof(input.y));
                    std::memcpy(
                        &input.z,
                        source.data() + recordOffset +
                            kCoordinateZOffset,
                        sizeof(input.z));
                    if (!IsValidCoordinate(input)) continue;
                    HardwareBreakpointCoordinate output = input;
                    output.z += kCoordinateZAdjustment;
                    if (!std::isfinite(output.z)) continue;
                    exact[id] = output;
                    if (!requireFallbackConfidence ||
                        !IsPlausibleFallbackCoordinate(input, output)) {
                        continue;
                    }
                    plausible[id] = output;
                    if (IsStrongFallbackCoordinate(input)) {
                        strongIds.insert(id);
                    } else {
                        strongIds.erase(id);
                    }
                }
            };

        for (const std::uint32_t id : ids) {
            if (id != 0) nonzeroIds.insert(id);
        }
        decodeRecords(
            records,
            exactCandidates,
            plausibleCandidates,
            strongCandidateIds);

        snapshot.rawIdArray = rawIdArray;
        snapshot.idArray = idArray;
        snapshot.rawCount = rawCount;
        snapshot.logicalCount = logicalCount;
        snapshot.nonzeroIdCount = nonzeroIds.size();
        snapshot.exactValidCount = exactCandidates.size();

        if (requireFallbackConfidence) {
            std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
                firstPlausible = std::move(plausibleCandidates);
            std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
                verifiedExact;
            decodeRecords(
                verifiedRecords,
                verifiedExact,
                plausibleCandidates,
                strongCandidateIds);
            snapshot.exactValidCount = verifiedExact.size();

            std::size_t overlapCount = 0;
            std::size_t stableCount = 0;
            std::unordered_set<std::uint32_t> stableCandidateIds;
            stableCandidateIds.reserve(plausibleCandidates.size());
            for (const auto& entry : plausibleCandidates) {
                const auto previous = firstPlausible.find(entry.first);
                if (previous == firstPlausible.end()) continue;
                ++overlapCount;
                const double dx = static_cast<double>(entry.second.x) -
                    static_cast<double>(previous->second.x);
                const double dy = static_cast<double>(entry.second.y) -
                    static_cast<double>(previous->second.y);
                const double dz = static_cast<double>(entry.second.z) -
                    static_cast<double>(previous->second.z);
                if (std::hypot(dx, dy, dz) <= 500.0) {
                    ++stableCount;
                    stableCandidateIds.insert(entry.first);
                }
            }
            const std::size_t maximumPlausible = std::max(
                firstPlausible.size(), plausibleCandidates.size());
            const bool coherentSnapshots =
                maximumPlausible != 0 &&
                overlapCount * 100U >= maximumPlausible * 90U &&
                stableCount * 100U >= overlapCount * 95U;

            for (auto iterator = plausibleCandidates.begin();
                 iterator != plausibleCandidates.end();) {
                if (stableCandidateIds.find(iterator->first) ==
                    stableCandidateIds.end()) {
                    strongCandidateIds.erase(iterator->first);
                    iterator = plausibleCandidates.erase(iterator);
                } else {
                    ++iterator;
                }
            }

            std::unordered_map<CoordinateBits, std::size_t,
                               CoordinateBitsHash>
                coordinateFrequencies;
            coordinateFrequencies.reserve(plausibleCandidates.size());
            for (const auto& entry : plausibleCandidates) {
                ++coordinateFrequencies[GetCoordinateBits(entry.second)];
            }
            for (auto iterator = plausibleCandidates.begin();
                 iterator != plausibleCandidates.end();) {
                const auto frequency = coordinateFrequencies.find(
                    GetCoordinateBits(iterator->second));
                if (frequency != coordinateFrequencies.end() &&
                    frequency->second >= 4) {
                    strongCandidateIds.erase(iterator->first);
                    iterator = plausibleCandidates.erase(iterator);
                } else {
                    ++iterator;
                }
            }

            std::unordered_set<CoordinateBits, CoordinateBitsHash>
                distinctCoordinates;
            distinctCoordinates.reserve(plausibleCandidates.size());
            for (const auto& entry : plausibleCandidates) {
                distinctCoordinates.insert(
                    GetCoordinateBits(entry.second));
                if (strongCandidateIds.find(entry.first) !=
                    strongCandidateIds.end()) {
                    ++snapshot.evidenceCount;
                }
            }
            snapshot.plausibleCount = plausibleCandidates.size();
            snapshot.distinctCount = distinctCoordinates.size();
            snapshot.requiredCount = std::max(
                kFallbackMinimumRequiredCoordinates,
                (itemCount + 49U) / 50U);
            const std::size_t requiredDistinct = std::min(
                snapshot.requiredCount,
                kFallbackMinimumRequiredCoordinates);
            snapshot.qualified =
                coherentSnapshots &&
                snapshot.plausibleCount >= snapshot.requiredCount &&
                snapshot.evidenceCount >= snapshot.requiredCount &&
                snapshot.distinctCount >= requiredDistinct &&
                snapshot.plausibleCount * 100U >=
                    snapshot.exactValidCount *
                        kFallbackMinimumPlausiblePercent;
            snapshot.coordinates = std::move(plausibleCandidates);
            TraceOrderedQuality(
                pollCount_,
                recordsBase,
                snapshot.nonzeroIdCount,
                snapshot.exactValidCount,
                snapshot.plausibleCount,
                snapshot.evidenceCount,
                snapshot.distinctCount,
                snapshot.requiredCount,
                snapshot.qualified);
        } else if (orderedRecordTable) {
            std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
                verifiedExact;
            std::unordered_map<std::uint32_t, HardwareBreakpointCoordinate>
                unusedPlausible;
            std::unordered_set<std::uint32_t> unusedStrongIds;
            decodeRecords(
                verifiedRecords,
                verifiedExact,
                unusedPlausible,
                unusedStrongIds);
            bool stable = true;
            for (const std::uint32_t id : ids) {
                if (id == 0) continue;
                const auto first = exactCandidates.find(id);
                const auto second = verifiedExact.find(id);
                if ((first == exactCandidates.end()) !=
                    (second == verifiedExact.end())) {
                    stable = false;
                    break;
                }
                if (first == exactCandidates.end()) continue;
                const double dx =
                    static_cast<double>(second->second.x) -
                    static_cast<double>(first->second.x);
                const double dy =
                    static_cast<double>(second->second.y) -
                    static_cast<double>(first->second.y);
                const double dz =
                    static_cast<double>(second->second.z) -
                    static_cast<double>(first->second.z);
                if (std::hypot(dx, dy, dz) > 500.0) {
                    stable = false;
                    break;
                }
            }
            if (!stable) {
                TraceOrderedTable(
                    pollCount_, "records_changed", world, targetRoot,
                    recordsBase, manager, rawIdArray, idArray, rawCount,
                    logicalCount, 0, coordinates_.size());
                return false;
            }
            snapshot.coordinates = std::move(verifiedExact);
            snapshot.plausibleCount = snapshot.coordinates.size();
            snapshot.evidenceCount = snapshot.coordinates.size();
            snapshot.distinctCount = snapshot.coordinates.size();
            snapshot.qualified = true;
        } else {
            snapshot.coordinates = std::move(exactCandidates);
            snapshot.plausibleCount = snapshot.coordinates.size();
            snapshot.evidenceCount = snapshot.coordinates.size();
            snapshot.distinctCount = snapshot.coordinates.size();
            snapshot.qualified = true;
        }

        if (world != world_) return false;
        return true;
    } catch (...) {
        return false;
    }
}

bool HardwareBreakpointCoordinateRuntime::PublishCoordinateTable(
    std::uintptr_t world,
    std::uintptr_t targetRoot,
    std::uintptr_t manager,
    std::uintptr_t recordsBase,
    CoordinateTableSnapshot snapshot) noexcept {
    if (world != world_ || recordsBase == 0 || !snapshot.qualified) {
        return false;
    }

    try {
        const bool orderedRecordTable =
            UsesOrderedRecordTable(profile_);
        const bool baseChanged =
            publishedRecordsBase_ != recordsBase;
        auto next = std::move(snapshot.coordinates);

        if (world != world_) return false;
        coordinates_.swap(next);
        recordsBase_ = recordsBase;
        pendingRecordsBase_ = recordsBase;
        publishedRecordsBase_ = recordsBase;
        publishedTargetRoot_ = targetRoot;
        publishedManager_ = manager;
        publishedIdArray_ = snapshot.idArray;
        publishedEvidenceCount_ = snapshot.evidenceCount;
        publishedDistinctCount_ = snapshot.distinctCount;
        if (orderedRecordTable) {
            samplingTargetRoot_ = targetRoot;
            samplingManager_ = manager;
            samplingIdArray_ = snapshot.idArray;
        }
        orderedRefreshFailureCount_ = 0;
        if (UsesOrderedRecordTable(profile_) ||
            UsesStableRecordTable(profile_)) {
            TraceOrderedTable(
                pollCount_, "published", world, targetRoot, recordsBase,
                manager, snapshot.rawIdArray, snapshot.idArray,
                snapshot.rawCount, snapshot.logicalCount,
                snapshot.plausibleCount, coordinates_.size(),
                baseChanged);
        }
        return true;
    } catch (...) {
        return false;
    }
}

void HardwareBreakpointCoordinateRuntime::ClearPublishedState() noexcept {
    coordinates_.clear();
    meshStreamCoordinates_.clear();
    meshStreamIds_.clear();
    recordsBase_ = 0;
    publishedRecordsBase_ = 0;
    publishedTargetRoot_ = 0;
    publishedManager_ = 0;
    publishedIdArray_ = 0;
    publishedEvidenceCount_ = 0;
    publishedDistinctCount_ = 0;
    orderedRefreshFailureCount_ = 0;
}

void HardwareBreakpointCoordinateRuntime::ClearConfigurationState() noexcept {
    records_.fill({});
    lastTotalRecords_ = 0;
    lastHitAddress_ = 0;
}

void HardwareBreakpointCoordinateRuntime::ClearWorldState() noexcept {
    seenRecords_.fill({});
    candidateRing_.fill(0);
    candidateObservations_.fill({});
    ClearPublishedState();
    pendingRecordsBase_ = 0;
    samplingTargetRoot_ = 0;
    samplingManager_ = 0;
    samplingIdArray_ = 0;
    candidateWriteIndex_ = 0;
    candidateCount_ = 0;
    lastTotalRecords_ = 0;
    lastHitAddress_ = 0;
}

void HardwareBreakpointCoordinateRuntime::ClearSamplingState() noexcept {
    records_.fill({});
    ClearWorldState();
    world_ = 0;
    pollCount_ = 0;
    acceptedSampleCount_ = 0;
    needsReconfigure_ = false;
}

}  // namespace lengjing::game::native
