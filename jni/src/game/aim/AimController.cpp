#include "game/aim/AimController.h"

#include "game/aim/AimInputAdapter.h"
#include "game/aim/AimPrediction.h"
#include "game/aim/GyroscopeDirectionPolicy.h"
#include "game/aim/TouchMotionPolicy.h"
#include "game/native/FrameProjection.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace lengjing::game::aim {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kTouchSlot = 8;

bool Finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Triggered(const TargetSnapshot& target) {
    if (!target.valid) return false;
    switch (target.triggerMode) {
        case 0: return target.firing;
        case 1: return target.zooming;
        case 2: return target.firing || target.zooming;
        default: return false;
    }
}

float ActiveSpeed(const TargetSnapshot& target) {
    const float speed = target.zooming
        ? target.tuning.adsSpeed
        : target.tuning.hipSpeed;
    return std::clamp(
        std::isfinite(speed) ? speed : 30.0f, 1.0f, 100.0f);
}

bool TargetAllowed(const TargetSnapshot& target) {
    if (!Triggered(target) || !Finite(target.world) || !Finite(target.view.location) ||
        !std::isfinite(target.screenDistancePixels) ||
        !std::isfinite(target.worldDistanceMeters)) {
        return false;
    }
    if (target.enforceFov &&
        target.screenDistancePixels >
            std::max(0.0f, target.tuning.rangePixels)) {
        return false;
    }
    const float distanceLimit = target.zooming
        ? target.tuning.adsDistanceMeters
        : target.tuning.hipDistanceMeters;
    return !target.enforceDistance ||
        target.worldDistanceMeters <= std::max(0.0f, distanceLimit);
}

bool ProjectTarget(
    const TargetSnapshot& target,
    int screenWidth,
    int screenHeight,
    native::ScreenProjection& output) {
    const Vec3 predicted = PredictAimPoint(target);
    const native::ProjectionPoint point{
        predicted.x,
        predicted.y,
        predicted.z,
    };
    const native::ProjectionView view{
        {target.view.location.x,
         target.view.location.y,
         target.view.location.z},
        {target.view.pitch, target.view.yaw, target.view.roll},
        target.view.fieldOfView,
    };
    output = native::ProjectWorldPoint(point, view, screenWidth, screenHeight);
    return output.valid && output.camera.forward > 0.001f;
}

float RandomTouchOffset() {
    const float magnitude = static_cast<float>(std::rand() % 16 + 5);
    return std::rand() % 2 == 0 ? magnitude : -magnitude;
}

class TouchLoopDelay final {
public:
    TouchLoopDelay(bool enabled, float speed, float smoothing)
        : enabled_(enabled), speed_(speed), smoothing_(smoothing) {}

    ~TouchLoopDelay() {
        if (!enabled_ || !std::isfinite(speed_) ||
            !std::isfinite(smoothing_)) {
            return;
        }
        const float delay = speed_ * std::max(smoothing_, 0.0f) * 100.0f;
        if (delay > 0.0f) {
            std::this_thread::sleep_for(std::chrono::microseconds(
                static_cast<std::int64_t>(delay)));
        }
    }

private:
    bool enabled_ = false;
    float speed_ = 0.0f;
    float smoothing_ = 0.0f;
};

}  // namespace

AimController::~AimController() {
    Stop();
}

AimController::AimController() : input_(std::make_unique<AimInputAdapter>()) {}

bool AimController::Start(ui::AimInputMode mode) {
    Stop();
    mode_ = mode;
    if (input_ == nullptr || !input_->Start(mode_)) {
        mode_ = ui::AimInputMode::ReadOnly;
        return false;
    }
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    if (mode_ == ui::AimInputMode::ReadOnly) {
        return true;
    }
    try {
        worker_ = std::thread(&AimController::WorkerMain, this);
    } catch (...) {
        running_.store(false, std::memory_order_release);
        input_->Stop();
        mode_ = ui::AimInputMode::ReadOnly;
        return false;
    }
    return true;
}

void AimController::Stop() {
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) worker_.join();
    running_.store(false, std::memory_order_release);
    enabled_.store(false, std::memory_order_release);
    if (input_ != nullptr) input_->Stop();
    mode_ = ui::AimInputMode::ReadOnly;
    ClearTarget();
}

void AimController::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled) ClearTarget();
}

void AimController::Publish(const TargetSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(mutex_);
    target_ = snapshot;
}

void AimController::ClearTarget() {
    std::lock_guard<std::mutex> lock(mutex_);
    target_ = TargetSnapshot{};
}

bool AimController::Running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

TargetSnapshot AimController::LoadTarget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return target_;
}

bool AimController::DispatchTouch(int action, double x, double y) {
    return input_ != nullptr &&
        input_->SendTouch(action, kTouchSlot, x, y);
}

