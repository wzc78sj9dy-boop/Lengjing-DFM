#ifndef LENGJING_ENABLE_ALGORITHM_COORDINATE
#define LENGJING_ENABLE_ALGORITHM_COORDINATE 0
#endif

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
#error "Algorithm coordinate feature is temporarily disabled"
#endif

#include "app/AppController.h"
#include "app/RenderBackendSelection.h"
#include "app/RuntimeExitPolicy.h"
#include "app/RuntimePresentationPolicy.h"
#include "auth/CloudLayoutStartupPolicy.h"
#include "auth/RemoteAuth.h"
#include "game/native/MemoryTransport.h"
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
#include "game/native/AlgorithmCoordinateProbePolicy.h"
#endif
#include "platform/BackgroundProcess.h"
#include "platform/MenuKeyMonitor.h"
#include "platform/PerformanceTrace.h"

#include "Android_Graphics/GraphicsManager.h"
#include "Android_draw/字体.h"
#include "Android_touch/TouchHelperA.h"
#include "ImGui/imgui.h"
#include "native_surface/ANativeWindowCreator.h"
#include "ui/MenuLogoData.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#ifndef LENGJING_VERSION
#define LENGJING_VERSION "dev"
#endif

#ifndef LENGJING_AUTH_VERSION
#define LENGJING_AUTH_VERSION "1000"
#endif

#ifndef LENGJING_ENABLE_RUNTIME_AUTH
#define LENGJING_ENABLE_RUNTIME_AUTH 0
#endif

#ifndef LENGJING_ENABLE_COORDINATE_DEBUG_LOG
#define LENGJING_ENABLE_COORDINATE_DEBUG_LOG 0
#endif

