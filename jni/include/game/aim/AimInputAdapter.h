#pragma once

#include "ui/UiModel.h"

#include <memory>

namespace lengjing::game::aim {

class AimInputAdapter final {
public:
    AimInputAdapter();
    ~AimInputAdapter();

    AimInputAdapter(const AimInputAdapter&) = delete;
    AimInputAdapter& operator=(const AimInputAdapter&) = delete;

    bool Start(ui::AimInputMode mode);
    void Stop() noexcept;
    bool ConfigureDisplay(int width, int height, int orientation);
    bool SendTouch(int action, int slot, double x, double y);
    bool SendGyroscope(float pitch, float yaw, int orientation);
    void ReleaseOutput() noexcept;
    bool IsTouchMode() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lengjing::game::aim
