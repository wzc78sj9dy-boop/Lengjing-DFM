#include "game/aim/AimController.h"

#include "game/aim/AimPrediction.h"
#include "game/aim/AimInputAdapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace lengjing::game::aim {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kTouchSlot = 8;

float WrapDegrees(float degrees) {
    while (degrees > 180.0f) degrees -= 360.0f;
    while (degrees < -180.0f) degrees += 360.0f;
    return degrees;
}

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

struct AimError {
    float pitch = 0.0f;
    float yaw = 0.0f;
};

bool CalculateAimError(const TargetSnapshot& target, AimError& output) {
    const Vec3 predicted = PredictAimPoint(target);
    const float dx = predicted.x - target.view.location.x;
    const float dy = predicted.y - target.view.location.y;
    const float dz = predicted.z - target.view.location.z;
    const float horizontal = std::hypot(dx, dy);
    if (!std::isfinite(horizontal) || horizontal <= 0.01f) {
        return false;
    }
    const float targetPitch = std::atan2(dz, horizontal) * 180.0f / kPi;
    const float targetYaw = std::atan2(dy, dx) * 180.0f / kPi;
    if (target.view.halfWidth <= 0.5f ||
        !std::isfinite(target.view.fieldOfView)) {
        return false;
    }
    const float smoothing = std::max(0.01f, target.tuning.smoothing);
    const float divisor = smoothing * 100.0f;
    const float scale = (target.view.halfWidth * 2.0f /
        std::clamp(target.view.fieldOfView, 5.0f, 170.0f)) / divisor;
    output.pitch = WrapDegrees(targetPitch - target.view.pitch) * scale;
    output.yaw = WrapDegrees(targetYaw - target.view.yaw) * scale;
    return std::isfinite(output.pitch) && std::isfinite(output.yaw);
}

class StepIntegrator final {
public:
    std::pair<int, int> Step(float x, float y, float maximumX, float maximumY) {
        const float length = std::hypot(x, y);
        if (!std::isfinite(length) || length <= 0.0001f) {
            Reset();
            return {};
        }
        const float denominator = std::sqrt(length * length + length);
        residualX_ += std::clamp(x / denominator * std::fabs(maximumX),
                                 -std::fabs(x), std::fabs(x));
        residualY_ += std::clamp(y / denominator * std::fabs(maximumY),
                                 -std::fabs(y), std::fabs(y));
        const int wholeX = static_cast<int>(std::trunc(residualX_));
        const int wholeY = static_cast<int>(std::trunc(residualY_));
        residualX_ -= wholeX;
        residualY_ -= wholeY;
        return {wholeX, wholeY};
    }

    void Reset() {
        residualX_ = 0.0;
        residualY_ = 0.0;
    }

private:
    double residualX_ = 0.0;
    double residualY_ = 0.0;
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
    StepIntegrator integrator;

    const auto release = [&] {
        if (touchDown) DispatchTouch(2, touchX, touchY);
        if (gyroscopeActive && input_ != nullptr) input_->ReleaseOutput();
        touchDown = false;
        gyroscopeActive = false;
        touchX = 0.0;
        touchY = 0.0;
        identity = 0;
        integrator.Reset();
    };

    while (!stopRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(1ms);
        const TargetSnapshot target = LoadTarget();
        if (!enabled_.load(std::memory_order_acquire) || !TargetAllowed(target)) {
            release();
            continue;
        }

        AimError error{};
        if (!CalculateAimError(target, error)) {
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
        if (identity != target.identity ||
            (touchMode && touchDown &&
             (std::fabs(anchorX - newAnchorX) > 1.0f ||
              std::fabs(anchorY - newAnchorY) > 1.0f))) {
            release();
            identity = target.identity;
            initialDistance = std::max(target.screenDistancePixels, 1.0f);
            curveSign = (identity & 1U) == 0U ? 1.0f : -1.0f;
        }
        if (touchMode) {
            anchorX = newAnchorX;
            anchorY = newAnchorY;
            if (!touchDown) {
                touchX = anchorX;
                touchY = anchorY;
                touchDown = DispatchTouch(0, touchX, touchY);
                if (!touchDown) {
                    release();
                    continue;
                }
            }
        }

        if (target.curvedMotion) {
            const float length = std::hypot(error.pitch, error.yaw);
            if (length > 0.001f) {
                const float progress = std::clamp(
                    target.screenDistancePixels / initialDistance, 0.0f, 1.0f);
                const float bend = std::sin(progress * kPi) *
                    std::min(initialDistance * 0.01f, 3.0f) * curveSign;
                const float pitch = error.pitch;
                error.pitch += -error.yaw / length * bend;
                error.yaw += pitch / length * bend;
            }
        }
        const float stateScale = ActiveSpeed(target) / 30.0f;
        if (!touchMode) {
            const float length = std::hypot(error.pitch, error.yaw);
            const float denominator = std::sqrt(length * length + length);
            if (!std::isfinite(denominator) || denominator <= 0.0001f) {
                release();
                continue;
            }
            const float pitchLimit =
                std::fabs(target.tuning.verticalSpeed * 0.01f * stateScale);
            const float yawLimit =
                std::fabs(target.tuning.horizontalSpeed * 0.005f * stateScale);
            const float pitchStep = std::clamp(
                error.pitch / denominator * pitchLimit,
                -std::fabs(error.pitch),
                std::fabs(error.pitch));
            const float yawStep = std::clamp(
                error.yaw / denominator * yawLimit,
                -std::fabs(error.yaw),
                std::fabs(error.yaw));
            if (!input_->SendGyroscope(
                    pitchStep, yawStep, target.orientation)) {
                release();
                continue;
            }
            gyroscopeActive = true;
            continue;
        }

        const auto step = integrator.Step(
            error.yaw,
            -error.pitch,
            target.tuning.horizontalSpeed * 0.005f * stateScale,
            target.tuning.verticalSpeed * 0.01f * stateScale);
        touchX += step.first;
        touchY += step.second;

        const float maximumRange = std::max(
            target.view.halfWidth * 2.0f,
            target.view.halfHeight * 2.0f);
        const float range = std::clamp(target.touchRange, 20.0f, maximumRange);
        if (std::fabs(touchX - anchorX) >= range ||
            std::fabs(touchY - anchorY) >= range) {
            DispatchTouch(2, touchX, touchY);
            touchX = anchorX;
            touchY = anchorY;
            integrator.Reset();
            touchDown = DispatchTouch(0, touchX, touchY);
        }
        if (touchDown && !DispatchTouch(1, touchX, touchY)) release();
    }
    release();
    running_.store(false, std::memory_order_release);
}

}  // namespace lengjing::game::aim
