#include "app/AppController.h"

#include "game/aim/AimModePolicy.h"
#include "app/RenderBackendSelection.h"
#include "app/RuntimeExitPolicy.h"
#include "app/RuntimePresentationPolicy.h"
#include "game/GameVersionPolicy.h"
#include "platform/PerformanceTrace.h"

#include "ImGui/imgui.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace lengjing::app {
namespace {

constexpr auto kConfigSaveDelay = std::chrono::milliseconds(750);
constexpr auto kConfigRetryDelay = std::chrono::seconds(2);
constexpr auto kToastLifetime = std::chrono::seconds(5);
constexpr auto kToastDuplicateWindow = std::chrono::milliseconds(1250);
constexpr std::size_t kMaximumLogBytes = 64 * 1024;
constexpr std::size_t kMaximumToastNotifications = 5;

constexpr std::array<int, 7> kFrameRates{
    30, 60, 90, 120, 144, 165, 0};

bool IsTerminal(game::RuntimePhase phase) {
    return phase == game::RuntimePhase::Stopped ||
        phase == game::RuntimePhase::Faulted;
}

ImU32 WithOpacity(ImU32 color, float opacity) {
    ImVec4 converted = ImGui::ColorConvertU32ToFloat4(color);
    converted.w *= std::clamp(opacity, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(converted);
}

float Approach(float current, float target, float speed, float deltaTime) {
    const float factor = 1.0f - std::exp(
        -std::max(0.0f, speed) * std::clamp(deltaTime, 0.0f, 0.1f));
    return current + (target - current) * factor;
}

float Lerp(float start, float end, float amount) {
    return start + (end - start) * std::clamp(amount, 0.0f, 1.0f);
}

float SmoothStep01(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

float AdvanceSpring(float current,
                    float& velocity,
                    float target,
                    float stiffness,
                    float damping,
                    float deltaTime) {
    float remaining = std::clamp(deltaTime, 0.0f, 0.1f);
    while (remaining > 0.0f) {
        const float step = std::min(remaining, 1.0f / 120.0f);
        const float acceleration =
            (target - current) * std::max(0.0f, stiffness) -
            velocity * std::max(0.0f, damping);
        velocity += acceleration * step;
        current += velocity * step;
        remaining -= step;
    }
    return current;
}

bool ContainsPoint(float x,
                   float y,
                   float minimumX,
                   float minimumY,
                   float maximumX,
                   float maximumY) {
    return x >= minimumX && x <= maximumX &&
        y >= minimumY && y <= maximumY;
}

ImU32 BlendColor(ImU32 start, ImU32 end, float amount) {
    const float clamped = std::clamp(amount, 0.0f, 1.0f);
    const ImVec4 first = ImGui::ColorConvertU32ToFloat4(start);
    const ImVec4 second = ImGui::ColorConvertU32ToFloat4(end);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        Lerp(first.x, second.x, clamped),
        Lerp(first.y, second.y, clamped),
        Lerp(first.z, second.z, clamped),
        Lerp(first.w, second.w, clamped)));
}

std::array<ImVec2, 8> ChamferedPanelPoints(
    const ImVec2& minimum,
    const ImVec2& maximum,
    float cut) {
    const float safeCut = std::clamp(
        cut,
        0.0f,
        std::max(
            0.0f,
            std::min(maximum.x - minimum.x, maximum.y - minimum.y) * 0.5f));
    return {
        ImVec2(minimum.x + safeCut, minimum.y),
        ImVec2(maximum.x - safeCut, minimum.y),
        ImVec2(maximum.x, minimum.y + safeCut),
        ImVec2(maximum.x, maximum.y - safeCut),
        ImVec2(maximum.x - safeCut, maximum.y),
        ImVec2(minimum.x + safeCut, maximum.y),
        ImVec2(minimum.x, maximum.y - safeCut),
        ImVec2(minimum.x, minimum.y + safeCut),
    };
}

std::array<ImVec2, 8> OffsetPanelPoints(
    const std::array<ImVec2, 8>& points,
    const ImVec2& offset) {
    std::array<ImVec2, 8> shifted = points;
    for (ImVec2& point : shifted) {
        point.x += offset.x;
        point.y += offset.y;
    }
    return shifted;
}

}  // namespace

AppController::AppController(AppOptions options)
    : AppController(
          std::move(options), game::CreateNativeGameBackend(), OverlayRenderer{}) {}

AppController::AppController(AppOptions options,
                             std::unique_ptr<game::GameBackend> backend,
                             OverlayRenderer renderer)
    : options_(std::move(options)),
      config_(options_.configPath),
      runtime_(std::move(backend)),
      renderer_(std::move(renderer)),
      baseRenderStyle_(renderer_.Style()) {
    menuView_.SetLogoTexture(options_.menuLogoTexture);
    model_.aim.inputMode = options_.inputMode;

    std::string error;
    if (!config_.Load(model_, &error)) {
        AppendLog(error.empty() ? "本地配置加载失败" : std::move(error));
    }
    toastNotificationsEnabled_.store(
        model_.system.toastNotifications, std::memory_order_release);

    model_.runtime.driverOptions = options_.driverOptions;
    model_.runtime.buildVersion = options_.buildVersion;
    if (!model_.runtime.driverOptions.empty()) {
        model_.runtime.driverIndex = std::clamp(
            model_.runtime.driverIndex, 0,
            static_cast<int>(model_.runtime.driverOptions.size()) - 1);
    }

    runtime_.UpdateSettings(BuildFeatureSettings());
    runtime_.SetAimEnabled(game::aim::IsAimOutputRequested(
        model_.aim.enabled, model_.aim.trajectoryTracking));
    RefreshRenderStyle();
}

AppController::~AppController() {
    runtime_.Stop();
    runtime_.WaitUntilStopped();
    terminalWorkerJoined_ = true;
    FlushConfig(true);
}

