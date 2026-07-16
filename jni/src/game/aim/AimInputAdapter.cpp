#include "game/aim/AimInputAdapter.h"

#include "Android_touch/TouchHelperA.h"
#include "ProgramMotionChannel.h"
#include "game/native/KernelModuleLoader.h"
#include "paradise/paradise_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>

namespace lengjing::game::aim {
namespace {

constexpr int kTouchSlotCount = 10;

int NormalizeOrientation(int orientation) {
    return ((orientation % 4) + 4) % 4;
}

void ApplyOrientation(int orientation, float& pitch, float& yaw) {
    switch (NormalizeOrientation(orientation)) {
        case 0:
            pitch = -pitch;
            break;
        case 1:
            break;
        case 2:
            yaw = -yaw;
            break;
        default:
            pitch = -pitch;
            yaw = -yaw;
            break;
    }
}

}  // namespace

struct AimInputAdapter::Impl {
    ui::AimInputMode mode = ui::AimInputMode::WriteTouch;
    bool started = false;
    int width = 0;
    int height = 0;
    int orientation = 0;
    bool kernelTouchReady = false;
    std::array<bool, kTouchSlotCount> activeSlots{};
    std::unique_ptr<paradise_driver> kernel;
    ProgramMotionChannel program;

    bool IsTouchMode() const noexcept {
        return mode == ui::AimInputMode::WriteTouch ||
            mode == ui::AimInputMode::KernelTouch;
    }

    bool SendRawTouch(int action, int slot, int x, int y) {
        if (mode == ui::AimInputMode::WriteTouch) {
            if (action == 2) return Touch_Up(slot);
            if (action == 0) return Touch_Down(slot, x, y);
            return action == 1 && Touch_Move(slot, x, y);
        }
        if (mode != ui::AimInputMode::KernelTouch ||
            kernel == nullptr || !kernelTouchReady) {
            return false;
        }
        if (action == 2) return kernel->touch_up(slot);
        if (action == 0) return kernel->touch_down(slot, x, y);
        return action == 1 && kernel->touch_move(slot, x, y);
    }

