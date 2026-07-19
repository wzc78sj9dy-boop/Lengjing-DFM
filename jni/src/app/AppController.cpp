#include "app/AppController.h"

#include "game/aim/AimModePolicy.h"
#include "app/RenderBackendSelection.h"

#include "ImGui/imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
    ImGuiIO& io = ImGui::GetIO();
    model_.runtime.framesPerSecond =
        std::max(0.0f, presentedFramesPerSecond);
    model_.runtime.screenWidth = std::max(0, static_cast<int>(io.DisplaySize.x));
    model_.runtime.screenHeight = std::max(0, static_cast<int>(io.DisplaySize.y));

    SyncToastSetting();
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

    std::fprintf(stderr, "[touch-debug] app-log=%s\n", message.c_str());
    std::fflush(stderr);

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
        AppendLog("运行模块无法启动");
        return;
    }
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
    const float fontSize = std::max(12.0f, style.metrics.smallFontSize);
    const float margin = std::clamp(screenWidth * 0.018f, 12.0f, 28.0f);
    const float spacing = 8.0f;
    const float paddingX = 13.0f;
    const float paddingY = 10.0f;
    const float accentWidth = 3.0f;
    const float maximumWidth = std::clamp(screenWidth * 0.34f, 220.0f, 390.0f);
    const float contentWidth = maximumWidth - paddingX * 2.0f - accentWidth;
    float bottom = screenHeight - margin;

    for (auto notification = notifications.rbegin();
         notification != notifications.rend(); ++notification) {
        const float secondsLeft =
            std::chrono::duration<float>(notification->expiresAt - now).count();
        const float opacity = std::clamp(secondsLeft / 0.4f, 0.0f, 1.0f);
        const ImVec2 textSize = font->CalcTextSizeA(
            fontSize, std::numeric_limits<float>::max(), contentWidth,
            notification->message.c_str());
        const float panelWidth = std::min(
            maximumWidth, textSize.x + paddingX * 2.0f + accentWidth);
        const float panelHeight = textSize.y + paddingY * 2.0f;
        const ImVec2 minimum{
            screenWidth - margin - panelWidth,
            bottom - panelHeight,
        };
        const ImVec2 maximum{
            screenWidth - margin,
            bottom,
        };

        drawList->AddRectFilled(
            ImVec2(minimum.x + 2.0f, minimum.y + 3.0f),
            ImVec2(maximum.x + 2.0f, maximum.y + 3.0f),
            WithOpacity(style.colors.shadow, opacity),
            style.metrics.panelRounding);
        drawList->AddRectFilled(
            minimum, maximum,
            WithOpacity(style.colors.surfaceRaised, opacity),
            style.metrics.panelRounding);
        drawList->AddRectFilled(
            minimum,
            ImVec2(minimum.x + accentWidth, maximum.y),
            WithOpacity(style.colors.accent, opacity),
            style.metrics.panelRounding);
        drawList->AddRect(
            minimum, maximum, WithOpacity(style.colors.border, opacity),
            style.metrics.panelRounding, 0, 1.0f);
        drawList->AddText(
            font, fontSize,
            ImVec2(minimum.x + paddingX + accentWidth, minimum.y + paddingY),
            WithOpacity(style.colors.text, opacity),
            notification->message.c_str(), nullptr, contentWidth);

        bottom = minimum.y - spacing;
        if (bottom <= margin) {
            break;
        }
    }
}

game::FeatureSettings AppController::BuildFeatureSettings() const {
    game::FeatureSettings settings{};
    settings.visual = model_.visual;
    settings.loot = model_.loot;
    settings.radar = model_.radar;
    settings.aim = model_.aim;
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
    options.cloudLayout = options_.cloudLayout;
    options.algorithmPosition = options_.algorithmPosition;
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
            AppendLog(status.message.empty()
                ? "运行模块发生错误"
                : std::string("运行错误: ") + status.message);
            break;
        }
    } else if (!status.message.empty() && status.message != lastStatus_.message) {
        AppendLog(status.message);
    } else if (status.phase == game::RuntimePhase::Running &&
               status.message.empty() && !lastStatus_.message.empty()) {
        AppendLog("数据链已恢复");
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
    if (model_.visual.enabled && model_.visual.debugInfo) {
        DrawDebugInfo(frame, drawList);
    }
}

