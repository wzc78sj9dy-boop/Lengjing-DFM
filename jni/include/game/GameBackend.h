#pragma once

#include "game/FeatureSettings.h"
#include "game/GameFrame.h"

#include <memory>
#include <string>

namespace lengjing::game {

struct RuntimeOptions {
    int gameVersionIndex = 0;
    int driverIndex = 0;
    ui::AimInputMode inputMode = ui::AimInputMode::WriteTouch;
    int screenWidth = 0;
    int screenHeight = 0;
    int orientation = 0;
    std::string programDirectory;
};

struct RuntimeProbe {
    int processId = 0;
    bool baseReady = false;
    std::size_t customItemCount = 0;
};

class GameBackend {
public:
    virtual ~GameBackend() = default;

    virtual bool Open(const RuntimeOptions& options,
                      RuntimeProbe& probe,
                      std::string& error) = 0;
    virtual void Close() noexcept = 0;
    virtual bool ReadFrame(const FeatureSettings& settings,
                           GameFrame& frame,
                           RuntimeProbe& probe,
                           std::string& error) = 0;
    virtual void UpdateDisplayGeometry(
        int width, int height, int orientation) = 0;
    virtual void SetAimEnabled(bool enabled) = 0;
    virtual void ReloadCustomItems() = 0;
};

std::unique_ptr<GameBackend> CreateNativeGameBackend();

}  // namespace lengjing::game
