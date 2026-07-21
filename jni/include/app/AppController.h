#pragma once

#include "config/LocalConfig.h"
#include "game/GameRuntime.h"
#include "render/overlay_renderer.h"
#include "ui/MenuView.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace lengjing::app {

struct AppOptions {
    std::string configPath = "lengjing.json";
    std::string programDirectory;
    std::vector<std::string> driverOptions;
    std::string buildVersion;
    void* menuLogoTexture = nullptr;
    ui::AimInputMode inputMode = ui::AimInputMode::ReadOnly;
    std::shared_ptr<const auth::CloudLayoutDocument> cloudLayout;
    game::native::AlgorithmPositionRuntimeConfig algorithmPosition;
};

class AppController final : public ui::UiActions {
public:
    explicit AppController(AppOptions options);
    AppController(AppOptions options,
                  std::unique_ptr<game::GameBackend> backend,
                  OverlayRenderer renderer = OverlayRenderer{});
    ~AppController() override;

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    void RenderFrame(float presentedFramesPerSecond);

    ui::UiModel& Model() noexcept;
    const ui::UiModel& Model() const noexcept;
    bool ExitRequested() const noexcept;
    int RuntimeExitCode() const;
    int TargetFrameRate() const noexcept;
    ui::RenderBackend DesiredRenderBackend() const noexcept;
    void ReportRenderBackend(
        ui::RenderBackend activeBackend,
        bool fallbackApplied = false) noexcept;
    void SetMenuVisible(bool visible) noexcept;
    void SetMenuLogoTexture(void* texture) noexcept;
    void SetDisplayGeometry(int width, int height, int orientation) noexcept;
    void AppendLog(std::string message);

    void StartRuntime() override;
    void StopRuntime() override;
    void HideMenu() override;
    void ExitApplication() override;
    void ClearLogs() override;
    void ResetLocalSettings() override;
    void ReloadCustomItems() override;
    void AimEnabledChanged(bool enabled) override;
    void SettingsChanged(ui::SettingsDomain domain) override;

private:
    struct ToastNotification {
        std::string message;
        std::chrono::steady_clock::time_point refreshedAt{};
        std::chrono::steady_clock::time_point expiresAt{};
    };

    game::FeatureSettings BuildFeatureSettings() const;
    game::RuntimeOptions BuildRuntimeOptions() const;
    void SyncRuntimeStatus();
    void SyncToastSetting();
    void DrawToastNotifications(ImDrawList* drawList);
    void RefreshRenderStyle();
    void DrawGameFrame(const game::GameFrame& frame, ImDrawList* drawList);
    void DrawPopulation(const game::GameFrame& frame, ImDrawList* drawList);
    void ScheduleConfigSave();
    bool FlushConfig(bool force);

    AppOptions options_;
    config::LocalConfig config_;
    game::GameRuntime runtime_;
    OverlayRenderer renderer_;
    RenderStyle baseRenderStyle_{};
    ui::MenuView menuView_;
    ui::UiModel model_{};
    game::RuntimeStatus lastStatus_{};
    bool coordinateDecryptSuccessNotified_ = false;
    game::CoordinateDecryptError lastReportedCoordinateError_ =
        game::CoordinateDecryptError::None;
    int lastReportedCoordinateSystemError_ = 0;
    std::chrono::steady_clock::time_point configSaveDeadline_{};
    std::deque<ToastNotification> toastNotifications_;
    std::mutex toastNotificationsMutex_;
    std::atomic<bool> toastNotificationsEnabled_{true};
    bool configDirty_ = false;
    bool terminalWorkerJoined_ = true;
    bool exitRequested_ = false;
    int displayOrientation_ = 0;
    bool populationDrawnThisFrame_ = false;
    bool populationBoundsValid_ = false;
    bool populationPressActive_ = false;
    bool populationPressCanceled_ = false;
    float populationBoundsMinimumX_ = 0.0f;
    float populationBoundsMinimumY_ = 0.0f;
    float populationBoundsMaximumX_ = 0.0f;
    float populationBoundsMaximumY_ = 0.0f;
    float populationPressMinimumX_ = 0.0f;
    float populationPressMinimumY_ = 0.0f;
    float populationPressMaximumX_ = 0.0f;
    float populationPressMaximumY_ = 0.0f;
    float populationAlpha_ = 0.0f;
    float populationPress_ = 0.0f;
    float populationWidth_ = -1.0f;
    float populationTop_ = -1.0f;
    float populationTopVelocity_ = 0.0f;
    float populationMenuProgress_ = -1.0f;
    float populationHover_ = 0.0f;
    float populationPulse_ = 0.0f;
    int lastPlayerCount_ = -1;
    int lastBotCount_ = -1;
};

}  // namespace lengjing::app
