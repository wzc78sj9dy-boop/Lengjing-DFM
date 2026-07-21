#include "app/AppController.h"
#include "app/RenderBackendSelection.h"
#include "app/RuntimeExitPolicy.h"
#include "app/RuntimePresentationPolicy.h"
#include "auth/CloudLayoutStartupPolicy.h"
#include "auth/RemoteAuth.h"
#include "game/native/MemoryTransport.h"
#include "platform/BackgroundProcess.h"
#include "platform/MenuKeyMonitor.h"

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

int CoordinateProbeSeconds() {
    const char* value = std::getenv("LENGJING_COORDINATE_PROBE_SECONDS");
    if (value == nullptr || value[0] == '\0') return 0;
    int seconds = 0;
    const char* end = value + std::char_traits<char>::length(value);
    const auto parsed = std::from_chars(value, end, seconds, 10);
    return parsed.ec == std::errc{} && parsed.ptr == end &&
            seconds >= 1 && seconds <= 120
        ? seconds
        : 0;
}

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
    if (!runtime.Start(options)) return 10;

    lengjing::game::RuntimeStatus last{};
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        const lengjing::game::RuntimeStatus status = runtime.Status();
        last = status;
        if (status.phase == lengjing::game::RuntimePhase::Faulted) break;
        std::this_thread::sleep_for(100ms);
    }
    runtime.Stop();
    runtime.WaitUntilStopped();
    const auto decryptPresentation =
        lengjing::app::ResolveCoordinateDecryptPresentation(
            last.coordinateRequested,
            last.coordinateEntryReady,
            last.coordinateContextReady,
            last.coordinateSuccesses);
    std::fprintf(
        stderr, "%s\n",
        lengjing::app::CoordinateDecryptPresentationText(
            decryptPresentation));
    const int runtimeExitCode = lengjing::app::ResolveRuntimeExitCode(
        cloudLayoutActive, last.phase, last.failureKind);
    if (runtimeExitCode != 0) return runtimeExitCode;
    return last.coordinateSuccesses != 0
        ? 0
        : (last.coordinateContextReady
            ? 13
            : (last.coordinateEntryReady ? 12 : 11));
}

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

    if (!lengjing::platform::DetachFromTerminal()) return 3;
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
        graphics->EndFrame();
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