void AppController::DrawPopulation(const game::GameFrame& frame,
                                   ImDrawList* drawList) {
    if (drawList == nullptr || model_.runtime.screenWidth <= 1 ||
        model_.runtime.screenHeight <= 1) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const RenderStyle& style = renderer_.Style();
    ImFont* font = ImGui::GetFont();
    if (font == nullptr) {
        return;
    }
    populationDrawnThisFrame_ = true;

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

    char statusText[64]{};
    std::snprintf(
        statusText, sizeof(statusText),
        "玩家 %d  ·  人机 %d", frame.playerCount, frame.botCount);
    const float fontSize = std::clamp(20.0f * scale, 16.0f, 27.0f);
    const ImVec2 textSize = font->CalcTextSizeA(
        fontSize,
        std::numeric_limits<float>::max(),
        0.0f,
        statusText);
    const float paddingX = 18.0f * scale;
    const float paddingY = 8.0f * scale;
    const float dotRadius = 4.0f * scale;
    const float contentGap = 11.0f * scale;
    const float panelHeight = std::ceil(textSize.y + paddingY * 2.0f);
    const float maximumWidth = std::max(
        1.0f, screenWidth - safeMarginX * 2.0f);
    const float minimumWidth = std::min(
        maximumWidth, panelHeight * 2.4f);
    const float targetWidth = std::clamp(
        textSize.x + paddingX * 2.0f + dotRadius * 2.0f + contentGap,
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
    static int debugPopulationLogCount = 0;
    if (debugPopulationLogCount < 1 || pressedNow || releasedNow) {
        std::fprintf(
            stderr,
            "[touch-debug] island bounds=(%.1f,%.1f)-(%.1f,%.1f) pos=(%.1f,%.1f) hovered=%d pressed=%d released=%d pressActive=%d visible=%d\n",
            panelMinimum.x,
            panelMinimum.y,
            panelMaximum.x,
            panelMaximum.y,
            io.MousePos.x,
            io.MousePos.y,
            hovered ? 1 : 0,
            pressedNow ? 1 : 0,
            releasedNow ? 1 : 0,
            populationPressActive_ ? 1 : 0,
            model_.visible ? 1 : 0);
        std::fflush(stderr);
        ++debugPopulationLogCount;
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

    const float rounding = panelHeight * 0.5f;
    const float opacity = std::clamp(populationAlpha_, 0.0f, 1.0f);
    const ImU32 panelColor = BlendColor(
        style.colors.surfaceRaised,
        style.colors.surfaceSoft,
        populationHover_ * 0.34f + populationPress_ * 0.20f);
    const ImU32 borderColor = BlendColor(
        style.colors.border,
        style.colors.accent,
        std::clamp(
            populationHover_ * 0.52f +
                populationPulse_ * 0.78f +
                populationPress_ * 0.24f,
            0.0f,
            1.0f));

    drawList->AddRectFilled(
        ImVec2(panelMinimum.x + 2.0f * scale,
               panelMinimum.y + 4.0f * scale),
        ImVec2(panelMaximum.x + 2.0f * scale,
               panelMaximum.y + 4.0f * scale),
        WithOpacity(style.colors.shadow, opacity * 0.62f),
        rounding);
    drawList->AddRectFilled(
        panelMinimum,
        panelMaximum,
        WithOpacity(panelColor, opacity),
        rounding);
    if (populationPulse_ > 0.01f) {
        drawList->AddRect(
            panelMinimum,
            panelMaximum,
            WithOpacity(
                style.colors.accent,
                opacity * populationPulse_ * 0.68f),
            rounding,
            0,
            std::max(1.0f, (1.0f + populationPulse_ * 1.8f) * scale));
    }
    drawList->AddRect(
        panelMinimum,
        panelMaximum,
        WithOpacity(borderColor, opacity),
        rounding,
        0,
        std::max(1.0f, 1.25f * scale));

    const float popFontSize =
        fontSize * (1.0f + populationPulse_ * 0.065f);
    const ImVec2 popTextSize = font->CalcTextSizeA(
        popFontSize,
        std::numeric_limits<float>::max(),
        0.0f,
        statusText);
    const float groupWidth =
        dotRadius * 2.0f + contentGap + popTextSize.x;
    const float contentStart = centerX - groupWidth * 0.5f;
    const float contentOffsetY =
        populationPress_ * scale - populationPulse_ * 1.5f * scale;
    const ImVec2 dotCenter{
        contentStart + dotRadius,
        panelMinimum.y + panelHeight * 0.5f + contentOffsetY};
    drawList->PushClipRect(panelMinimum, panelMaximum, true);
    drawList->AddCircleFilled(
        dotCenter,
        dotRadius + 3.0f * scale,
        WithOpacity(
            style.colors.accent,
            opacity * (0.08f + populationPulse_ * 0.16f)));
    drawList->AddCircleFilled(
        dotCenter,
        dotRadius,
        WithOpacity(style.colors.accent, opacity));
    const ImVec2 textPosition{
        contentStart + dotRadius * 2.0f + contentGap,
        panelMinimum.y +
            (panelHeight - popTextSize.y) * 0.5f +
            contentOffsetY};
    drawList->AddText(
        font,
        popFontSize,
        ImVec2(textPosition.x + scale, textPosition.y + scale),
        WithOpacity(style.colors.shadow, opacity * 0.52f),
        statusText);
    drawList->AddText(
        font,
        popFontSize,
        textPosition,
        WithOpacity(style.colors.text, opacity),
        statusText);
    drawList->PopClipRect();

    const float indicatorWidth = std::min(
        34.0f * scale, populationWidth_ * 0.20f);
    drawList->AddLine(
        ImVec2(centerX - indicatorWidth * 0.5f,
               panelMaximum.y - 3.0f * scale),
        ImVec2(centerX + indicatorWidth * 0.5f,
               panelMaximum.y - 3.0f * scale),
        WithOpacity(
            style.colors.accent,
            opacity * (0.32f +
                       populationHover_ * 0.36f +
                       populationPress_ * 0.16f)),
        std::max(1.0f, 1.4f * scale));
}

void AppController::DrawDebugInfo(const game::GameFrame& frame,
                                  ImDrawList* drawList) const {
    if (drawList == nullptr) return;

    std::array<std::array<char, 112>, 8> buffers{};
    std::snprintf(
        buffers[0].data(), buffers[0].size(), "FPS %.1f",
        model_.runtime.framesPerSecond);
    std::snprintf(
        buffers[1].data(), buffers[1].size(), "PID %d  BASE %s",
        model_.runtime.processId, model_.runtime.baseReady ? "OK" : "--");
    std::snprintf(
        buffers[2].data(), buffers[2].size(), "SEQ %llu",
        static_cast<unsigned long long>(frame.sequence));
    std::snprintf(
        buffers[3].data(), buffers[3].size(), "PLAYER %d  BOT %d  NEAR %d",
        frame.playerCount, frame.botCount, frame.nearbyEnemyCount);
    std::snprintf(
        buffers[4].data(), buffers[4].size(), "LABEL %zu  SIGNAL %zu  OBJECT %zu",
        frame.worldLabels.size(), frame.playerSignals.size(), frame.projectiles.size());
    std::snprintf(
        buffers[5].data(), buffers[5].size(),
        "GEO %s  MESH %zu  TRI %zu  GEN %llu",
        frame.geometryAvailable ? "OK" : "--",
        frame.geometryMeshCount,
        frame.geometryTriangleCount,
        static_cast<unsigned long long>(frame.geometryGeneration));
    std::snprintf(
        buffers[6].data(), buffers[6].size(),
        "COORD REQ %s  ENTRY %s  CTX %s  TID %d",
        model_.runtime.coordinateRequested ? "YES" : "NO",
        model_.runtime.coordinateEntryReady ? "OK" : "--",
        model_.runtime.coordinateContextReady ? "OK" : "--",
        model_.runtime.coordinateThreadId);
    std::snprintf(
        buffers[7].data(), buffers[7].size(),
        "COORD PC %llX  GEN %llu  RUN %llu/%llu",
        static_cast<unsigned long long>(
            model_.runtime.coordinateGuestPc),
        static_cast<unsigned long long>(
            model_.runtime.coordinateContextGeneration),
        static_cast<unsigned long long>(
            model_.runtime.coordinateSuccesses),
        static_cast<unsigned long long>(
            model_.runtime.coordinateAttempts));

    const RenderStyle& style = renderer_.Style();
    ImFont* font = ImGui::GetFont();
    const float fontSize = std::max(11.0f, style.metrics.smallFontSize);
    const float lineHeight = fontSize + 4.0f;
    float contentWidth = 0.0f;
    for (const auto& buffer : buffers) {
        contentWidth = std::max(
            contentWidth,
            font->CalcTextSizeA(fontSize, 1000.0f, 0.0f, buffer.data()).x);
    }

    constexpr float paddingX = 12.0f;
    constexpr float paddingY = 9.0f;
    const ImVec2 minimum{18.0f, 18.0f};
    const ImVec2 maximum{
        minimum.x + contentWidth + paddingX * 2.0f,
        minimum.y + paddingY * 2.0f + lineHeight * buffers.size(),
    };
    drawList->AddRectFilled(
        ImVec2(minimum.x + 2.0f, minimum.y + 3.0f),
        ImVec2(maximum.x + 2.0f, maximum.y + 3.0f),
        style.colors.shadow, style.metrics.panelRounding);
    drawList->AddRectFilled(
        minimum, maximum, style.colors.surfaceRaised, style.metrics.panelRounding);
    drawList->AddRect(
        minimum, maximum, style.colors.border, style.metrics.panelRounding,
        0, 1.0f);
    drawList->AddLine(
        ImVec2(minimum.x, minimum.y),
        ImVec2(minimum.x, maximum.y),
        style.colors.accent, 3.0f);
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        drawList->AddText(
            font, fontSize,
            ImVec2(
                minimum.x + paddingX,
                minimum.y + paddingY + lineHeight * static_cast<float>(index)),
            index == 0 ? style.colors.accent : style.colors.textMuted,
            buffers[index].data());
    }
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