void AimController::WorkerMain() {
    using namespace std::chrono_literals;
    const bool touchMode = input_ != nullptr && input_->IsTouchMode();
    bool touchDown = false;
    bool gyroscopeActive = false;
    double touchX = 0.0;
    double touchY = 0.0;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    std::uint64_t identity = 0;
    float initialDistance = 1.0f;
    float curveSign = 1.0f;
    bool curveInitialized = false;

    const auto release = [&] {
        if (touchDown) DispatchTouch(2, touchX, touchY);
        if (gyroscopeActive && input_ != nullptr) input_->ReleaseOutput();
        touchDown = false;
        gyroscopeActive = false;
        touchX = 0.0;
        touchY = 0.0;
        identity = 0;
        curveInitialized = false;
    };

    while (mode_ == ui::AimInputMode::ProgramGyroscope &&
           !stopRequested_.load(std::memory_order_acquire) &&
           (input_ == nullptr || !input_->PrepareGyroscope())) {
        for (int attempt = 0; attempt < 20 &&
             !stopRequested_.load(std::memory_order_acquire); ++attempt) {
            std::this_thread::sleep_for(100ms);
        }
    }

    while (!stopRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(1000000 / 120));
        const TargetSnapshot target = LoadTarget();
        const float activeSpeed = ActiveSpeed(target);
        const TouchLoopDelay touchDelay(
            touchMode, activeSpeed, target.tuning.smoothing);
        if (!enabled_.load(std::memory_order_acquire) || !TargetAllowed(target)) {
            release();
            continue;
        }

        const float screenWidth = target.view.halfWidth * 2.0f;
        const float screenHeight = target.view.halfHeight * 2.0f;
        const float newAnchorX = target.touchCenterX;
        const float newAnchorY = target.touchCenterY;
        if (screenWidth <= 1.0f || screenHeight <= 1.0f ||
            (touchMode && (!std::isfinite(newAnchorX) ||
                           !std::isfinite(newAnchorY))) ||
            input_ == nullptr ||
            !input_->ConfigureDisplay(
                static_cast<int>(std::lround(screenWidth)),
                static_cast<int>(std::lround(screenHeight)),
                target.orientation)) {
            release();
            continue;
        }

        native::ScreenProjection projected{};
        if (!ProjectTarget(
                target,
                static_cast<int>(std::lround(screenWidth)),
                static_cast<int>(std::lround(screenHeight)),
                projected)) {
            release();
            continue;
        }
        constexpr float kProjectionBoundary = 50.0f;
        if (projected.x < -kProjectionBoundary ||
            projected.x > screenWidth + kProjectionBoundary ||
            projected.y < -kProjectionBoundary ||
            projected.y > screenHeight + kProjectionBoundary) {
            release();
            continue;
        }

        const float offsetX = projected.x - screenWidth * 0.5f;
        const float offsetY = projected.y - screenHeight * 0.5f;
        if (identity != target.identity ||
            (touchMode && touchDown &&
             (std::fabs(anchorX - newAnchorX) > 1.0f ||
              std::fabs(anchorY - newAnchorY) > 1.0f))) {
            release();
            identity = target.identity;
        }
        if (touchMode) {
            anchorX = newAnchorX;
            anchorY = newAnchorY;
            if (!touchDown) {
                touchX = anchorX + RandomTouchOffset();
                touchY = anchorY + RandomTouchOffset();
                touchDown = DispatchTouch(0, touchX, touchY);
                if (!touchDown) {
                    release();
                    continue;
                }
            }
            const TouchMotionStep step = ResolveTouchScreenStep(
                offsetX, offsetY, activeSpeed, screenWidth, screenHeight);
            touchX += step.x;
            touchY += step.y;

            const float range = std::max(target.touchRange, 0.0f);
            if (std::fabs(touchX - anchorX) >= range ||
                std::fabs(touchY - anchorY) >= range) {
                DispatchTouch(2, touchX, touchY);
                touchX = anchorX + RandomTouchOffset();
                touchY = anchorY + RandomTouchOffset();
                curveInitialized = false;
                touchDown = DispatchTouch(0, touchX, touchY);
                if (!touchDown) {
                    release();
                    continue;
                }
            }

            double outputX = touchX;
            double outputY = touchY;
            if (target.curvedMotion) {
                const float distance = std::hypot(offsetX, offsetY);
                if (!curveInitialized || distance > initialDistance * 1.5f) {
                    curveSign = std::rand() % 2 == 0 ? 1.0f : -1.0f;
                    initialDistance = std::max(distance, 1.0f);
                    curveInitialized = true;
                }
                const float progress = initialDistance > 1.0f
                    ? std::min(distance / initialDistance, 1.0f)
                    : 0.0f;
                const float bend = std::sin(progress * kPi) *
                    std::min(initialDistance * 0.08f, 20.0f) * curveSign;
                if (distance > 1.0f) {
                    outputX += -offsetY / distance * bend;
                    outputY += offsetX / distance * bend;
                }
                if (distance < 3.0f) curveInitialized = false;
            } else {
                curveInitialized = false;
            }
            if (!DispatchTouch(1, outputX, outputY)) release();
            continue;
        }

        if (std::fabs(offsetX) < 1.5f && std::fabs(offsetY) < 1.5f) {
            release();
            continue;
        }
        const GyroscopeDirection motion = ResolveGyroscopeScreenMotion(
            offsetX,
            offsetY,
            activeSpeed,
            target.tuning.smoothing,
            target.orientation);
        if (!input_->SendGyroscope(
                motion.pitch, motion.yaw, target.orientation)) {
            release();
            continue;
        }
        gyroscopeActive = true;
    }
    release();
    running_.store(false, std::memory_order_release);
}

}  // namespace lengjing::game::aim
