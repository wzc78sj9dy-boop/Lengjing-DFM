#pragma once

#include "ui/UiActions.h"
#include "ui/UiModel.h"

namespace lengjing::ui {

class MenuView final {
public:
    void SetLogoTexture(void* texture) noexcept;
    void Render(UiModel& model, UiActions& actions);

private:
    void* logoTexture_ = nullptr;
    bool positionInitialized_ = false;
    bool dragActive_ = false;
    float windowX_ = 0.0f;
    float windowY_ = 0.0f;
    float dragOffsetX_ = 0.0f;
    float dragOffsetY_ = 0.0f;
    float lastDisplayWidth_ = 0.0f;
    float lastDisplayHeight_ = 0.0f;
    Page animatedPage_ = Page::Runtime;
    float windowAnimation_ = 0.0f;
    float pageAnimation_ = 1.0f;
    bool wasVisible_ = false;
    bool pageStateInitialized_ = false;
};

}  // namespace lengjing::ui
