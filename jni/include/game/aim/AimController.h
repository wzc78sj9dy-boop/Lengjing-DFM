#pragma once

#include "ui/UiModel.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace lengjing::game::aim {

class AimInputAdapter;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct ViewState {
    Vec3 location{};
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    float fieldOfView = 90.0f;
    float halfWidth = 0.0f;
    float halfHeight = 0.0f;
};

struct TargetSnapshot {
    bool valid = false;
    std::uint64_t identity = 0;
    Vec3 world{};
    Vec3 velocity{};
    float screenDistancePixels = 0.0f;
    float worldDistanceMeters = 0.0f;
    float projectileSpeedCmPerSecond = 90000.0f;
    bool firing = false;
    bool zooming = false;
    bool curvedMotion = false;
    int triggerMode = 0;
    int orientation = 0;
    float touchRange = 300.0f;
    float touchCenterX = 1450.0f;
    float touchCenterY = 380.0f;
    ui::AimTuning tuning{};
    ViewState view{};
};

class AimController final {
public:
    AimController();
    ~AimController();

    AimController(const AimController&) = delete;
    AimController& operator=(const AimController&) = delete;

    bool Start(ui::AimInputMode mode);
    void Stop();
    void SetEnabled(bool enabled);
    void Publish(const TargetSnapshot& snapshot);
    void ClearTarget();
    bool Running() const noexcept;

private:
    void WorkerMain();
    TargetSnapshot LoadTarget() const;
    bool DispatchTouch(int action, double x, double y);

    mutable std::mutex mutex_;
    TargetSnapshot target_{};
    ui::AimInputMode mode_ = ui::AimInputMode::WriteTouch;
    std::atomic_bool stopRequested_{true};
    std::atomic_bool enabled_{false};
    std::atomic_bool running_{false};
    std::thread worker_;
    std::unique_ptr<AimInputAdapter> input_;
};

}  // namespace lengjing::game::aim
