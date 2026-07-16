#pragma once

#include "ui/UiActions.h"
#include "ui/UiModel.h"

namespace lengjing::ui {

class MenuView final {
public:
    void SetLogoTexture(void* texture) noexcept;
    void RequestRecenter() noexcept;
    void ClearTopOverlayBounds() noexcept;
    void SetTopOverlayBounds(
        float minimumX,
        float minimumY,
        float maximumX,
        float maximumY,
        float layoutMaximumY) noexcept;
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
    bool topOverlayValid_ = false;
    float topOverlayMinimumX_ = 0.0f;
    float topOverlayMinimumY_ = 0.0f;
    float topOverlayMaximumX_ = 0.0f;
    float topOverlayMaximumY_ = 0.0f;
    float topOverlayLayoutMaximumY_ = 0.0f;
    bool animationAnchorValid_ = false;
    float animationAnchorMinimumX_ = 0.0f;
    float animationAnchorMinimumY_ = 0.0f;
    float animationAnchorMaximumX_ = 0.0f;
    float animationAnchorMaximumY_ = 0.0f;
};

}  // namespace lengjing::ui