namespace {

std::atomic_bool gStopRequested{false};
constexpr bool kRuntimeAuthEnabled =
    LENGJING_ENABLE_RUNTIME_AUTH != 0;

void HandleSignal(int) {
    gStopRequested.store(true, std::memory_order_release);
}

std::string CurrentDirectory() {
    char path[1024]{};
    return getcwd(path, sizeof(path)) != nullptr
        ? std::string(path)
        : std::string("/data/local/tmp");
}

std::string LocalConfigPath(const std::string& programDirectory) {
#if defined(__ANDROID__)
    static_cast<void>(programDirectory);
    return "/data/adb/lengjing.json";
#else
    return programDirectory + "/lengjing.json";
#endif
}

lengjing::game::native::AlgorithmPositionRuntimeConfig
CoordinateReplayConfiguration() {
    if (const char* guestPc =
            std::getenv("LENGJING_COORDINATE_DECRYPT_PC")) {
        return lengjing::game::native::ParseAlgorithmPositionGuestPc(
            guestPc);
    }
    if (const char* decryptRva =
            std::getenv("LENGJING_COORDINATE_DECRYPT_RVA")) {
        return lengjing::game::native::ParseAlgorithmPositionDecryptRva(
            decryptRva);
    }
    return {};
}

int ProbeSeconds(const char* environmentName) {
    const char* value = std::getenv(environmentName);
    if (value == nullptr || value[0] == '\0') return 0;
    int seconds = 0;
    const char* end = value + std::char_traits<char>::length(value);
    const auto parsed = std::from_chars(value, end, seconds, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end &&
            seconds >= 1 && seconds <= 120
        ? seconds
        : 0;
}

int CoordinateProbeSeconds() {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    return ProbeSeconds("LENGJING_COORDINATE_PROBE_SECONDS");
#else
    return 0;
#endif
}

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
int AlgorithmCoordinateProbeSeconds() {
    return ProbeSeconds("LENGJING_ALGORITHM_PROBE_SECONDS");
}
#endif

int CoordinateProbeDriver() {
    const char* value = std::getenv("LENGJING_COORDINATE_PROBE_DRIVER");
    if (value == nullptr || value[0] == '\0') return 1;
    int driver = -1;
    const char* end = value + std::char_traits<char>::length(value);
    const auto parsed = std::from_chars(value, end, driver, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end &&
            lengjing::game::native::IsValidMemoryTransportMode(driver)
        ? driver
        : 1;
}

int RunCoordinateProbe(
    int seconds,
    int width,
    int height,
    const std::string& programDirectory,
    std::shared_ptr<const lengjing::auth::CloudLayoutDocument> cloudLayout,
    const lengjing::game::native::AlgorithmPositionRuntimeConfig&
        algorithmPosition) {
    using namespace std::chrono_literals;
    const bool cloudLayoutActive = cloudLayout != nullptr;
    lengjing::game::RuntimeOptions options;
    options.gameVersionIndex = 0;
    options.driverIndex = CoordinateProbeDriver();
    options.inputMode = lengjing::ui::AimInputMode::ReadOnly;
    options.screenWidth = width;
    options.screenHeight = height;
    options.programDirectory = programDirectory;
    options.cloudLayout = std::move(cloudLayout);
    options.algorithmPosition = algorithmPosition;

    lengjing::game::FeatureSettings settings;
    settings.visual.coordinateDecrypt = true;
    lengjing::game::GameRuntime runtime(
        lengjing::game::CreateNativeGameBackend());
    runtime.UpdateSettings(settings);
    if (!runtime.Start(options)) {
        const std::string diagnostic =
            lengjing::game::FormatRuntimeDiagnostic(
                lengjing::game::RuntimeError::StartRejected,
                -EBUSY);
        std::fprintf(stderr, "%s\n", diagnostic.c_str());
        return 10;
    }

    lengjing::game::RuntimeStatus last{};
    lengjing::game::CoordinateDecryptError lastReportedError =
        lengjing::game::CoordinateDecryptError::None;
    int lastReportedSystemError = 0;
    lengjing::game::CoordinateReadDiagnostic lastReportedRead{};
    lengjing::game::CoordinatePoolPointerDiagnostic
        lastReportedPoolPointer{};
    lengjing::game::CoordinateEntryDiagnostic lastReportedEntry{};
    lengjing::game::RuntimeError lastReportedRuntimeError =
        lengjing::game::RuntimeError::None;
    int lastReportedRuntimeSystemError = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const lengjing::game::RuntimeStatus status = runtime.Status();
        if (status.coordinateError !=
                lengjing::game::CoordinateDecryptError::None &&
            (status.coordinateError != lastReportedError ||
             status.coordinateSystemError != lastReportedSystemError ||
             status.coordinateRead != lastReportedRead ||
             status.coordinatePoolPointer != lastReportedPoolPointer ||
             status.coordinateEntry != lastReportedEntry)) {
            const std::string diagnostic =
                lengjing::game::FormatCoordinateDecryptDiagnostic(
                    status.coordinateError,
                    status.coordinateSystemError,
                    status.coordinateRead,
                    status.coordinatePoolPointer,
                    status.coordinateEntry);
            std::fprintf(stderr, "%s\n", diagnostic.c_str());
            lastReportedError = status.coordinateError;
            lastReportedSystemError = status.coordinateSystemError;
            lastReportedRead = status.coordinateRead;
            lastReportedPoolPointer = status.coordinatePoolPointer;
            lastReportedEntry = status.coordinateEntry;
        } else if (status.coordinateError ==
                   lengjing::game::CoordinateDecryptError::None) {
            lastReportedError =
                lengjing::game::CoordinateDecryptError::None;
            lastReportedSystemError = 0;
            lastReportedRead = {};
            lastReportedPoolPointer = {};
            lastReportedEntry = {};
        }
        if (status.runtimeError != lengjing::game::RuntimeError::None &&
            (status.runtimeError != lastReportedRuntimeError ||
             status.runtimeSystemError != lastReportedRuntimeSystemError)) {
            const std::string diagnostic =
                lengjing::game::FormatRuntimeDiagnostic(
                    status.runtimeError,
                    status.runtimeSystemError);
            std::fprintf(stderr, "%s\n", diagnostic.c_str());
            lastReportedRuntimeError = status.runtimeError;
            lastReportedRuntimeSystemError = status.runtimeSystemError;
        } else if (status.runtimeError ==
                   lengjing::game::RuntimeError::None) {
            lastReportedRuntimeError = lengjing::game::RuntimeError::None;
            lastReportedRuntimeSystemError = 0;
        }
        last = status;
        if (status.phase == lengjing::game::RuntimePhase::Faulted) break;
        std::this_thread::sleep_for(100ms);
    }
    runtime.Stop();
    runtime.WaitUntilStopped();
    const int runtimeExitCode = lengjing::app::ResolveRuntimeExitCode(
        cloudLayoutActive, last.phase, last.failureKind);
    if (runtimeExitCode != 0) return runtimeExitCode;
    return last.coordinateSuccesses != 0
        ? 0
        : (last.coordinateContextReady
            ? 13
            : (last.coordinateEntryReady ? 12 : 11));
}

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
bool SameRuntimeCodecProbeSummary(
    const lengjing::game::native::RuntimeCoordinateCodecDiagnostic& left,
    const lengjing::game::native::RuntimeCoordinateCodecDiagnostic& right) {
    return left.stage == right.stage && left.error == right.error &&
        left.hook == right.hook && left.trampoline == right.trampoline &&
        left.callback == right.callback && left.context == right.context &&
        left.state == right.state && left.config == right.config &&
        left.divisor == right.divisor && left.mask == right.mask &&
        left.zBias == right.zBias &&
        left.fingerprintWindow == right.fingerprintWindow &&
        left.expectedFingerprint == right.expectedFingerprint &&
        left.observedFingerprint == right.observedFingerprint;
}

int RunAlgorithmCoordinateProbe(
    int seconds,
    int width,
    int height,
    const std::string& programDirectory,
    std::shared_ptr<const lengjing::auth::CloudLayoutDocument> cloudLayout) {
    using namespace std::chrono_literals;
    const bool cloudLayoutActive = cloudLayout != nullptr;
    lengjing::game::RuntimeOptions options;
    options.gameVersionIndex = 0;
    options.driverIndex = CoordinateProbeDriver();
    options.inputMode = lengjing::ui::AimInputMode::ReadOnly;
    options.screenWidth = width;
    options.screenHeight = height;
    options.programDirectory = programDirectory;
    options.cloudLayout = std::move(cloudLayout);

    lengjing::game::FeatureSettings settings;
    settings.visual.coordinateDecrypt = false;
    settings.visual.algorithmDecrypt = true;
    lengjing::game::GameRuntime runtime(
        lengjing::game::CreateNativeGameBackend());
    runtime.UpdateSettings(settings);
    if (!runtime.Start(options)) {
        const std::string diagnostic =
            lengjing::game::FormatRuntimeDiagnostic(
                lengjing::game::RuntimeError::StartRejected,
                -EBUSY);
        std::fprintf(stderr, "%s\n", diagnostic.c_str());
        return 10;
    }

    lengjing::game::RuntimeStatus last{};
    lengjing::game::native::RuntimeCoordinateCodecDiagnostic
        lastReportedRuntime{};
    lengjing::game::native::RuntimeCoordinateCodecDiagnostic
        successfulRuntime{};
    bool reportedRuntimeReady = false;
    bool requested = false;
    bool active = false;
    bool runtimeReady = false;
    bool tableReady = false;
    std::uint64_t refreshes = 0;
    std::uint64_t resolveAttempts = 0;
    std::uint64_t resolveSuccesses = 0;
    std::uint64_t attempts = 0;
    std::uint64_t successes = 0;
    std::uint64_t objectAttempts = 0;
    std::uint64_t objectSuccesses = 0;
    std::uint64_t tableAttempts = 0;
    std::uint64_t tableSuccesses = 0;
    std::uint64_t fallbacks = 0;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const lengjing::game::RuntimeStatus status = runtime.Status();
        requested = requested || status.algorithmCoordinateRequested;
        active = active || status.algorithmCoordinateActive;
        runtimeReady = runtimeReady || status.algorithmCoordinateRuntimeReady;
        tableReady = tableReady || status.algorithmCoordinateTableReady;
        refreshes = std::max(
            refreshes, status.algorithmCoordinateRefreshes);
        resolveAttempts = std::max(
            resolveAttempts, status.algorithmCoordinateResolveAttempts);
        resolveSuccesses = std::max(
            resolveSuccesses, status.algorithmCoordinateResolveSuccesses);
        attempts = std::max(
            attempts, status.algorithmCoordinateAttempts);
        successes = std::max(
            successes, status.algorithmCoordinateSuccesses);
        objectAttempts = std::max(
            objectAttempts, status.algorithmCoordinateObjectAttempts);
        objectSuccesses = std::max(
            objectSuccesses, status.algorithmCoordinateObjectSuccesses);
        tableAttempts = std::max(
            tableAttempts, status.algorithmCoordinateTableAttempts);
        tableSuccesses = std::max(
            tableSuccesses, status.algorithmCoordinateTableSuccesses);
        fallbacks = std::max(
            fallbacks, status.algorithmCoordinateFallbacks);

        if (status.algorithmCoordinateRuntime.error !=
                lengjing::game::native::
                    RuntimeCoordinateCodecError::None &&
            !SameRuntimeCodecProbeSummary(
                status.algorithmCoordinateRuntime,
                lastReportedRuntime)) {
            const std::string diagnostic =
                lengjing::game::native::
                    FormatRuntimeCoordinateCodecDiagnostic(
                        status.algorithmCoordinateRuntime);
            std::fprintf(stderr, "%s\n", diagnostic.c_str());
            lastReportedRuntime = status.algorithmCoordinateRuntime;
        }
        if (status.algorithmCoordinateRuntimeReady &&
            !reportedRuntimeReady) {
            reportedRuntimeReady = true;
            const std::string diagnostic =
                lengjing::game::native::
                    FormatRuntimeCoordinateCodecDiagnostic(
                        status.algorithmCoordinateRuntime);
            std::fprintf(stderr, "%s\n", diagnostic.c_str());
        }
        if (status.algorithmCoordinateObjectSuccesses != 0 &&
            lengjing::game::native::
                IsAlgorithmCoordinateObjectSampleValid(
                    status.algorithmCoordinateRequested,
                    status.algorithmCoordinateActive,
                    status.algorithmCoordinateObjectSuccesses,
                    status.algorithmCoordinateFallbacks,
                    status.algorithmCoordinateRuntime)) {
            successfulRuntime = status.algorithmCoordinateRuntime;
        }
        last = status;
        if (status.phase == lengjing::game::RuntimePhase::Faulted) break;
        std::this_thread::sleep_for(100ms);
    }
    runtime.Stop();
    runtime.WaitUntilStopped();

    std::fprintf(
        stderr,
        "[algorithm-coordinate-probe] requested=%d active=%d "
        "runtime_ready=%d table_ready=%d refreshes=%llu "
        "resolve_attempts=%llu resolve_successes=%llu attempts=%llu "
        "successes=%llu object_attempts=%llu object_successes=%llu "
        "table_attempts=%llu table_successes=%llu fallbacks=%llu "
        "runtime_error=%u table_error=%u table=%llX records=%llX "
        "count=%u valid=%u object=%llX token=%llX "
        "raw=(%.3f,%.3f,%.3f) v_adjust=(%.3f,%.3f) "
        "base_z=%.3f visual_acceptance=%d\n",
        requested ? 1 : 0,
        active ? 1 : 0,
        runtimeReady ? 1 : 0,
        tableReady ? 1 : 0,
        static_cast<unsigned long long>(refreshes),
        static_cast<unsigned long long>(resolveAttempts),
        static_cast<unsigned long long>(resolveSuccesses),
        static_cast<unsigned long long>(attempts),
        static_cast<unsigned long long>(successes),
        static_cast<unsigned long long>(objectAttempts),
        static_cast<unsigned long long>(objectSuccesses),
        static_cast<unsigned long long>(tableAttempts),
        static_cast<unsigned long long>(tableSuccesses),
        static_cast<unsigned long long>(fallbacks),
        static_cast<unsigned>(
            lengjing::game::native::RuntimeCoordinateCodecErrorCode(
                last.algorithmCoordinateRuntime.error)),
        static_cast<unsigned>(
            lengjing::game::native::AlgorithmCoordinateReadErrorCode(
                last.algorithmCoordinate.error)),
        static_cast<unsigned long long>(last.algorithmCoordinate.table),
        static_cast<unsigned long long>(last.algorithmCoordinate.records),
        static_cast<unsigned>(last.algorithmCoordinate.count),
        static_cast<unsigned>(last.algorithmCoordinate.validCount),
        static_cast<unsigned long long>(successfulRuntime.object),
        static_cast<unsigned long long>(successfulRuntime.token),
        successfulRuntime.decodedX,
        successfulRuntime.decodedY,
        successfulRuntime.decodedZ,
        successfulRuntime.verticalAdjustmentFirst,
        successfulRuntime.verticalAdjustmentSecond,
        successfulRuntime.presentedZ,
        lengjing::game::native::
            kAlgorithmCoordinateVisualAcceptanceCompleted ? 1 : 0);
    std::fflush(stderr);

    const int runtimeExitCode = lengjing::app::ResolveRuntimeExitCode(
        cloudLayoutActive, last.phase, last.failureKind);
    if (runtimeExitCode != 0) return runtimeExitCode;
    if (lengjing::game::native::
            IsAlgorithmCoordinateObjectProbeSuccessful(
                requested,
                active,
                objectSuccesses,
                fallbacks,
                successfulRuntime)) {
        return 0;
    }
    if (!requested || !active || refreshes == 0) return 11;
    if (!runtimeReady || resolveSuccesses == 0) return 12;
    return 13;
}
#endif

struct CloudLayoutFetchResult {
    std::shared_ptr<const lengjing::auth::CloudLayoutDocument> snapshot;
    bool continueStartup = false;
};

CloudLayoutFetchResult FetchAuthenticatedCloudLayout(
    lengjing::auth::AuthSession& session) {
    const lengjing::auth::T3AuthConfig& config =
        lengjing::auth::kDefaultT3AuthConfig;
    const lengjing::auth::CloudRuntimeIdentity identity =
        lengjing::auth::ResolveCloudRuntimeIdentity(config);
    const bool hasAnyCloudVariableValue =
        config.cloudVariable.HasAnyValue();
    const bool configurationComplete =
        config.cloudVariable.IsConfigured() &&
        config.cloudIdentity.IsConfigured() && identity.IsValid();
    const auto initialAction =
        lengjing::auth::ResolveCloudLayoutStartupAction(
            hasAnyCloudVariableValue,
            configurationComplete,
            false,
            false,
            false);
    if (initialAction ==
        lengjing::auth::CloudLayoutStartupAction::UseBuiltInLayout) {
        return {{}, true};
    }
    if (initialAction !=
        lengjing::auth::CloudLayoutStartupAction::FetchCloudLayout) {
        std::fprintf(
            stderr, "%s\n", lengjing::app::VerificationFailureText());
        return {};
    }

    lengjing::auth::CloudLayoutStore store(identity);
    const lengjing::auth::CloudLayoutUpdateResult result =
        session.RefreshCloudLayout(store);
    const auto refreshAction =
        lengjing::auth::ResolveCloudLayoutStartupAction(
            true,
            true,
            true,
            result.Succeeded(),
            result.snapshot != nullptr);
    if (refreshAction !=
        lengjing::auth::CloudLayoutStartupAction::UseCloudLayout) {
        std::fprintf(
            stderr, "%s\n", lengjing::app::VerificationFailureText());
        return {};
    }
    return {result.snapshot, true};
}

std::vector<std::string> DriverOptions() {
    return {
        "纯C（支持虚拟机）",
        "棱镜内核驱动（推荐）",
        "内核 RPC",
    };
}

void InstallChineseFont() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig configuration{};
    configuration.FontDataOwnedByAtlas = false;
    configuration.OversampleH = 2;
    configuration.OversampleV = 2;
    configuration.PixelSnapH = true;
    io.Fonts->Clear();
    io.Fonts->AddFontFromMemoryTTF(
        const_cast<unsigned int*>(font_data),
        static_cast<int>(font_size),
        26.0f,
        &configuration,
        io.Fonts->GetGlyphRangesChineseFull());
}

