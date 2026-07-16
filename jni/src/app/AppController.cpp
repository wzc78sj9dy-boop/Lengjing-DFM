#include "app/AppController.h"

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
    runtime_.SetAimEnabled(model_.aim.enabled);
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

    menuView_.ClearTopOverlayBounds();
    if (model_.runtime.active) {
        const std::shared_ptr<const game::GameFrame> frame = runtime_.LatestFrame();
        if (frame != nullptr) {
            DrawGameFrame(*frame, ImGui::GetBackgroundDrawList());
        }
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
    if (normalizedOrientation != displayOrientation_) {
        menuView_.RequestRecenter();
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
    runtime_.SetAimEnabled(model_.aim.enabled);
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

void AppController::AimEnabledChanged(bool enabled) {
    runtime_.SetAimEnabled(enabled);
}

void AppController::SettingsChanged(ui::SettingsDomain domain) {
    SyncToastSetting();
    runtime_.UpdateSettings(BuildFeatureSettings());
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
            runtime_.SetAimEnabled(model_.aim.enabled);
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
    const float screenWidth = static_cast<float>(model_.runtime.screenWidth);
    const float screenHeight = static_cast<float>(model_.runtime.screenHeight);
    const float scale = std::clamp(
        std::min(screenWidth, screenHeight) / 1080.0f, 0.78f, 1.35f);
    const float centerX = screenWidth * 0.5f;
    const float targetTop = model_.visible
        ? std::max(6.0f, 8.0f * scale)
        : std::max(18.0f, 22.0f * scale);
    const float layoutTop = std::max(6.0f, 8.0f * scale);
    if (populationTop_ < 0.0f) {
        populationTop_ = targetTop;
    }
    populationTop_ = Approach(
        populationTop_, targetTop, 12.0f, io.DeltaTime);
    const float top = populationTop_;
    const float maximumWidth = std::max(1.0f, screenWidth - 32.0f * scale);
    const float compactWidth = std::min(maximumWidth, 420.0f * scale);
    const float compactHeight = 72.0f * scale;
    const ImVec2 panelMinimum{
        centerX - compactWidth * 0.5f, top};
    const ImVec2 panelMaximum{
        centerX + compactWidth * 0.5f, top + compactHeight};
    menuView_.SetTopOverlayBounds(
        panelMinimum.x,
        panelMinimum.y,
        panelMaximum.x,
        panelMaximum.y,
        layoutTop + compactHeight);

    const bool hovered =
        io.MousePos.x >= panelMinimum.x && io.MousePos.x <= panelMaximum.x &&
        io.MousePos.y >= panelMinimum.y && io.MousePos.y <= panelMaximum.y;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        populationPressActive_ = true;
    }
    if (populationPressActive_ && !hovered) {
        populationPressActive_ = false;
    }
    if (populationPressActive_ &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (hovered) {
            model_.visible = !model_.visible;
        }
        populationPressActive_ = false;
    }
    populationPress_ = Approach(
        populationPress_, populationPressActive_ ? 1.0f : 0.0f,
        populationPressActive_ ? 26.0f : 14.0f, io.DeltaTime);
    populationHover_ = Approach(
        populationHover_, hovered ? 1.0f : 0.0f, 14.0f, io.DeltaTime);

    if (lastPlayerCount_ != frame.playerCount ||
        lastBotCount_ != frame.botCount) {
        if (lastPlayerCount_ >= 0 && lastBotCount_ >= 0) {
            populationPulse_ = 1.0f;
        }
        lastPlayerCount_ = frame.playerCount;
        lastBotCount_ = frame.botCount;
    }
    populationPulse_ = Approach(
        populationPulse_, 0.0f, 4.0f, io.DeltaTime);

    const float rounding = compactHeight * 0.5f;
    const ImU32 panelColor = BlendColor(
        style.colors.surfaceRaised, style.colors.surfaceSoft,
        populationHover_ * 0.45f + populationPress_ * 0.15f);
    const ImU32 borderColor = BlendColor(
        style.colors.border, style.colors.accent,
        populationHover_ * 0.75f +
            populationPulse_ * 0.25f +
            populationPress_ * 0.18f);

    drawList->AddRectFilled(
        ImVec2(panelMinimum.x + 3.0f * scale, panelMinimum.y + 6.0f * scale),
        ImVec2(panelMaximum.x + 3.0f * scale, panelMaximum.y + 6.0f * scale),
        WithOpacity(style.colors.shadow, 0.72f), rounding);
    if (populationHover_ > 0.01f || populationPulse_ > 0.01f) {
        drawList->AddRect(
            ImVec2(panelMinimum.x - 3.0f * scale, panelMinimum.y - 3.0f * scale),
            ImVec2(panelMaximum.x + 3.0f * scale, panelMaximum.y + 3.0f * scale),
            WithOpacity(
                style.colors.accent,
                populationHover_ * 0.22f + populationPulse_ * 0.34f),
            rounding + 3.0f * scale, 0, 3.0f * scale);
    }
    drawList->AddRectFilled(
        panelMinimum, panelMaximum, panelColor, rounding);
    drawList->AddRect(
        panelMinimum, panelMaximum, borderColor, rounding,
        0, std::max(1.0f, 1.4f * scale));

    const float dotRadius =
        (5.0f + populationPulse_ * 2.5f) * scale;
    const ImVec2 dotCenter{
        panelMinimum.x + 30.0f * scale,
        panelMinimum.y + compactHeight * 0.5f,
    };
    drawList->AddCircleFilled(
        dotCenter, dotRadius + 5.0f * scale,
        WithOpacity(style.colors.accent, 0.11f + populationPulse_ * 0.16f));
    drawList->AddCircleFilled(dotCenter, dotRadius, style.colors.accent);

    ImFont* font = ImGui::GetFont();
    char compactText[64]{};
    std::snprintf(
        compactText, sizeof(compactText),
        "玩家 %d  ·  人机 %d", frame.playerCount, frame.botCount);
    const float fontSize = 20.0f * scale;
    const ImVec2 textSize = font->CalcTextSizeA(
        fontSize, maximumWidth, 0.0f, compactText);
    drawList->AddText(
        font, fontSize,
        ImVec2(
            centerX - textSize.x * 0.5f,
            panelMinimum.y + (compactHeight - textSize.y) * 0.5f),
        style.colors.text, compactText);

    const float indicatorWidth = 52.0f * scale;
    drawList->AddLine(
        ImVec2(centerX - indicatorWidth * 0.5f, panelMaximum.y - 5.0f * scale),
        ImVec2(centerX + indicatorWidth * 0.5f, panelMaximum.y - 5.0f * scale),
        WithOpacity(style.colors.accent, 0.55f + populationHover_ * 0.35f),
        2.0f * scale);
}

void AppController::DrawDebugInfo(const game::GameFrame& frame,
                                  ImDrawList* drawList) const {
    if (drawList == nullptr) return;

    std::array<std::array<char, 112>, 6> buffers{};
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
