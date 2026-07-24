#pragma once

#include "auth/CloudLayout.h"
#include "auth/CoordinatePoolCloudLayout.h"
#include "game/CoordinateDecryptDiagnostics.h"
#include "game/FeatureSettings.h"
#include "game/GameFrame.h"
#include "game/RuntimeDiagnostics.h"
#include "game/native/AlgorithmCoordinateDiagnostics.h"
#include "game/native/AlgorithmPositionRuntime.h"
#include "game/native/RuntimeCoordinateCodec.h"

#include <cstdint>
#include <memory>
#include <string>

namespace lengjing::game {

enum class RuntimeFailureKind : std::uint8_t {
    None,
    CloudLayoutRejected,
};

struct RuntimeOptions {
    int gameVersionIndex = 0;
    int driverIndex = 0;
    ui::AimInputMode inputMode = ui::AimInputMode::ReadOnly;
    int screenWidth = 0;
    int screenHeight = 0;
    int orientation = 0;
    std::string programDirectory;
    std::shared_ptr<const auth::CloudLayoutDocument> cloudLayout;
    std::shared_ptr<const auth::CoordinatePoolCloudLayoutDocument>
        coordinateDecrypt2Layout;
    native::AlgorithmPositionRuntimeConfig algorithmPosition;
};

struct RuntimeProbe {
    int processId = 0;
    bool baseReady = false;
    std::size_t customItemCount = 0;
    bool coordinateRequested = false;
    bool coordinateEntryReady = false;
    bool coordinateContextReady = false;
    int coordinateThreadId = 0;
    std::uintptr_t coordinateGuestPc = 0;
    std::uint64_t coordinateContextGeneration = 0;
    std::uint64_t coordinateAttempts = 0;
    std::uint64_t coordinateSuccesses = 0;
    CoordinateDecryptError coordinateError = CoordinateDecryptError::None;
    int coordinateSystemError = 0;
    CoordinateReadDiagnostic coordinateRead{};
    CoordinatePoolPointerDiagnostic coordinatePoolPointer{};
    CoordinateEntryDiagnostic coordinateEntry{};
    bool algorithmCoordinateRequested = false;
    bool algorithmCoordinateActive = false;
    bool algorithmCoordinateTableReady = false;
    bool algorithmCoordinateRuntimeReady = false;
    std::uint64_t algorithmCoordinateRefreshes = 0;
    std::uint64_t algorithmCoordinateResolveAttempts = 0;
    std::uint64_t algorithmCoordinateResolveSuccesses = 0;
    std::uint64_t algorithmCoordinateAttempts = 0;
    std::uint64_t algorithmCoordinateSuccesses = 0;
    std::uint64_t algorithmCoordinateObjectAttempts = 0;
    std::uint64_t algorithmCoordinateObjectSuccesses = 0;
    std::uint64_t algorithmCoordinateTableAttempts = 0;
    std::uint64_t algorithmCoordinateTableSuccesses = 0;
    std::uint64_t algorithmCoordinateFallbacks = 0;
    native::AlgorithmCoordinateSource algorithmCoordinateSource =
        native::AlgorithmCoordinateSource::None;
    native::AlgorithmCoordinateDiagnostic algorithmCoordinate{};
    native::RuntimeCoordinateCodecDiagnostic algorithmCoordinateRuntime{};
    RuntimeError runtimeError = RuntimeError::None;
    int runtimeSystemError = 0;
    RuntimeFailureKind failureKind = RuntimeFailureKind::None;
};

class GameBackend {
public:
    virtual ~GameBackend() = default;

    virtual bool Open(const RuntimeOptions& options,
                      RuntimeProbe& probe,
                      std::string& error) = 0;
    virtual bool Close() noexcept = 0;
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
