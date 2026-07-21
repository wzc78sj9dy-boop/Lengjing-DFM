#include "game/aim/AimInputAdapter.h"

#include "Android_touch/TouchHelperA.h"
#include "Android_touch/TouchTransform.h"
#include "ProgramMotionChannel.h"
#include "game/aim/GyroscopeDirectionPolicy.h"
#include "game/native/KernelModuleLoader.h"
#include "paradise/paradise_api.h"

#include <algorithm>
#include <array>
#include <chrono>
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

}  // namespace

struct AimInputAdapter::Impl {
    ui::AimInputMode mode = ui::AimInputMode::ReadOnly;
    bool started = false;
    int width = 0;
    int height = 0;
    int orientation = 0;
    bool writeTouchReady = false;
    bool kernelTouchReady = false;
    std::array<bool, kTouchSlotCount> activeSlots{};
    std::unique_ptr<paradise_driver> kernel;
    ProgramMotionChannel program;
    std::chrono::steady_clock::time_point nextTouchStartAttempt{};
    std::chrono::steady_clock::time_point nextKernelTouchStartAttempt{};
    std::chrono::steady_clock::time_point nextProgramStartAttempt{};

    bool IsTouchMode() const noexcept {
        return mode == ui::AimInputMode::WriteTouch ||
            mode == ui::AimInputMode::KernelTouch;
    }

    bool SendRawTouch(int action, int slot, int x, int y) {
        if (mode == ui::AimInputMode::WriteTouch) {
            if (!writeTouchReady) return false;
            const bool sent = action == 2
                ? Touch_Up(slot)
                : action == 0
                    ? Touch_Down(slot, x, y)
                    : action == 1 && Touch_Move(slot, x, y);
            if (!sent && !IsTouchWriteReady()) {
                writeTouchReady = false;
                nextTouchStartAttempt =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            return sent;
        }
        if (mode != ui::AimInputMode::KernelTouch ||
            kernel == nullptr || !kernelTouchReady) {
            return false;
        }
        const bool sent = action == 2
            ? kernel->touch_up(slot)
            : action == 0
                ? kernel->touch_down(slot, x, y)
                : action == 1 && kernel->touch_move(slot, x, y);
        if (!sent) {
            kernel->touch_destroy();
            kernelTouchReady = false;
            activeSlots.fill(false);
            nextKernelTouchStartAttempt =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
        return sent;
    }

    void MapKernelTouch(double x, double y, int& outputX, int& outputY) const {
        const touch::PixelPoint mapped = touch::DisplayToNaturalPixels(
            x, y, width, height, orientation);
        outputX = std::clamp(
            static_cast<int>(std::lround(mapped.x)), 0, mapped.maximumX);
        outputY = std::clamp(
            static_cast<int>(std::lround(mapped.y)), 0, mapped.maximumY);
    }

    bool SendProgramMotion(float pitch, float yaw) {
        if (program.Send(pitch, yaw)) return true;

        return PrepareProgramMotion() && program.Send(pitch, yaw);
    }

    bool PrepareProgramMotion() {
        if (program.Ready()) return true;
        const auto now = std::chrono::steady_clock::now();
        if (now < nextProgramStartAttempt) return false;
        nextProgramStartAttempt = now + std::chrono::seconds(2);
        return program.Start();
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
        case ui::AimInputMode::ReadOnly:
            break;
        case ui::AimInputMode::WriteTouch:
            StopTouchScreen();
            impl_->writeTouchReady = TouchScreenHandle(1);
            if (!impl_->writeTouchReady) {
                impl_->nextTouchStartAttempt =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
            }
            break;
        case ui::AimInputMode::ProgramGyroscope:
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
    impl_->writeTouchReady = false;
    impl_->kernelTouchReady = false;
    impl_->activeSlots.fill(false);
    impl_->width = 0;
    impl_->height = 0;
    impl_->orientation = 0;
    impl_->nextTouchStartAttempt = {};
    impl_->nextKernelTouchStartAttempt = {};
    impl_->nextProgramStartAttempt = {};
    impl_->mode = ui::AimInputMode::ReadOnly;
    impl_->started = false;
}

bool AimInputAdapter::PrepareGyroscope() {
    if (impl_ == nullptr || !impl_->started) return false;
    if (impl_->mode == ui::AimInputMode::ProgramGyroscope) {
        return impl_->PrepareProgramMotion();
    }
    return impl_->mode == ui::AimInputMode::KernelGyroscope &&
        impl_->kernel != nullptr;
}

bool AimInputAdapter::ConfigureDisplay(
    int width,
    int height,
    int orientation) {
    if (impl_ == nullptr || !impl_->started || width <= 1 || height <= 1) {
        return false;
    }
    const int normalizedOrientation = NormalizeOrientation(orientation);
    bool releasedForGeometryChange = false;
    if (impl_->IsTouchMode() && impl_->width > 0 && impl_->height > 0 &&
        (impl_->width != width || impl_->height != height ||
         impl_->orientation != normalizedOrientation)) {
        for (int slot = 0; slot < kTouchSlotCount; ++slot) {
            if (!impl_->activeSlots[static_cast<std::size_t>(slot)]) continue;
            impl_->SendRawTouch(2, slot, 0, 0);
            releasedForGeometryChange = true;
        }
        impl_->activeSlots.fill(false);
    }
    if (impl_->mode == ui::AimInputMode::WriteTouch) {
        ConfigureTouchDisplay(width, height, normalizedOrientation);
        if (!impl_->writeTouchReady) {
            const auto now = std::chrono::steady_clock::now();
            if (now < impl_->nextTouchStartAttempt) return false;
            impl_->nextTouchStartAttempt = now + std::chrono::seconds(2);
            StopTouchScreen();
            impl_->writeTouchReady = TouchScreenHandle(1);
            if (!impl_->writeTouchReady) return false;
        }
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
            impl_->nextKernelTouchStartAttempt = {};
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < impl_->nextKernelTouchStartAttempt) return false;
        impl_->kernelTouchReady = impl_->kernel->touch_init(width, height);
        if (!impl_->kernelTouchReady) {
            impl_->nextKernelTouchStartAttempt =
                now + std::chrono::seconds(2);
            return false;
        }
        impl_->nextKernelTouchStartAttempt = {};
    }
    impl_->width = width;
    impl_->height = height;
    impl_->orientation = normalizedOrientation;
    return !releasedForGeometryChange;
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
    const std::size_t slotIndex = static_cast<std::size_t>(slot);
    if (action == 2 && !impl_->activeSlots[slotIndex]) return true;
    if (impl_->mode == ui::AimInputMode::KernelTouch && action != 2) {
        impl_->MapKernelTouch(x, y, touchX, touchY);
    }
    const bool sent = impl_->SendRawTouch(action, slot, touchX, touchY);
    if (sent) {
        impl_->activeSlots[slotIndex] = action != 2;
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
    const GyroscopeDirection direction =
        ResolveGyroscopeDirection(orientation, pitch, yaw);
    pitch = direction.pitch;
    yaw = direction.yaw;
    if (impl_->mode == ui::AimInputMode::ProgramGyroscope) {
        return impl_->SendProgramMotion(pitch, yaw);
    }
    if (impl_->mode == ui::AimInputMode::KernelGyroscope &&
        impl_->kernel != nullptr) {
        const KernelGyroscopeCommand command =
            ResolveKernelGyroscopeCommand(direction);
        return impl_->kernel->gyro_update(
            command.x, command.y, PARADISE_GYRO_MASK_ALL, true);
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
