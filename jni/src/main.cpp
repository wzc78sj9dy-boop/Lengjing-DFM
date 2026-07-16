#include "app/AppController.h"
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
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef LENGJING_VERSION
#define LENGJING_VERSION "dev"
#endif

namespace {

std::atomic_bool gStopRequested{false};

void HandleSignal(int) {
    gStopRequested.store(true, std::memory_order_release);
}

std::string CurrentDirectory() {
    char path[1024]{};
    return getcwd(path, sizeof(path)) != nullptr
        ? std::string(path)
        : std::string("/data/local/tmp");
}

std::vector<std::string> DriverOptions() {
    return {
        "纯C（支持虚拟机）",
        "棱镜内核驱动（推荐）",
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
        if (window_ == window && target_ == target) {
            return;
        }

        window_ = window;
        target_ = target;
        const float frameRate = static_cast<float>(target);
        if (SetWithStrategy() != nullptr) {
            constexpr std::int8_t kSeamlessOnly = 0;
            constexpr std::int8_t kAlways = 1;
            SetWithStrategy()(
                window,
                frameRate,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
                target > 0 ? kAlways : kSeamlessOnly);
        } else if (SetBasic() != nullptr) {
            SetBasic()(
                window,
                frameRate,
                ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT);
        }
    }

    void Reset() {
        window_ = nullptr;
        target_ = -1;
    }

private:
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

    ANativeWindow* window_ = nullptr;
    int target_ = -1;
};

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const std::string programDirectory = CurrentDirectory();
    const std::string configPath = programDirectory + "/lengjing.json";

    auto display = android::ANativeWindowCreator::GetDisplayInfo();
    int surfaceWidth = display.width;
    int surfaceHeight = display.height;
    if (surfaceWidth <= 0 || surfaceHeight <= 0) {
        std::fprintf(stderr, "无法获取屏幕尺寸\n");
        return 1;
    }

    ANativeWindow* window = android::ANativeWindowCreator::Create(
        "lengjing-surface", surfaceWidth, surfaceHeight, false);
    if (window == nullptr) {
        std::fprintf(stderr, "无法创建绘制窗口\n");
        return 1;
    }

    std::unique_ptr<AndroidImgui> graphics =
        GraphicsManager::getGraphicsInterface(GraphicsManager::CPU);
    if (graphics == nullptr || !graphics->Init_Render(window, surfaceWidth, surfaceHeight)) {
        std::fprintf(stderr, "无法初始化图形接口\n");
        android::ANativeWindowCreator::Destroy(window);
        return 1;
    }

    InstallChineseFont();
    BaseTexData* menuLogo = graphics->LoadTextureFromMemory(
        const_cast<unsigned char*>(lengjing::ui::assets::kMenuLogoPng),
        static_cast<int>(lengjing::ui::assets::kMenuLogoPngSize));
    ConfigureTouchDisplay(surfaceWidth, surfaceHeight, display.orientation);
    TouchScreenHandle(0);

    lengjing::app::AppOptions options;
    options.configPath = configPath;
    options.programDirectory = programDirectory;
    options.driverOptions = DriverOptions();
    options.buildVersion = LENGJING_VERSION;
    options.menuLogoTexture = menuLogo != nullptr ? menuLogo->DS : nullptr;
    lengjing::app::AppController controller(std::move(options));
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
        [&](int width, int height) {
            controller.SetMenuLogoTexture(nullptr);
            bool recreated = false;
            {
                std::lock_guard<std::mutex> lock(surfaceMutex);
                graphics->Shutdown();
                android::ANativeWindowCreator::Destroy(window);
                window = android::ANativeWindowCreator::Create(
                    "lengjing-surface", width, height, false);
                graphics = GraphicsManager::getGraphicsInterface(
                    GraphicsManager::CPU);
                recreated =
                    window != nullptr &&
                    graphics != nullptr &&
                    graphics->Init_Render(window, width, height);
                frameRateHint.Reset();
                if (!recreated && window != nullptr) {
                    android::ANativeWindowCreator::Destroy(window);
                    window = nullptr;
                }
            }
            if (!recreated) {
                return false;
            }
            InstallChineseFont();
            menuLogo = graphics->LoadTextureFromMemory(
                const_cast<unsigned char*>(
                    lengjing::ui::assets::kMenuLogoPng),
                static_cast<int>(
                    lengjing::ui::assets::kMenuLogoPngSize));
            controller.SetMenuLogoTexture(
                menuLogo != nullptr ? menuLogo->DS : nullptr);
            return true;
        };
    auto nextDisplayRefresh = std::chrono::steady_clock::time_point{};
    int orientation = display.orientation;
    while (!gStopRequested.load(std::memory_order_acquire) &&
           !controller.ExitRequested()) {
        const auto keyRequest = menuKeys.ConsumeRequest();
        if (keyRequest == lengjing::platform::MenuKeyRequest::Show) {
            controller.SetMenuVisible(true);
        } else if (keyRequest == lengjing::platform::MenuKeyRequest::Hide) {
            controller.SetMenuVisible(false);
        }

        const auto now = std::chrono::steady_clock::now();
        const bool recoverSurface =
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
        if ((recoverSurface || sizeChanged) &&
            !recreateSurface(targetWidth, targetHeight)) {
            std::fprintf(stderr, "无法重建绘制窗口\n");
            gStopRequested.store(true, std::memory_order_release);
            break;
        }
        if (sizeChanged || orientationChanged) {
            surfaceWidth = targetWidth;
            surfaceHeight = targetHeight;
            if (displayValid) {
                orientation = display.orientation;
            }
            ConfigureTouchDisplay(
                surfaceWidth, surfaceHeight, orientation);
            controller.SetDisplayGeometry(
                surfaceWidth, surfaceHeight, orientation);
        }

        PumpTouchInput();
        frameRateHint.Apply(window, controller.TargetFrameRate());
        graphics->NewFrame();
        controller.RenderFrame(graphics->PresentedFrameRate());
        graphics->EndFrame();
        limiter.Wait(controller.TargetFrameRate());
    }

    menuKeys.Stop();
    StopTouchScreen();
    mirrorRunning.store(false, std::memory_order_release);
    if (mirrorThread.joinable()) {
        mirrorThread.join();
    }
    graphics->Shutdown();
    android::ANativeWindowCreator::Destroy(window);
    return 0;
}
