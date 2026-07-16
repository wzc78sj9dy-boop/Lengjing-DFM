#include "app/AppController.h"
#include "auth/RemoteAuth.h"
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
#include <cstdio>
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

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    const std::string programDirectory = CurrentDirectory();
    lengjing::auth::AuthSession authSession;
    if (!lengjing::auth::LoginInteractive(programDirectory, authSession)) {
        std::fprintf(stderr, "登录失败, 程序退出\n");
        return 1;
    }
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
    auto nextDisplayRefresh = std::chrono::steady_clock::time_point{};
    int orientation = display.orientation;
    while (!gStopRequested.load(std::memory_order_acquire) &&
           !authSession.ExitRequested() &&
           !controller.ExitRequested()) {
        const auto keyRequest = menuKeys.ConsumeRequest();
        if (keyRequest == lengjing::platform::MenuKeyRequest::Show) {
            controller.SetMenuVisible(true);
        } else if (keyRequest == lengjing::platform::MenuKeyRequest::Hide) {
            controller.SetMenuVisible(false);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextDisplayRefresh) {
            display = android::ANativeWindowCreator::GetDisplayInfo();
            if (display.width > 1 && display.height > 1 &&
                (surfaceWidth != display.width ||
                 surfaceHeight != display.height ||
                 orientation != display.orientation)) {
                const bool sizeChanged =
                    surfaceWidth != display.width ||
                    surfaceHeight != display.height;
                if (sizeChanged) {
                    controller.SetMenuLogoTexture(nullptr);
                    bool recreated = false;
                    {
                        std::lock_guard<std::mutex> lock(surfaceMutex);
                        graphics->Shutdown();
                        android::ANativeWindowCreator::Destroy(window);
                        window = android::ANativeWindowCreator::Create(
                            "lengjing-surface",
                            display.width,
                            display.height,
                            false);
                        graphics = GraphicsManager::getGraphicsInterface(
                            GraphicsManager::CPU);
                        recreated =
                            window != nullptr &&
                            graphics != nullptr &&
                            graphics->Init_Render(
                                window, display.width, display.height);
                        if (!recreated && window != nullptr) {
                            android::ANativeWindowCreator::Destroy(window);
                            window = nullptr;
                        }
                    }
                    if (!recreated) {
                        std::fprintf(stderr, "无法重建绘制窗口\n");
                        gStopRequested.store(true, std::memory_order_release);
                        break;
                    }
                    InstallChineseFont();
                    menuLogo = graphics->LoadTextureFromMemory(
                        const_cast<unsigned char*>(
                            lengjing::ui::assets::kMenuLogoPng),
                        static_cast<int>(
                            lengjing::ui::assets::kMenuLogoPngSize));
                    controller.SetMenuLogoTexture(
                        menuLogo != nullptr ? menuLogo->DS : nullptr);
                }
                surfaceWidth = display.width;
                surfaceHeight = display.height;
                orientation = display.orientation;
                ConfigureTouchDisplay(surfaceWidth, surfaceHeight, orientation);
                controller.SetDisplayGeometry(
                    surfaceWidth, surfaceHeight, orientation);
            }
            nextDisplayRefresh = now + std::chrono::milliseconds(500);
        }

        PumpTouchInput();
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