class FrameLimiter final {
public:
    void Wait(int framesPerSecond) {
        if (framesPerSecond <= 0) {
            nextFrame_ = {};
            return;
        }
        const auto interval = std::chrono::nanoseconds(1000000000LL / framesPerSecond);
        const auto now = std::chrono::steady_clock::now();
        if (nextFrame_.time_since_epoch().count() == 0 || nextFrame_ < now - interval) {
            nextFrame_ = now + interval;
        } else {
            std::this_thread::sleep_until(nextFrame_);
            nextFrame_ += interval;
        }
    }

private:
    std::chrono::steady_clock::time_point nextFrame_{};
};

class SurfaceFrameRateHint final {
public:
    void Apply(ANativeWindow* window, int framesPerSecond) {
        if (window == nullptr) {
            Reset();
            return;
        }
        const int target = std::max(0, framesPerSecond);
        if (appliedWindow_ == window && appliedTarget_ == target) {
            ResetRetry();
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (retryWindow_ == window && retryTarget_ == target &&
            now < nextRetry_) {
            return;
        }

        const float frameRate = static_cast<float>(target);
        std::int32_t result = -1;
        if (SetWithStrategy() != nullptr) {
            constexpr std::int8_t kSeamlessOnly = 0;
            constexpr std::int8_t kAlways = 1;
            result = SetWithStrategy()(
                window,
                frameRate,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                target > 0 ? kAlways : kSeamlessOnly);
        } else if (SetBasic() != nullptr) {
            result = SetBasic()(
                window,
                frameRate,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
        if (result == 0) {
            appliedWindow_ = window;
            appliedTarget_ = target;
            ResetRetry();
            return;
        }

        if (retryWindow_ != window || retryTarget_ != target) {
            retryDelay_ = kInitialRetryDelay;
        }
        retryWindow_ = window;
        retryTarget_ = target;
        nextRetry_ = now + retryDelay_;
        retryDelay_ = std::min(retryDelay_ * 2, kMaximumRetryDelay);
    }

    void Reset() {
        appliedWindow_ = nullptr;
        appliedTarget_ = -1;
        ResetRetry();
    }

private:
    static constexpr std::chrono::milliseconds kInitialRetryDelay{50};
    static constexpr std::chrono::milliseconds kMaximumRetryDelay{1000};

    using SetWithStrategyFunction =
        std::int32_t (*)(ANativeWindow*, float, std::int8_t, std::int8_t);
    using SetBasicFunction =
        std::int32_t (*)(ANativeWindow*, float, std::int8_t);

    static void* AndroidLibrary() {
        static void* library =
            dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        return library;
    }

    static SetWithStrategyFunction SetWithStrategy() {
        static const auto function =
            reinterpret_cast<SetWithStrategyFunction>(
                AndroidLibrary() != nullptr
                    ? dlsym(
                          AndroidLibrary(),
                          "ANativeWindow_setFrameRateWithChangeStrategy")
                    : nullptr);
        return function;
    }

    static SetBasicFunction SetBasic() {
        static const auto function =
            reinterpret_cast<SetBasicFunction>(
                AndroidLibrary() != nullptr
                    ? dlsym(AndroidLibrary(), "ANativeWindow_setFrameRate")
                    : nullptr);
        return function;
    }

    void ResetRetry() {
        retryWindow_ = nullptr;
        retryTarget_ = -1;
        nextRetry_ = {};
        retryDelay_ = kInitialRetryDelay;
    }

    ANativeWindow* appliedWindow_ = nullptr;
    int appliedTarget_ = -1;
    ANativeWindow* retryWindow_ = nullptr;
    int retryTarget_ = -1;
    std::chrono::steady_clock::time_point nextRetry_{};
    std::chrono::milliseconds retryDelay_ = kInitialRetryDelay;
};

struct GraphicsInitialization {
    std::unique_ptr<AndroidImgui> graphics;
    lengjing::ui::RenderBackend activeBackend =
        lengjing::ui::RenderBackend::Cpu;
    bool fallbackApplied = false;
};

GraphicsInitialization InitializeGraphics(
    ANativeWindow* window,
    int width,
    int height,
    lengjing::ui::RenderBackend requestedBackend) {
    GraphicsInitialization result;
    const lengjing::ui::RenderBackend normalizedRequest =
        lengjing::app::NormalizeRenderBackend(requestedBackend);

    const auto tryBackend = [&](lengjing::ui::RenderBackend backend) {
        std::unique_ptr<AndroidImgui> candidate =
            GraphicsManager::getGraphicsInterface(
                lengjing::app::GraphicsApiForRenderBackend(backend));
        if (candidate == nullptr ||
            !candidate->Init_Render(window, width, height)) {
            return false;
        }
        result.graphics = std::move(candidate);
        result.activeBackend = backend;
        return true;
    };

    if (tryBackend(normalizedRequest)) {
        return result;
    }
    if (normalizedRequest != lengjing::ui::RenderBackend::Cpu) {
        result.fallbackApplied = true;
        if (tryBackend(lengjing::ui::RenderBackend::Cpu)) {
            return result;
        }
    }
    result.graphics.reset();
    return result;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const std::string programDirectory = CurrentDirectory();
    const std::string configPath = LocalConfigPath(programDirectory);

    lengjing::auth::AuthSession authSession;
    std::shared_ptr<const lengjing::auth::CloudLayoutDocument> cloudLayout;
    if constexpr (kRuntimeAuthEnabled) {
        if (!lengjing::auth::LoginInteractive(
                authSession,
                LENGJING_AUTH_VERSION,
                {},
                lengjing::auth::kDefaultT3AuthConfig,
                false)) {
            return 2;
        }
        CloudLayoutFetchResult cloudFetch =
            FetchAuthenticatedCloudLayout(authSession);
        if (!cloudFetch.continueStartup) {
            return lengjing::auth::kCloudLayoutStartupFailureExitCode;
        }
        cloudLayout = std::move(cloudFetch.snapshot);
    }

    const auto algorithmPosition = CoordinateReplayConfiguration();
    const int coordinateProbeSeconds = CoordinateProbeSeconds();
#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    const int algorithmCoordinateProbeSeconds =
        AlgorithmCoordinateProbeSeconds();
    if (coordinateProbeSeconds != 0 &&
        algorithmCoordinateProbeSeconds != 0) {
        std::fprintf(stderr, "probe mode conflict\n");
        return 16;
    }
    if (algorithmCoordinateProbeSeconds != 0) {
        const auto display = android::ANativeWindowCreator::GetDisplayInfo();
        if (display.width <= 0 || display.height <= 0) {
            std::fprintf(stderr, "无法获取屏幕尺寸\n");
            return 1;
        }
        return RunAlgorithmCoordinateProbe(
            algorithmCoordinateProbeSeconds,
            display.width,
            display.height,
            programDirectory,
            std::move(cloudLayout));
    }
#endif
    if (coordinateProbeSeconds != 0) {
        const auto display = android::ANativeWindowCreator::GetDisplayInfo();
        if (display.width <= 0 || display.height <= 0) {
            std::fprintf(stderr, "无法获取屏幕尺寸\n");
            return 1;
        }
        return RunCoordinateProbe(
            coordinateProbeSeconds,
            display.width,
            display.height,
            programDirectory,
            std::move(cloudLayout),
            algorithmPosition);
    }

    if (lengjing::platform::PerformanceTraceEnabled()) {
        constexpr char kPerformanceLogPath[] =
            "/data/local/tmp/lengjing_performance.txt";
        const char* requestedPerformanceLogPath =
            std::getenv("LENGJING_PERFORMANCE_LOG_PATH");
        const char* performanceLogPath =
            requestedPerformanceLogPath != nullptr &&
                requestedPerformanceLogPath[0] == '/'
            ? requestedPerformanceLogPath
            : kPerformanceLogPath;
        if (!lengjing::platform::DetachFromTerminal(performanceLogPath)) {
            return 3;
        }
        std::fprintf(
            stderr,
            "[perf-start] schema=2 version=%s pid=%d\n",
            LENGJING_VERSION,
            static_cast<int>(getpid()));
        std::fflush(stderr);
    } else {
#if LENGJING_ENABLE_COORDINATE_DEBUG_LOG
    constexpr char kCoordinateDebugLogPath[] =
        "/sdcard/Download/lengjing_coordinate_debug.txt";
    const char* requestedCoordinateDebugLogPath =
        std::getenv("LENGJING_COORDINATE_DEBUG_LOG_PATH");
    const char* coordinateDebugLogPath =
        requestedCoordinateDebugLogPath != nullptr &&
            requestedCoordinateDebugLogPath[0] == '/'
        ? requestedCoordinateDebugLogPath
        : kCoordinateDebugLogPath;
    setenv("LENGJING_COORDINATE_TRACE", "1", 1);
    setenv("LENGJING_COORDINATE_CANDIDATES_FULL", "0", 1);
    if (!lengjing::platform::DetachFromTerminal(
            coordinateDebugLogPath)) {
        return 3;
    }
    std::fprintf(
        stderr,
        "[coordinate-debug-start] schema=4 version=%s pid=%d "
        "trace=1 candidates_full=0 slot_family_calibration=1\n",
        LENGJING_VERSION,
        static_cast<int>(getpid()));
    std::fflush(stderr);
#else
    if (!lengjing::platform::DetachFromTerminal()) return 3;
#endif
    }
    if constexpr (kRuntimeAuthEnabled) {
        std::system(
            "chmod 000 /sys/class/kgsl/kgsl/pagetables >/dev/null 2>&1");
        std::system("am force-stop bin.mt.plus >/dev/null 2>&1");
        std::system("am force-stop bin.mt.plus.canary >/dev/null 2>&1");
        std::system("pkill -9 'bin.mt.plus$' >/dev/null 2>&1");
        std::system("pkill -9 'bin.mt.plus.canary$' >/dev/null 2>&1");
        std::system("killall -9 bin.mt.plus >/dev/null 2>&1");
        std::system("killall -9 bin.mt.plus.canary >/dev/null 2>&1");
    }
    auto display = android::ANativeWindowCreator::GetDisplayInfo();
    int surfaceWidth = display.width;
    int surfaceHeight = display.height;
    if (surfaceWidth <= 0 || surfaceHeight <= 0) {
        return 1;
    }

    if (kRuntimeAuthEnabled && !authSession.StartHeartbeat()) return 3;

    ANativeWindow* window = android::ANativeWindowCreator::Create(
        "lengjing-surface", surfaceWidth, surfaceHeight, false);
    if (window == nullptr) {
        std::fprintf(stderr, "无法创建绘制窗口\n");
        return 1;
    }

    lengjing::app::AppOptions options;
    options.configPath = configPath;
    options.programDirectory = programDirectory;
    options.driverOptions = DriverOptions();
    options.buildVersion = LENGJING_VERSION;
    options.cloudLayout = cloudLayout;
    options.algorithmPosition = algorithmPosition;
    lengjing::app::AppController controller(std::move(options));

    GraphicsInitialization initialGraphics = InitializeGraphics(
        window,
        surfaceWidth,
        surfaceHeight,
        controller.DesiredRenderBackend());
    if (initialGraphics.graphics == nullptr) {
        std::fprintf(stderr, "无法初始化图形接口\n");
        android::ANativeWindowCreator::Destroy(window);
        return 1;
    }
    std::unique_ptr<AndroidImgui> graphics =
        std::move(initialGraphics.graphics);
    lengjing::ui::RenderBackend activeBackend =
        initialGraphics.activeBackend;
    controller.ReportRenderBackend(
        activeBackend, initialGraphics.fallbackApplied);
    if (initialGraphics.fallbackApplied) {
        std::fprintf(
            stderr,
            "[render] selected backend initialization failed, using CPU\n");
    }

    InstallChineseFont();
    BaseTexData* menuLogo = graphics->LoadTextureFromMemory(
        const_cast<unsigned char*>(lengjing::ui::assets::kMenuLogoPng),
        static_cast<int>(lengjing::ui::assets::kMenuLogoPngSize));
    controller.SetMenuLogoTexture(
        menuLogo != nullptr ? menuLogo->DS : nullptr);
    ConfigureTouchDisplay(surfaceWidth, surfaceHeight, display.orientation);
    TouchScreenHandle(0);

    controller.SetDisplayGeometry(
        surfaceWidth, surfaceHeight, display.orientation);

    const char* performanceAutostart =
        std::getenv("LENGJING_PERFORMANCE_AUTOSTART");
    if (lengjing::platform::PerformanceTraceEnabled() &&
        performanceAutostart != nullptr &&
        performanceAutostart[0] != '\0' &&
        performanceAutostart[0] != '0') {
        const char* performanceCoordinate =
            std::getenv("LENGJING_PERFORMANCE_COORDINATE");
        if (performanceCoordinate != nullptr &&
            (performanceCoordinate[0] == '0' ||
             performanceCoordinate[0] == '1') &&
            performanceCoordinate[1] == '\0') {
            controller.Model().visual.coordinateDecrypt =
                performanceCoordinate[0] == '1';
        }
        controller.StartRuntime();
        controller.SetMenuVisible(false);
    }

#if LENGJING_ENABLE_ALGORITHM_COORDINATE
    const char* algorithmVisualAutostart =
        std::getenv("LENGJING_ALGORITHM_VISUAL_AUTOSTART");
    if (algorithmVisualAutostart != nullptr &&
        algorithmVisualAutostart[0] != '\0' &&
        algorithmVisualAutostart[0] != '0') {
        controller.Model().visual.coordinateDecrypt = false;
        controller.Model().visual.algorithmDecrypt = true;
        controller.StartRuntime();
        const char* showAlgorithmValidationMenu =
            std::getenv("LENGJING_ALGORITHM_VISUAL_SHOW_MENU");
        const bool showValidationMenu =
            showAlgorithmValidationMenu != nullptr &&
            showAlgorithmValidationMenu[0] != '\0' &&
            showAlgorithmValidationMenu[0] != '0';
        if (showValidationMenu) {
            controller.Model().page = lengjing::ui::Page::Runtime;
        }
        controller.SetMenuVisible(showValidationMenu);
    }
#endif

    lengjing::platform::MenuKeyMonitor menuKeys;
    menuKeys.Start();

    std::mutex surfaceMutex;
    std::atomic_bool mirrorRunning{true};
    std::thread mirrorThread([&mirrorRunning, &surfaceMutex] {
        using namespace std::chrono_literals;
        while (mirrorRunning.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(surfaceMutex);
                android::ANativeWindowCreator::ProcessMirrorDisplay();
            }
            for (int step = 0;
                 step < 10 && mirrorRunning.load(std::memory_order_acquire);
                 ++step) {
                std::this_thread::sleep_for(100ms);
            }
        }
    });

    FrameLimiter limiter;
    SurfaceFrameRateHint frameRateHint;
    frameRateHint.Apply(window, controller.TargetFrameRate());
    const auto recreateSurface =
        [&](int width,
            int height,
            int nextOrientation,
            lengjing::ui::RenderBackend requestedBackend) {
            controller.SetMenuLogoTexture(nullptr);
            menuLogo = nullptr;
            bool recreated = false;
            GraphicsInitialization initialization;
            {
                std::lock_guard<std::mutex> lock(surfaceMutex);
                if (graphics != nullptr) {
                    graphics->Shutdown();
                    graphics.reset();
                }
                if (window != nullptr) {
                    android::ANativeWindowCreator::Destroy(window);
                    window = nullptr;
                }
                window = android::ANativeWindowCreator::Create(
                    "lengjing-surface", width, height, false);
                if (window != nullptr) {
                    initialization = InitializeGraphics(
                        window, width, height, requestedBackend);
                    recreated = initialization.graphics != nullptr;
                    if (recreated) {
                        graphics = std::move(initialization.graphics);
                    }
                }
                frameRateHint.Reset();
                if (!recreated && window != nullptr) {
                    android::ANativeWindowCreator::Destroy(window);
                    window = nullptr;
                }
            }
            if (!recreated) {
                return false;
            }
            activeBackend = initialization.activeBackend;
            controller.ReportRenderBackend(
                activeBackend, initialization.fallbackApplied);
            if (initialization.fallbackApplied) {
                std::fprintf(
                    stderr,
                    "[render] selected backend initialization failed, using CPU\n");
            }
            InstallChineseFont();
            menuLogo = graphics->LoadTextureFromMemory(
                const_cast<unsigned char*>(
                    lengjing::ui::assets::kMenuLogoPng),
                static_cast<int>(
                    lengjing::ui::assets::kMenuLogoPngSize));
            controller.SetMenuLogoTexture(
                menuLogo != nullptr ? menuLogo->DS : nullptr);
            ConfigureTouchDisplay(width, height, nextOrientation);
            controller.SetDisplayGeometry(width, height, nextOrientation);
            return true;
        };
    auto nextDisplayRefresh = std::chrono::steady_clock::time_point{};
    auto nextSurfaceRetry = std::chrono::steady_clock::time_point{};
    bool surfaceFailureReported = false;
    int runtimeExitCode = 0;
    int orientation = display.orientation;
    while (!gStopRequested.load(std::memory_order_acquire) &&
           !controller.ExitRequested() &&
           (!kRuntimeAuthEnabled || !authSession.ExitRequested())) {
        runtimeExitCode = controller.RuntimeExitCode();
        if (runtimeExitCode != 0) break;
        const auto keyRequest = menuKeys.ConsumeRequest();
        if (keyRequest == lengjing::platform::MenuKeyRequest::Show) {
            controller.SetMenuVisible(true);
        } else if (keyRequest == lengjing::platform::MenuKeyRequest::Hide) {
            controller.SetMenuVisible(false);
        }

        const auto now = std::chrono::steady_clock::now();
        const bool surfaceUnavailable =
            graphics == nullptr || window == nullptr;
        const bool recoverSurface =
            graphics != nullptr &&
            graphics->ConsumeSurfaceRecoveryRequest();
        const bool refreshDisplay = now >= nextDisplayRefresh;
        if (refreshDisplay) {
            display = android::ANativeWindowCreator::GetDisplayInfo();
            nextDisplayRefresh = now + std::chrono::milliseconds(500);
        }
        const bool displayValid =
            display.width > 1 && display.height > 1;
        const int targetWidth =
            displayValid ? display.width : surfaceWidth;
        const int targetHeight =
            displayValid ? display.height : surfaceHeight;
        const bool sizeChanged =
            surfaceWidth != targetWidth ||
            surfaceHeight != targetHeight;
        const bool orientationChanged =
            displayValid && orientation != display.orientation;
        const lengjing::ui::RenderBackend desiredBackend =
            controller.DesiredRenderBackend();
        const bool backendChanged = desiredBackend != activeBackend;
        const int targetOrientation =
            displayValid ? display.orientation : orientation;
        const bool recreationRequested =
            surfaceUnavailable || recoverSurface || sizeChanged ||
            orientationChanged || backendChanged;
        if (recreationRequested) {
            if (surfaceUnavailable && now < nextSurfaceRetry) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));
                continue;
            }
            if (!recreateSurface(
                    targetWidth,
                    targetHeight,
                    targetOrientation,
                    desiredBackend)) {
                if (!surfaceFailureReported) {
                    std::fprintf(
                        stderr,
                        "无法重建绘制窗口，正在重试\n");
                    surfaceFailureReported = true;
                }
                nextSurfaceRetry =
                    now + std::chrono::milliseconds(100);
                nextDisplayRefresh = now;
                continue;
            }
            surfaceFailureReported = false;
            nextSurfaceRetry = std::chrono::steady_clock::time_point{};
        }
        if (recreationRequested) {
            surfaceWidth = targetWidth;
            surfaceHeight = targetHeight;
            orientation = targetOrientation;
        }

        PumpTouchInput();
        frameRateHint.Apply(window, controller.TargetFrameRate());
        limiter.Wait(controller.TargetFrameRate());
        graphics->NewFrame();
        controller.RenderFrame(graphics->PresentedFrameRate());
        {
            lengjing::platform::PerformanceTraceScope submitTrace(
                lengjing::platform::PerformancePhase::GraphicsSubmit);
            graphics->EndFrame();
        }
    }

    if (kRuntimeAuthEnabled && authSession.ExitRequested()) {
        controller.StopRuntime();
        std::fprintf(
            stderr, "%s\n", lengjing::app::VerificationFailureText());
    }

    menuKeys.Stop();
    StopTouchScreen();
    mirrorRunning.store(false, std::memory_order_release);
    if (mirrorThread.joinable()) {
        mirrorThread.join();
    }
    controller.SetMenuLogoTexture(nullptr);
    menuLogo = nullptr;
    if (graphics != nullptr) {
        graphics->Shutdown();
    }
    if (window != nullptr) {
        android::ANativeWindowCreator::Destroy(window);
    }
    return runtimeExitCode;
}