    void MapKernelTouch(double x, double y, int& outputX, int& outputY) const {
        double mappedX = x;
        double mappedY = y;
        int maximumX = std::max(0, width);
        int maximumY = std::max(0, height);
        switch (orientation) {
            case 1:
                mappedX = static_cast<double>(height) - y;
                mappedY = x;
                maximumX = std::max(0, height);
                maximumY = std::max(0, width);
                break;
            case 2:
                mappedX = static_cast<double>(width) - x;
                mappedY = static_cast<double>(height) - y;
                break;
            case 3:
                mappedX = y;
                mappedY = static_cast<double>(width) - x;
                maximumX = std::max(0, height);
                maximumY = std::max(0, width);
                break;
            default:
                break;
        }
        outputX = std::clamp(
            static_cast<int>(std::lround(mappedX)), 0, maximumX);
        outputY = std::clamp(
            static_cast<int>(std::lround(mappedY)), 0, maximumY);
    }
};

AimInputAdapter::AimInputAdapter() : impl_(std::make_unique<Impl>()) {}

AimInputAdapter::~AimInputAdapter() {
    Stop();
}

bool AimInputAdapter::Start(ui::AimInputMode mode) {
    Stop();
    if (impl_ == nullptr) return false;
    impl_->mode = mode;

    switch (mode) {
        case ui::AimInputMode::WriteTouch:
            StopTouchScreen();
            if (!TouchScreenHandle(1)) {
                TouchScreenHandle(0);
                return false;
            }
            break;
        case ui::AimInputMode::ProgramGyroscope:
            if (!impl_->program.Start()) return false;
            break;
        case ui::AimInputMode::KernelTouch: {
            std::string error;
            if (!native::EnsureKernelDriverReady(error)) return false;
            impl_->kernel = std::make_unique<paradise_driver>();
            break;
        }
        case ui::AimInputMode::KernelGyroscope: {
            std::string error;
            if (!native::EnsureKernelDriverReady(error)) return false;
            impl_->kernel = std::make_unique<paradise_driver>();
            if (impl_->kernel == nullptr ||
                !impl_->kernel->gyro_update(
                    0.0f, 0.0f, PARADISE_GYRO_MASK_ALL, false)) {
                impl_->kernel.reset();
                return false;
            }
            break;
        }
        default:
            return false;
    }

    impl_->started = true;
    return true;
}

void AimInputAdapter::Stop() noexcept {
    if (impl_ == nullptr) return;
    ReleaseOutput();
    if (impl_->started && impl_->mode == ui::AimInputMode::WriteTouch) {
        StopTouchScreen();
        TouchScreenHandle(0);
    }
    if (impl_->kernelTouchReady && impl_->kernel != nullptr) {
        impl_->kernel->touch_destroy();
    }
    impl_->program.Close();
    impl_->kernel.reset();
    impl_->kernelTouchReady = false;
    impl_->activeSlots.fill(false);
    impl_->width = 0;
    impl_->height = 0;
    impl_->orientation = 0;
    impl_->mode = ui::AimInputMode::WriteTouch;
    impl_->started = false;
}

bool AimInputAdapter::ConfigureDisplay(
    int width,
    int height,
    int orientation) {
    if (impl_ == nullptr || !impl_->started || width <= 1 || height <= 1) {
        return false;
    }
    const int normalizedOrientation = NormalizeOrientation(orientation);
    if (impl_->mode == ui::AimInputMode::WriteTouch) {
        ConfigureTouchDisplay(width, height, normalizedOrientation);
    }
    if (impl_->mode == ui::AimInputMode::KernelTouch &&
        (!impl_->kernelTouchReady ||
         impl_->width != width || impl_->height != height)) {
        if (impl_->kernel == nullptr) return false;
        if (impl_->kernelTouchReady) {
            for (int slot = 0; slot < kTouchSlotCount; ++slot) {
                if (impl_->activeSlots[static_cast<std::size_t>(slot)]) {
                    impl_->kernel->touch_up(slot);
                }
            }
            impl_->activeSlots.fill(false);
            impl_->kernel->touch_destroy();
            impl_->kernelTouchReady = false;
        }
        impl_->kernelTouchReady = impl_->kernel->touch_init(width, height);
        if (!impl_->kernelTouchReady) return false;
    }
    impl_->width = width;
    impl_->height = height;
    impl_->orientation = normalizedOrientation;
    return true;
}

bool AimInputAdapter::SendTouch(
    int action,
    int slot,
    double x,
    double y) {
    if (impl_ == nullptr || !impl_->started || !impl_->IsTouchMode() ||
        slot < 0 || slot >= kTouchSlotCount || action < 0 || action > 2 ||
        !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }

    int touchX = static_cast<int>(std::lround(x));
    int touchY = static_cast<int>(std::lround(y));
    if (impl_->mode == ui::AimInputMode::KernelTouch && action != 2) {
        impl_->MapKernelTouch(x, y, touchX, touchY);
    }
    const bool sent = impl_->SendRawTouch(action, slot, touchX, touchY);
    if (sent) {
        impl_->activeSlots[static_cast<std::size_t>(slot)] = action != 2;
    }
    return sent;
}

bool AimInputAdapter::SendGyroscope(
    float pitch,
    float yaw,
    int orientation) {
    if (impl_ == nullptr || !impl_->started ||
        !std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }
    ApplyOrientation(orientation, pitch, yaw);
    if (impl_->mode == ui::AimInputMode::ProgramGyroscope) {
        return impl_->program.Send(pitch, yaw);
    }
    if (impl_->mode == ui::AimInputMode::KernelGyroscope &&
        impl_->kernel != nullptr) {
        return impl_->kernel->gyro_update(
            -yaw, -pitch, PARADISE_GYRO_MASK_ALL, true);
    }
    return false;
}

void AimInputAdapter::ReleaseOutput() noexcept {
    if (impl_ == nullptr) return;
    if (impl_->IsTouchMode()) {
        for (int slot = 0; slot < kTouchSlotCount; ++slot) {
            if (impl_->activeSlots[static_cast<std::size_t>(slot)]) {
                impl_->SendRawTouch(2, slot, 0, 0);
            }
        }
        impl_->activeSlots.fill(false);
        return;
    }
    if (impl_->mode == ui::AimInputMode::ProgramGyroscope) {
        impl_->program.Release();
    } else if (
        impl_->mode == ui::AimInputMode::KernelGyroscope &&
        impl_->kernel != nullptr) {
        impl_->kernel->gyro_update(
            0.0f, 0.0f, PARADISE_GYRO_MASK_ALL, false);
    }
}

bool AimInputAdapter::IsTouchMode() const noexcept {
    return impl_ != nullptr && impl_->started && impl_->IsTouchMode();
}

}  // namespace lengjing::game::aim