void AppController::RenderFrame(float presentedFramesPerSecond) {
    platform::PerformanceTraceScope renderFrameTrace(
        platform::PerformancePhase::RenderFrame);
    ImGuiIO& io = ImGui::GetIO();
    model_.runtime.framesPerSecond =
        std::max(0.0f, presentedFramesPerSecond);
    model_.runtime.screenWidth = std::max(0, static_cast<int>(io.DisplaySize.x));
    model_.runtime.screenHeight = std::max(0, static_cast<int>(io.DisplaySize.y));

    SyncRuntimeStatus();
    FlushConfig(false);

    populationDrawnThisFrame_ = false;
    menuView_.ClearTopOverlayBounds();
    if (model_.runtime.active) {
        const std::shared_ptr<const game::GameFrame> frame = runtime_.LatestFrame();
        if (frame != nullptr) {
            DrawGameFrame(*frame, ImGui::GetBackgroundDrawList());
        }
    }
    if (!populationDrawnThisFrame_) {
        populationBoundsValid_ = false;
        populationPressActive_ = false;
        populationPressCanceled_ = false;
        populationAlpha_ = 0.0f;
        populationWidth_ = -1.0f;
        populationTop_ = -1.0f;
        populationTopVelocity_ = 0.0f;
        populationMenuProgress_ = -1.0f;
        populationHover_ = 0.0f;
        populationPress_ = 0.0f;
        populationPulse_ = 0.0f;
        lastPlayerCount_ = -1;
        lastBotCount_ = -1;
    }

    menuView_.Render(model_, *this);
    DrawToastNotifications(ImGui::GetForegroundDrawList());
}

ui::UiModel& AppController::Model() noexcept {
    return model_;
}

const ui::UiModel& AppController::Model() const noexcept {
    return model_;
}

bool AppController::ExitRequested() const noexcept {
    return exitRequested_;
}

int AppController::RuntimeExitCode() const {
    const game::RuntimeStatus status = runtime_.Status();
    return ResolveRuntimeExitCode(
        game::CloudLayoutMatchesGameVersion(
            options_.cloudLayout.get(),
            model_.runtime.gameVersionIndex),
        status.phase,
        status.failureKind);
}

int AppController::TargetFrameRate() const noexcept {
    const int index = std::clamp(
        model_.system.frameLimitIndex, 0,
        static_cast<int>(kFrameRates.size()) - 1);
    return kFrameRates[static_cast<std::size_t>(index)];
}

ui::RenderBackend AppController::DesiredRenderBackend() const noexcept {
    return NormalizeRenderBackend(model_.system.renderBackend);
}

void AppController::ReportRenderBackend(
    ui::RenderBackend activeBackend,
    bool fallbackApplied) noexcept {
    const ui::RenderBackend normalized =
        NormalizeRenderBackend(activeBackend);
    model_.runtime.activeRenderBackend = normalized;
    if (fallbackApplied && model_.system.renderBackend != normalized) {
        model_.system.renderBackend = normalized;
        ScheduleConfigSave();
    }
}

void AppController::SetMenuVisible(bool visible) noexcept {
    model_.visible = visible;
}

void AppController::SetMenuLogoTexture(void* texture) noexcept {
    menuView_.SetLogoTexture(texture);
}

void AppController::SetDisplayGeometry(
    int width, int height, int orientation) noexcept {
    if (width <= 1 || height <= 1) {
        return;
    }
    const int normalizedOrientation =
        ((orientation % 4) + 4) % 4;
    const bool geometryChanged =
        width != model_.runtime.screenWidth ||
        height != model_.runtime.screenHeight ||
        normalizedOrientation != displayOrientation_;
    if (geometryChanged) {
        menuView_.RequestRecenter();
        populationBoundsValid_ = false;
        populationPressActive_ = false;
        populationPressCanceled_ = false;
        populationWidth_ = -1.0f;
        populationTop_ = -1.0f;
        populationTopVelocity_ = 0.0f;
    }
    model_.runtime.screenWidth = width;
    model_.runtime.screenHeight = height;
    displayOrientation_ = normalizedOrientation;
    runtime_.UpdateDisplayGeometry(width, height, displayOrientation_);
}

void AppController::AppendLog(std::string message) {
    if (message.empty()) {
        return;
    }

    if (toastNotificationsEnabled_.load(std::memory_order_acquire)) {
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(toastNotificationsMutex_);
        if (toastNotificationsEnabled_.load(std::memory_order_acquire)) {
            auto duplicate = std::find_if(
                toastNotifications_.rbegin(), toastNotifications_.rend(),
                [&](const ToastNotification& notification) {
                    return notification.message == message &&
                        now - notification.refreshedAt <= kToastDuplicateWindow;
                });
            if (duplicate != toastNotifications_.rend()) {
                ToastNotification refreshed = std::move(*duplicate);
                toastNotifications_.erase(std::next(duplicate).base());
                refreshed.refreshedAt = now;
                refreshed.expiresAt = now + kToastLifetime;
                toastNotifications_.push_back(std::move(refreshed));
            } else {
                toastNotifications_.push_back(ToastNotification{
                    message,
                    now,
                    now + kToastLifetime,
                });
                while (toastNotifications_.size() > kMaximumToastNotifications) {
                    toastNotifications_.pop_front();
                }
            }
        }
    }

    std::string& log = model_.runtime.logText;
    if (!log.empty() && log.back() != '\n') {
        log.push_back('\n');
    }
    log.append(message);
    log.push_back('\n');

    if (log.size() <= kMaximumLogBytes) {
        return;
    }
    const std::size_t minimumTrim = log.size() - kMaximumLogBytes;
    const std::size_t lineEnd = log.find('\n', minimumTrim);
    log.erase(0, lineEnd == std::string::npos ? minimumTrim : lineEnd + 1);
}

void AppController::StartRuntime() {
    const game::RuntimeStatus status = runtime_.Status();
    if (IsTerminal(status.phase) && !terminalWorkerJoined_) {
        runtime_.WaitUntilStopped();
        terminalWorkerJoined_ = true;
    }

    runtime_.UpdateSettings(BuildFeatureSettings());
    runtime_.SetAimEnabled(game::aim::IsAimOutputRequested(
        model_.aim.enabled, model_.aim.trajectoryTracking));
    if (!runtime_.Start(BuildRuntimeOptions())) {
        AppendLog(game::FormatRuntimeDiagnostic(
            game::RuntimeError::StartRejected,
            -EBUSY));
        return;
    }
    coordinateDecryptSuccessNotified_ = false;
    lastReportedCoordinateError_ = game::CoordinateDecryptError::None;
    lastReportedCoordinateSystemError_ = 0;
    lastReportedRuntimeError_ = game::RuntimeError::None;
    lastReportedRuntimeSystemError_ = 0;
    terminalWorkerJoined_ = false;
}

void AppController::StopRuntime() {
    runtime_.Stop();
}

void AppController::HideMenu() {
    model_.visible = false;
}

void AppController::ExitApplication() {
    exitRequested_ = true;
    runtime_.Stop();
    FlushConfig(true);
}

void AppController::ClearLogs() {
    model_.runtime.logText.clear();
}

void AppController::ResetLocalSettings() {
    const bool visible = model_.visible;
    const ui::Page page = model_.page;
    const ui::AimInputMode inputMode = model_.aim.inputMode;
    const std::size_t customItemCount = model_.loot.customItemCount;
    ui::RuntimeModel runtimeState = std::move(model_.runtime);

    model_ = ui::UiModel{};
    model_.visible = visible;
    model_.page = page;
    model_.runtime = std::move(runtimeState);
    model_.runtime.gameVersionIndex = 0;
    model_.runtime.driverIndex = 0;
    model_.aim.inputMode = inputMode;
    model_.loot.customItemCount = customItemCount;
    toastNotificationsEnabled_.store(
        model_.system.toastNotifications, std::memory_order_release);

    runtime_.UpdateSettings(BuildFeatureSettings());
    runtime_.SetAimEnabled(false);
    RefreshRenderStyle();
    AppendLog("本地配置已重置");
    configDirty_ = true;
    FlushConfig(true);
}

void AppController::ReloadCustomItems() {
    runtime_.ReloadCustomItems();
    AppendLog("已提交自定义物资重载");
}

void AppController::AimEnabledChanged(bool) {
    runtime_.SetAimEnabled(game::aim::IsAimOutputRequested(
        model_.aim.enabled, model_.aim.trajectoryTracking));
}

void AppController::SettingsChanged(ui::SettingsDomain domain) {
    SyncToastSetting();
    runtime_.UpdateSettings(BuildFeatureSettings());
    if (domain == ui::SettingsDomain::Aim) {
        runtime_.SetAimEnabled(game::aim::IsAimOutputRequested(
            model_.aim.enabled, model_.aim.trajectoryTracking));
    }
    if (domain == ui::SettingsDomain::Visual) {
        RefreshRenderStyle();
    }
    ScheduleConfigSave();
}

void AppController::SyncToastSetting() {
    const bool enabled = model_.system.toastNotifications;
    toastNotificationsEnabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        std::lock_guard lock(toastNotificationsMutex_);
        toastNotifications_.clear();
    }
}

void AppController::DrawToastNotifications(ImDrawList* drawList) {
    if (drawList == nullptr ||
        !toastNotificationsEnabled_.load(std::memory_order_acquire)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<ToastNotification> notifications;
    {
        std::lock_guard lock(toastNotificationsMutex_);
        toastNotifications_.erase(
            std::remove_if(
                toastNotifications_.begin(), toastNotifications_.end(),
                [&](const ToastNotification& notification) {
                    return notification.expiresAt <= now;
                }),
            toastNotifications_.end());
        notifications.assign(
            toastNotifications_.begin(), toastNotifications_.end());
    }
    if (notifications.empty() ||
        !toastNotificationsEnabled_.load(std::memory_order_acquire)) {
        return;
    }

    const float screenWidth = static_cast<float>(model_.runtime.screenWidth);
    const float screenHeight = static_cast<float>(model_.runtime.screenHeight);
    if (screenWidth <= 0.0f || screenHeight <= 0.0f) {
        return;
    }

    const RenderStyle& style = renderer_.Style();
    ImFont* font = ImGui::GetFont();
    if (font == nullptr) {
        return;
    }

    constexpr ImU32 kIndigoShadow = IM_COL32(2, 3, 15, 230);
    constexpr ImU32 kIndigoShell = IM_COL32(10, 12, 38, 246);
    constexpr ImU32 kIndigoRaised = IM_COL32(22, 24, 64, 244);
    constexpr ImU32 kIndigoBorder = IM_COL32(72, 82, 132, 230);
    constexpr ImU32 kElectricCyan = IM_COL32(52, 230, 255, 255);
    constexpr ImU32 kSignalMagenta = IM_COL32(242, 77, 214, 255);
    constexpr ImU32 kSignalAmber = IM_COL32(255, 190, 70, 255);
    constexpr ImU32 kColdText = IM_COL32(232, 241, 255, 255);

    const float scale = std::clamp(
        std::min(screenWidth, screenHeight) / 1080.0f, 0.78f, 1.28f);
    const float fontSize = std::max(
        13.0f, style.metrics.smallFontSize * std::clamp(scale, 0.9f, 1.16f));
    const float margin = std::clamp(
        screenWidth * 0.018f, 14.0f * scale, 30.0f * scale);
    const float spacing = 10.0f * scale;
    const float paddingX = 14.0f * scale;
    const float paddingY = 12.0f * scale;
    const float markerColumn = 31.0f * scale;
    const float availableWidth = std::max(120.0f, screenWidth - margin * 2.0f);
    const float maximumWidth = std::min(
        availableWidth,
        std::clamp(screenWidth * 0.36f, 280.0f * scale, 460.0f * scale));
    const float minimumWidth = std::min(maximumWidth, 260.0f * scale);
    const float contentWidth = std::max(
        40.0f, maximumWidth - paddingX * 2.0f - markerColumn);
    float bottom = screenHeight - margin;

    for (auto notification = notifications.rbegin();
         notification != notifications.rend(); ++notification) {
        const float secondsLeft =
            std::chrono::duration<float>(notification->expiresAt - now).count();
        const float age =
            std::chrono::duration<float>(now - notification->refreshedAt).count();
        const float enterProgress = SmoothStep01(age / 0.20f);
        const float exitProgress = SmoothStep01(secondsLeft / 0.38f);
        const float opacity = std::min(enterProgress, exitProgress);
        if (opacity <= 0.001f) {
            continue;
        }
        const ImVec2 textSize = font->CalcTextSizeA(
            fontSize, std::numeric_limits<float>::max(), contentWidth,
            notification->message.c_str());
        const float panelWidth = std::clamp(
            textSize.x + paddingX * 2.0f + markerColumn,
            minimumWidth,
            maximumWidth);
        const float panelHeight = std::max(
            58.0f * scale, textSize.y + paddingY * 2.0f + 5.0f * scale);
        const float slideOffset = (1.0f - enterProgress) * 28.0f * scale;
        const ImVec2 minimum{
            screenWidth - margin - panelWidth + slideOffset,
            bottom - panelHeight,
        };
        const ImVec2 maximum{
            screenWidth - margin + slideOffset,
            bottom,
        };
        const float cut = 8.0f * scale;
        const std::array<ImVec2, 8> panel =
            ChamferedPanelPoints(minimum, maximum, cut);
        const std::array<ImVec2, 8> shadow = OffsetPanelPoints(
            panel, ImVec2(4.0f * scale, 5.0f * scale));

        drawList->AddConvexPolyFilled(
            shadow.data(),
            static_cast<int>(shadow.size()),
            WithOpacity(kIndigoShadow, opacity * 0.78f));
        drawList->AddConvexPolyFilled(
            panel.data(),
            static_cast<int>(panel.size()),
            WithOpacity(kIndigoShell, opacity));
        const ImVec2 insetMinimum{
            minimum.x + 2.0f * scale,
            minimum.y + 2.0f * scale};
        const ImVec2 insetMaximum{
            maximum.x - 2.0f * scale,
            maximum.y - 2.0f * scale};
        const std::array<ImVec2, 8> inset = ChamferedPanelPoints(
            insetMinimum, insetMaximum, std::max(0.0f, cut - 2.0f * scale));
        drawList->AddConvexPolyFilled(
            inset.data(),
            static_cast<int>(inset.size()),
            WithOpacity(kIndigoRaised, opacity * 0.56f));
        drawList->AddPolyline(
            panel.data(),
            static_cast<int>(panel.size()),
            WithOpacity(kIndigoBorder, opacity),
            ImDrawFlags_Closed,
            std::max(1.0f, 1.2f * scale));

        const float railY = minimum.y + 2.0f * scale;
        const float railMiddle = Lerp(
            minimum.x + cut,
            maximum.x - cut,
            0.56f);
        drawList->AddLine(
            ImVec2(minimum.x + cut, railY),
            ImVec2(railMiddle, railY),
            WithOpacity(kElectricCyan, opacity),
            std::max(1.0f, 2.0f * scale));
        drawList->AddLine(
            ImVec2(railMiddle, railY),
            ImVec2(maximum.x - cut, railY),
            WithOpacity(kSignalMagenta, opacity),
            std::max(1.0f, 2.0f * scale));

        const ImVec2 markerCenter{
            minimum.x + paddingX + 8.0f * scale,
            minimum.y + panelHeight * 0.5f - 1.0f * scale};
        const float markerRadius = 7.0f * scale;
        const std::array<ImVec2, 4> marker{
            ImVec2(markerCenter.x, markerCenter.y - markerRadius),
            ImVec2(markerCenter.x + markerRadius, markerCenter.y),
            ImVec2(markerCenter.x, markerCenter.y + markerRadius),
            ImVec2(markerCenter.x - markerRadius, markerCenter.y),
        };
        drawList->AddConvexPolyFilled(
            marker.data(),
            static_cast<int>(marker.size()),
            WithOpacity(kElectricCyan, opacity * 0.16f));
        drawList->AddPolyline(
            marker.data(),
            static_cast<int>(marker.size()),
            WithOpacity(kElectricCyan, opacity),
            ImDrawFlags_Closed,
            std::max(1.0f, 1.4f * scale));
        drawList->AddCircleFilled(
            markerCenter,
            std::max(1.5f, 2.2f * scale),
            WithOpacity(kSignalMagenta, opacity));

        const float dividerX = minimum.x + paddingX + markerColumn - 5.0f * scale;
        drawList->AddLine(
            ImVec2(dividerX, minimum.y + 12.0f * scale),
            ImVec2(dividerX, maximum.y - 12.0f * scale),
            WithOpacity(kSignalAmber, opacity * 0.62f),
            std::max(1.0f, scale));
        drawList->AddText(
            font, fontSize,
            ImVec2(
                minimum.x + paddingX + markerColumn,
                minimum.y + (panelHeight - textSize.y) * 0.5f - 1.0f * scale),
            WithOpacity(kColdText, opacity),
            notification->message.c_str(), nullptr, contentWidth);

        const float lifetime = std::max(
            0.001f, std::chrono::duration<float>(kToastLifetime).count());
        const float remaining = std::clamp(secondsLeft / lifetime, 0.0f, 1.0f);
        const float trackLeft = minimum.x + paddingX + markerColumn;
        const float trackRight = maximum.x - paddingX;
        const float trackY = maximum.y - 7.0f * scale;
        drawList->AddRectFilled(
            ImVec2(trackLeft, trackY),
            ImVec2(trackRight, trackY + 2.0f * scale),
            WithOpacity(kIndigoBorder, opacity * 0.55f));
        const float progressRight = Lerp(trackLeft, trackRight, remaining);
        if (progressRight > trackLeft) {
            drawList->AddRectFilledMultiColor(
                ImVec2(trackLeft, trackY),
                ImVec2(progressRight, trackY + 2.0f * scale),
                WithOpacity(kElectricCyan, opacity),
                WithOpacity(kSignalMagenta, opacity),
                WithOpacity(kSignalMagenta, opacity),
                WithOpacity(kElectricCyan, opacity));
        }
        drawList->AddTriangleFilled(
            ImVec2(maximum.x - cut, maximum.y),
            ImVec2(maximum.x, maximum.y - cut),
            ImVec2(maximum.x - cut, maximum.y - cut),
            WithOpacity(kSignalAmber, opacity * 0.82f));

        bottom = minimum.y - spacing;
        if (bottom <= margin) {
            break;
        }
    }
}

game::FeatureSettings AppController::BuildFeatureSettings() const {
    game::FeatureSettings settings{};
    settings.visual = model_.visual;
    settings.visual.debugInfo = false;
    settings.visual.classNameDebug = false;
    settings.loot = model_.loot;
    settings.radar = model_.radar;
    settings.aim = model_.aim;
    settings.aim.trajectoryTracking =
        game::IsProjectileTrackingRequested(
            model_.aim.trajectoryTracking);
    return settings;
}

game::RuntimeOptions AppController::BuildRuntimeOptions() const {
    game::RuntimeOptions options{};
    options.gameVersionIndex = model_.runtime.gameVersionIndex;
    options.driverIndex = model_.runtime.driverIndex;
    options.inputMode = model_.aim.inputMode;
    options.screenWidth = model_.runtime.screenWidth;
    options.screenHeight = model_.runtime.screenHeight;
    options.orientation = displayOrientation_;
    options.programDirectory = options_.programDirectory;
    options.cloudLayout = game::SelectCloudLayoutForGameVersion(
        options_.cloudLayout,
        options.gameVersionIndex);
    return options;
}

void AppController::SyncRuntimeStatus() {
    const game::RuntimeStatus status = runtime_.Status();
    model_.runtime.active = status.phase == game::RuntimePhase::Running;
    model_.runtime.busy = status.phase == game::RuntimePhase::Starting ||
        status.phase == game::RuntimePhase::Stopping;
    model_.runtime.stopping = status.phase == game::RuntimePhase::Stopping;
    model_.runtime.processId = status.processId;
    model_.runtime.baseReady = status.baseReady;
    model_.runtime.coordinateRequested = status.coordinateRequested;
    model_.runtime.coordinateEntryReady = status.coordinateEntryReady;
    model_.runtime.coordinateContextReady = status.coordinateContextReady;
    model_.runtime.coordinateThreadId = status.coordinateThreadId;
    model_.runtime.coordinateGuestPc = status.coordinateGuestPc;
    model_.runtime.coordinateContextGeneration =
        status.coordinateContextGeneration;
    model_.runtime.coordinateAttempts = status.coordinateAttempts;
    model_.runtime.coordinateSuccesses = status.coordinateSuccesses;
    model_.runtime.coordinateErrorCode =
        game::CoordinateDecryptErrorCode(status.coordinateError);
    model_.runtime.coordinateSystemError = status.coordinateSystemError;
    model_.runtime.runtimeErrorCode =
        game::RuntimeErrorCode(status.runtimeError);
    model_.runtime.runtimeSystemError = status.runtimeSystemError;
    const CoordinateDecryptPresentation nextDecryptPresentation =
        ResolveCoordinateDecryptPresentation(
            status.coordinateRequested,
            status.coordinateEntryReady,
            status.coordinateContextReady,
            status.coordinateSuccesses);
    if (ShouldNotifyCoordinateDecryptSuccess(
            nextDecryptPresentation,
            coordinateDecryptSuccessNotified_)) {
        coordinateDecryptSuccessNotified_ = true;
        AppendLog(CoordinateDecryptPresentationText(nextDecryptPresentation));
    }
    model_.loot.customItemCount = status.customItemCount;

    if (status.phase != lastStatus_.phase) {
        switch (status.phase) {
        case game::RuntimePhase::Stopped:
            if (lastStatus_.phase != game::RuntimePhase::Stopped) {
                AppendLog("运行模块已停止");
            }
            break;
        case game::RuntimePhase::Starting:
            AppendLog("正在启动运行模块");
            break;
        case game::RuntimePhase::Running:
            AppendLog("运行模块已启动");
            runtime_.SetAimEnabled(game::aim::IsAimOutputRequested(
                model_.aim.enabled, model_.aim.trajectoryTracking));
            break;
        case game::RuntimePhase::Stopping:
            AppendLog("正在停止运行模块");
            break;
        case game::RuntimePhase::Faulted:
            break;
        }
    }

    const bool coordinateDiagnosticChanged =
        status.coordinateError != game::CoordinateDecryptError::None &&
        (status.coordinateError != lastReportedCoordinateError_ ||
         status.coordinateSystemError !=
             lastReportedCoordinateSystemError_);
    if (coordinateDiagnosticChanged) {
        AppendLog(game::FormatCoordinateDecryptDiagnostic(
            status.coordinateError,
            status.coordinateSystemError));
        lastReportedCoordinateError_ = status.coordinateError;
        lastReportedCoordinateSystemError_ =
            status.coordinateSystemError;
    } else if (status.coordinateError ==
               game::CoordinateDecryptError::None) {
        lastReportedCoordinateError_ = game::CoordinateDecryptError::None;
        lastReportedCoordinateSystemError_ = 0;
    }

    const game::RuntimeError reportedRuntimeError =
        status.runtimeError != game::RuntimeError::None
        ? status.runtimeError
        : (!status.message.empty() &&
           status.coordinateError == game::CoordinateDecryptError::None
            ? game::RuntimeError::FrameDataUnavailable
            : game::RuntimeError::None);
    const bool runtimeDiagnosticChanged =
        reportedRuntimeError != game::RuntimeError::None &&
        (reportedRuntimeError != lastReportedRuntimeError_ ||
         status.runtimeSystemError != lastReportedRuntimeSystemError_);
    if (runtimeDiagnosticChanged) {
        AppendLog(game::FormatRuntimeDiagnostic(
            reportedRuntimeError,
            status.runtimeSystemError));
        lastReportedRuntimeError_ = reportedRuntimeError;
        lastReportedRuntimeSystemError_ = status.runtimeSystemError;
    } else if (reportedRuntimeError == game::RuntimeError::None) {
        if (status.phase == game::RuntimePhase::Running &&
            status.message.empty() && !lastStatus_.message.empty() &&
            lastStatus_.coordinateError ==
                game::CoordinateDecryptError::None) {
            AppendLog(RuntimeDataRestoredText());
        }
        lastReportedRuntimeError_ = game::RuntimeError::None;
        lastReportedRuntimeSystemError_ = 0;
    }

    lastStatus_ = status;
    if (IsTerminal(status.phase) && !terminalWorkerJoined_) {
        runtime_.WaitUntilStopped();
        terminalWorkerJoined_ = true;
    }
}

void AppController::RefreshRenderStyle() {
    RenderStyle style = baseRenderStyle_;
    const float thickness = std::max(0.5f, model_.visual.lineThickness);
    const float fontScale = std::max(0.2f, model_.visual.fontScale);
    style.metrics.lineWidth = baseRenderStyle_.metrics.lineWidth * thickness;
    style.metrics.outlineWidth = baseRenderStyle_.metrics.outlineWidth *
        std::max(0.75f, thickness);
    style.metrics.fontSize = baseRenderStyle_.metrics.fontSize * fontScale;
    style.metrics.smallFontSize = baseRenderStyle_.metrics.smallFontSize * fontScale;
    renderer_.SetStyle(style);
}

void AppController::DrawGameFrame(const game::GameFrame& frame,
                                  ImDrawList* drawList) {
    if (drawList == nullptr) {
        return;
    }
    platform::PerformanceTraceScope drawFrameTrace(
        platform::PerformancePhase::DrawGameFrame);
    const int initialVertexCount = drawList->VtxBuffer.Size;
    const int initialIndexCount = drawList->IdxBuffer.Size;
    ImDrawList* foregroundDrawList = ImGui::GetForegroundDrawList();
    const bool separateForeground = foregroundDrawList != drawList;
    const int initialForegroundVertexCount = separateForeground
        ? foregroundDrawList->VtxBuffer.Size
        : 0;
    const int initialForegroundIndexCount = separateForeground
        ? foregroundDrawList->IdxBuffer.Size
        : 0;
    platform::RecordPerformanceCount(
        platform::PerformanceCounter::RenderPlayers,
        frame.players.size());

    const ScreenRect viewport{
        0.0f,
        0.0f,
        static_cast<float>(model_.runtime.screenWidth),
        static_cast<float>(model_.runtime.screenHeight),
    };
    if (!viewport.IsValid()) {
        return;
    }

    if (frame.radar.has_value()) {
        renderer_.DrawRadar(drawList, *frame.radar, viewport);
    }
    if (frame.hudMap.has_value()) {
        renderer_.DrawHudMap(drawList, *frame.hudMap, viewport);
    }
    for (const WorldLabel& label : frame.worldLabels) {
        if (label.kind != WorldLabelKind::ScreenAlert) {
            renderer_.DrawWorldLabel(drawList, label, viewport);
        }
    }
    for (const ProjectileVisual& projectile : frame.projectiles) {
        renderer_.DrawProjectile(drawList, projectile, viewport);
    }
    if (frame.modelGeometry.has_value()) {
        renderer_.DrawModelGeometry(drawList, *frame.modelGeometry, viewport);
    }
    for (const PlayerVisual& player : frame.players) {
        renderer_.DrawPlayer(drawList, player, viewport);
    }
    for (const PlayerSignalVisual& signal : frame.playerSignals) {
        renderer_.DrawPlayerSignal(drawList, signal, viewport);
    }
    for (const OffscreenMarker& marker : frame.offscreenMarkers) {
        renderer_.DrawOffscreenWarning(drawList, marker, viewport);
    }
    if (frame.highValueList.has_value()) {
        renderer_.DrawHighValueList(drawList, *frame.highValueList, viewport);
    }
    if (frame.crosshair.has_value()) {
        renderer_.DrawCrosshair(drawList, *frame.crosshair);
    }
    if (frame.aimGuide.has_value()) {
        renderer_.DrawAimGuide(drawList, *frame.aimGuide, viewport);
    }
    if (frame.touchRegion.has_value()) {
        renderer_.DrawTouchRegion(drawList, *frame.touchRegion, viewport);
    }
    for (const WorldLabel& label : frame.worldLabels) {
        if (label.kind == WorldLabelKind::ScreenAlert) {
            renderer_.DrawWorldLabel(drawList, label, viewport);
        }
    }
    if (model_.visual.enabled && model_.visual.playerCount) {
        DrawPopulation(frame, ImGui::GetForegroundDrawList());
    }
    platform::RecordPerformanceCount(
        platform::PerformanceCounter::DrawCommands,
        static_cast<std::uint64_t>(drawList->CmdBuffer.Size +
            (separateForeground
                ? foregroundDrawList->CmdBuffer.Size
                : 0)));
    platform::RecordPerformanceCount(
        platform::PerformanceCounter::DrawVertices,
        static_cast<std::uint64_t>(std::max(
            0,
            drawList->VtxBuffer.Size - initialVertexCount +
                (separateForeground
                    ? foregroundDrawList->VtxBuffer.Size -
                        initialForegroundVertexCount
                    : 0))));
    platform::RecordPerformanceCount(
        platform::PerformanceCounter::DrawIndices,
        static_cast<std::uint64_t>(std::max(
            0,
            drawList->IdxBuffer.Size - initialIndexCount +
                (separateForeground
                    ? foregroundDrawList->IdxBuffer.Size -
                        initialForegroundIndexCount
                    : 0))));
}

void AppController::DrawPopulation(const game::GameFrame& frame,
                                   ImDrawList* drawList) {
    if (drawList == nullptr || model_.runtime.screenWidth <= 1 ||
        model_.runtime.screenHeight <= 1) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    ImFont* font = ImGui::GetFont();
    if (font == nullptr) {
        return;
    }
    populationDrawnThisFrame_ = true;

    constexpr ImU32 kIndigoShadow = IM_COL32(2, 3, 16, 230);
    constexpr ImU32 kIndigoShell = IM_COL32(9, 11, 36, 244);
    constexpr ImU32 kIndigoRaised = IM_COL32(24, 25, 65, 246);
    constexpr ImU32 kIndigoHover = IM_COL32(31, 32, 78, 248);
    constexpr ImU32 kIndigoBorder = IM_COL32(70, 80, 132, 235);
    constexpr ImU32 kElectricCyan = IM_COL32(49, 229, 255, 255);
    constexpr ImU32 kSignalMagenta = IM_COL32(244, 72, 216, 255);
    constexpr ImU32 kSignalAmber = IM_COL32(255, 188, 66, 255);
    constexpr ImU32 kColdText = IM_COL32(229, 241, 255, 255);
    constexpr ImU32 kMagentaText = IM_COL32(255, 193, 240, 255);

    const float screenWidth = static_cast<float>(model_.runtime.screenWidth);
    const float screenHeight = static_cast<float>(model_.runtime.screenHeight);
    const float scale = std::clamp(
        std::min(screenWidth, screenHeight) / 1080.0f, 0.78f, 1.35f);
    const float centerX = screenWidth * 0.5f;
    const ImVec2 safePadding = ImGui::GetStyle().DisplaySafeAreaPadding;
    const float safeMarginX = std::max(
        safePadding.x,
        std::clamp(screenWidth * 0.012f, 12.0f * scale, 28.0f * scale));
    const float safeMarginY = std::max(
        safePadding.y,
        std::clamp(screenHeight * 0.014f, 10.0f * scale, 26.0f * scale));

    char playerText[32]{};
    char botText[32]{};
    std::snprintf(
        playerText, sizeof(playerText), "玩家 %d", frame.playerCount);
    std::snprintf(botText, sizeof(botText), "人机 %d", frame.botCount);
    const float fontSize = std::clamp(19.0f * scale, 16.0f, 26.0f);
    const ImVec2 playerTextSize = font->CalcTextSizeA(
        fontSize,
        std::numeric_limits<float>::max(),
        0.0f,
        playerText);
    const ImVec2 botTextSize = font->CalcTextSizeA(
        fontSize,
        std::numeric_limits<float>::max(),
        0.0f,
        botText);
    const float paddingX = 17.0f * scale;
    const float paddingY = 10.0f * scale;
    const float markerSpan = 13.0f * scale;
    const float markerGap = 11.0f * scale;
    const float segmentGap = 24.0f * scale;
    const float panelHeight = std::ceil(
        std::max(playerTextSize.y, botTextSize.y) + paddingY * 2.0f);
    const float maximumWidth = std::max(
        1.0f, screenWidth - safeMarginX * 2.0f);
    const float minimumWidth = std::min(
        maximumWidth, panelHeight * 4.0f);
    const float targetWidth = std::clamp(
        playerTextSize.x + botTextSize.x +
            paddingX * 2.0f + markerSpan + markerGap + segmentGap,
        minimumWidth,
        maximumWidth);
    if (populationWidth_ < 0.0f) {
        populationWidth_ = targetWidth;
    }
    populationWidth_ = std::clamp(
        Approach(populationWidth_, targetWidth, 10.5f, io.DeltaTime),
        minimumWidth,
        maximumWidth);

    if (lastPlayerCount_ != frame.playerCount ||
        lastBotCount_ != frame.botCount) {
        if (lastPlayerCount_ >= 0 && lastBotCount_ >= 0) {
            populationPulse_ = 1.0f;
        }
        lastPlayerCount_ = frame.playerCount;
        lastBotCount_ = frame.botCount;
    }
    populationPulse_ = Approach(
        populationPulse_, 0.0f, 5.2f, io.DeltaTime);

    if (populationMenuProgress_ < 0.0f) {
        populationMenuProgress_ = model_.visible ? 1.0f : 0.0f;
    }
    populationMenuProgress_ = Approach(
        populationMenuProgress_,
        model_.visible ? 1.0f : 0.0f,
        model_.visible ? 7.5f : 9.0f,
        io.DeltaTime);

    const float safeTop = safeMarginY;
    const float maximumTop = std::max(
        safeTop, screenHeight - safeMarginY - panelHeight);
    const float closedCenterY = std::clamp(
        screenHeight * 0.11f,
        safeTop + panelHeight * 0.5f,
        maximumTop + panelHeight * 0.5f);
    const float closedTop = closedCenterY - panelHeight * 0.5f;
    float targetTop = closedTop;
    if (model_.visible) {
        const float firstStage = SmoothStep01(
            populationMenuProgress_ / 0.62f);
        const float secondStage = SmoothStep01(
            (populationMenuProgress_ - 0.46f) / 0.54f);
        const float settleTop = std::min(
            closedTop, safeTop + 10.0f * scale);
        targetTop = Lerp(closedTop, settleTop, firstStage);
        targetTop = Lerp(targetTop, safeTop, secondStage);
    }
    if (populationTop_ < 0.0f) {
        populationTop_ = model_.visible ? safeTop : closedTop;
        populationTopVelocity_ = 0.0f;
    }
    populationTop_ = AdvanceSpring(
        populationTop_,
        populationTopVelocity_,
        targetTop,
        model_.visible ? 235.0f : 185.0f,
        model_.visible ? 19.0f : 21.0f,
        io.DeltaTime);
    if (populationTop_ < safeTop) {
        populationTop_ = safeTop;
        populationTopVelocity_ = std::max(0.0f, populationTopVelocity_);
    } else if (populationTop_ > maximumTop) {
        populationTop_ = maximumTop;
        populationTopVelocity_ = std::min(0.0f, populationTopVelocity_);
    }

    const float panelLeft = std::clamp(
        centerX - populationWidth_ * 0.5f,
        safeMarginX,
        screenWidth - safeMarginX - populationWidth_);
    const ImVec2 panelMinimum{panelLeft, populationTop_};
    const ImVec2 panelMaximum{
        panelLeft + populationWidth_, populationTop_ + panelHeight};
    menuView_.SetTopOverlayBounds(
        panelMinimum.x,
        panelMinimum.y,
        panelMaximum.x,
        panelMaximum.y,
        safeTop + panelHeight);

    populationBoundsMinimumX_ = panelMinimum.x;
    populationBoundsMinimumY_ = panelMinimum.y;
    populationBoundsMaximumX_ = panelMaximum.x;
    populationBoundsMaximumY_ = panelMaximum.y;
    populationBoundsValid_ = true;

    const bool hovered = ContainsPoint(
        io.MousePos.x,
        io.MousePos.y,
        panelMinimum.x,
        panelMinimum.y,
        panelMaximum.x,
        panelMaximum.y);
    const bool pressedNow =
        ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const bool releasedNow =
        ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (pressedNow && hovered) {
        populationPressActive_ = true;
        populationPressCanceled_ = false;
        populationPressMinimumX_ = panelMinimum.x;
        populationPressMinimumY_ = panelMinimum.y;
        populationPressMaximumX_ = panelMaximum.x;
        populationPressMaximumY_ = panelMaximum.y;
    }
    if (populationPressActive_) {
        const bool insidePressBounds = ContainsPoint(
            io.MousePos.x,
            io.MousePos.y,
            populationPressMinimumX_,
            populationPressMinimumY_,
            populationPressMaximumX_,
            populationPressMaximumY_);
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
            !insidePressBounds) {
            populationPressCanceled_ = true;
        }
        if (releasedNow) {
            if (!populationPressCanceled_ && insidePressBounds) {
                model_.visible = !model_.visible;
            }
            populationPressActive_ = false;
            populationPressCanceled_ = false;
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !pressedNow) {
            populationPressActive_ = false;
            populationPressCanceled_ = false;
        }
    }
    if (hovered || populationPressActive_) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    populationAlpha_ = Approach(
        populationAlpha_, 1.0f, 6.5f, io.DeltaTime);
    populationPress_ = Approach(
        populationPress_,
        populationPressActive_ && !populationPressCanceled_ ? 1.0f : 0.0f,
        populationPressActive_ ? 26.0f : 14.0f,
        io.DeltaTime);
    populationHover_ = Approach(
        populationHover_, hovered ? 1.0f : 0.0f, 14.0f, io.DeltaTime);

    const float opacity = std::clamp(populationAlpha_, 0.0f, 1.0f);
    const ImU32 panelColor = BlendColor(
        kIndigoShell,
        BlendColor(kIndigoRaised, kIndigoHover, populationHover_),
        std::clamp(
            populationHover_ * 0.58f + populationPress_ * 0.30f,
            0.0f,
            1.0f));
    const ImU32 borderColor = BlendColor(
        kIndigoBorder,
        BlendColor(kElectricCyan, kSignalMagenta, populationHover_ * 0.68f),
        std::clamp(
            0.22f +
                populationHover_ * 0.48f +
                populationPulse_ * 0.70f +
                populationPress_ * 0.18f,
            0.0f,
            1.0f));

    const float cut = 7.0f * scale;
    const std::array<ImVec2, 8> panel =
        ChamferedPanelPoints(panelMinimum, panelMaximum, cut);
    const std::array<ImVec2, 8> shadow = OffsetPanelPoints(
        panel, ImVec2(3.0f * scale, 4.0f * scale));
    drawList->AddConvexPolyFilled(
        shadow.data(),
        static_cast<int>(shadow.size()),
        WithOpacity(kIndigoShadow, opacity * 0.75f));
    drawList->AddConvexPolyFilled(
        panel.data(),
        static_cast<int>(panel.size()),
        WithOpacity(panelColor, opacity));

    const float upperRailY = panelMinimum.y + 2.0f * scale;
    const float lowerRailY = panelMaximum.y - 2.0f * scale;
    const float railCenter = centerX;
    drawList->AddLine(
        ImVec2(panelMinimum.x + cut, upperRailY),
        ImVec2(railCenter, upperRailY),
        WithOpacity(kElectricCyan, opacity * 0.90f),
        std::max(1.0f, 1.8f * scale));
    drawList->AddLine(
        ImVec2(railCenter, upperRailY),
        ImVec2(panelMaximum.x - cut, upperRailY),
        WithOpacity(kSignalMagenta, opacity * 0.90f),
        std::max(1.0f, 1.8f * scale));
    drawList->AddLine(
        ImVec2(panelMinimum.x + cut, lowerRailY),
        ImVec2(railCenter, lowerRailY),
        WithOpacity(kSignalMagenta, opacity * 0.32f),
        std::max(1.0f, scale));
    drawList->AddLine(
        ImVec2(railCenter, lowerRailY),
        ImVec2(panelMaximum.x - cut, lowerRailY),
        WithOpacity(kElectricCyan, opacity * 0.32f),
        std::max(1.0f, scale));

    if (populationPulse_ > 0.01f) {
        drawList->AddPolyline(
            panel.data(),
            static_cast<int>(panel.size()),
            WithOpacity(
                kSignalAmber,
                opacity * populationPulse_ * 0.82f),
            ImDrawFlags_Closed,
            std::max(1.0f, (1.0f + populationPulse_ * 1.6f) * scale));
    }
    drawList->AddPolyline(
        panel.data(),
        static_cast<int>(panel.size()),
        WithOpacity(borderColor, opacity),
        ImDrawFlags_Closed,
        std::max(1.0f, 1.25f * scale));

    const float popFontSize =
        fontSize * (1.0f + populationPulse_ * 0.055f);
    const ImVec2 popPlayerSize = font->CalcTextSizeA(
        popFontSize,
        std::numeric_limits<float>::max(),
        0.0f,
        playerText);
    const ImVec2 popBotSize = font->CalcTextSizeA(
        popFontSize,
        std::numeric_limits<float>::max(),
        0.0f,
        botText);
    const float groupWidth =
        markerSpan + markerGap + popPlayerSize.x + segmentGap + popBotSize.x;
    const float contentStart = centerX - groupWidth * 0.5f;
    const float contentOffsetY =
        populationPress_ * scale - populationPulse_ * 1.5f * scale;
    const ImVec2 markerCenter{
        contentStart + markerSpan * 0.5f,
        panelMinimum.y + panelHeight * 0.5f + contentOffsetY};

    drawList->PushClipRect(panelMinimum, panelMaximum, true);
    const float markerRadius = 5.0f * scale;
    const std::array<ImVec2, 4> marker{
        ImVec2(markerCenter.x, markerCenter.y - markerRadius),
        ImVec2(markerCenter.x + markerRadius, markerCenter.y),
        ImVec2(markerCenter.x, markerCenter.y + markerRadius),
        ImVec2(markerCenter.x - markerRadius, markerCenter.y),
    };
    drawList->AddConvexPolyFilled(
        marker.data(),
        static_cast<int>(marker.size()),
        WithOpacity(
            model_.visible ? kElectricCyan : kSignalAmber,
            opacity * (0.20f + populationPulse_ * 0.20f)));
    drawList->AddPolyline(
        marker.data(),
        static_cast<int>(marker.size()),
        WithOpacity(
            model_.visible ? kElectricCyan : kSignalAmber,
            opacity),
        ImDrawFlags_Closed,
        std::max(1.0f, 1.2f * scale));
    drawList->AddCircleFilled(
        markerCenter,
        std::max(1.2f, 1.8f * scale),
        WithOpacity(kSignalMagenta, opacity));

    const ImVec2 playerPosition{
        contentStart + markerSpan + markerGap,
        panelMinimum.y +
            (panelHeight - popPlayerSize.y) * 0.5f +
            contentOffsetY};
    const float dividerX =
        playerPosition.x + popPlayerSize.x + segmentGap * 0.5f;
    const ImVec2 botPosition{
        dividerX + segmentGap * 0.5f,
        panelMinimum.y +
            (panelHeight - popBotSize.y) * 0.5f +
            contentOffsetY};
    drawList->AddLine(
        ImVec2(
            dividerX,
            panelMinimum.y + 11.0f * scale + contentOffsetY),
        ImVec2(
            dividerX,
            panelMaximum.y - 11.0f * scale + contentOffsetY),
        WithOpacity(kSignalAmber, opacity * 0.74f),
        std::max(1.0f, 1.2f * scale));

    drawList->AddText(
        font,
        popFontSize,
        ImVec2(playerPosition.x + scale, playerPosition.y + scale),
        WithOpacity(kIndigoShadow, opacity * 0.72f),
        playerText);
    drawList->AddText(
        font,
        popFontSize,
        playerPosition,
        WithOpacity(kColdText, opacity),
        playerText);
    drawList->AddText(
        font,
        popFontSize,
        ImVec2(botPosition.x + scale, botPosition.y + scale),
        WithOpacity(kIndigoShadow, opacity * 0.72f),
        botText);
    drawList->AddText(
        font,
        popFontSize,
        botPosition,
        WithOpacity(kMagentaText, opacity),
        botText);
    drawList->PopClipRect();

    const float indicatorOpacity = opacity * (
        0.28f + populationHover_ * 0.46f + populationPress_ * 0.18f);
    const float indicatorY = panelMaximum.y - 5.0f * scale;
    const float indicatorHalfWidth = std::min(
        18.0f * scale, populationWidth_ * 0.09f);
    drawList->AddTriangleFilled(
        ImVec2(centerX - indicatorHalfWidth, indicatorY),
        ImVec2(centerX - 3.0f * scale, indicatorY),
        ImVec2(centerX - 7.0f * scale, indicatorY - 4.0f * scale),
        WithOpacity(kElectricCyan, indicatorOpacity));
    drawList->AddTriangleFilled(
        ImVec2(centerX - 3.0f * scale, indicatorY),
        ImVec2(centerX + 3.0f * scale, indicatorY),
        ImVec2(centerX, indicatorY - 4.0f * scale),
        WithOpacity(kSignalAmber, indicatorOpacity));
    drawList->AddTriangleFilled(
        ImVec2(centerX + 3.0f * scale, indicatorY),
        ImVec2(centerX + indicatorHalfWidth, indicatorY),
        ImVec2(centerX + 7.0f * scale, indicatorY - 4.0f * scale),
        WithOpacity(kSignalMagenta, indicatorOpacity));
}

void AppController::ScheduleConfigSave() {
    configDirty_ = true;
    configSaveDeadline_ = std::chrono::steady_clock::now() + kConfigSaveDelay;
}

bool AppController::FlushConfig(bool force) {
    if (!configDirty_) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && now < configSaveDeadline_) {
        return true;
    }

    std::string error;
    if (config_.Save(model_, &error)) {
        configDirty_ = false;
        return true;
    }

    configSaveDeadline_ = now + kConfigRetryDelay;
    AppendLog(error.empty() ? "本地配置保存失败" : std::move(error));
    return false;
}

}  // namespace lengjing::app
