#include "render/overlay_renderer.h"
#include "render/OverlayContrastPolicy.h"
#include "render/PlayerTracerPolicy.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace lengjing {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float Clamp01(float value) {
    if (!std::isfinite(value)) return 0.0f;
    return std::clamp(value, 0.0f, 1.0f);
}

float SafeScale(float value) {
    if (!std::isfinite(value)) return 1.0f;
    return std::clamp(value, 0.5f, 2.5f);
}

bool Finite(const ImVec2& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

ImVec2 Add(const ImVec2& left, const ImVec2& right) {
    return ImVec2(left.x + right.x, left.y + right.y);
}

ImVec2 Subtract(const ImVec2& left, const ImVec2& right) {
    return ImVec2(left.x - right.x, left.y - right.y);
}

ImVec2 Multiply(const ImVec2& value, float scale) {
    return ImVec2(value.x * scale, value.y * scale);
}

float Length(const ImVec2& value) {
    return std::sqrt(value.x * value.x + value.y * value.y);
}

ImVec2 Normalize(const ImVec2& value) {
    const float length = Length(value);
    if (!std::isfinite(length) || length <= 0.0001f) return ImVec2(0.0f, -1.0f);
    return Multiply(value, 1.0f / length);
}

ImVec2 Perpendicular(const ImVec2& value) {
    return ImVec2(-value.y, value.x);
}

ImVec2 Lerp(const ImVec2& first, const ImVec2& second, float amount) {
    const float t = Clamp01(amount);
    return ImVec2(
        first.x + (second.x - first.x) * t,
        first.y + (second.y - first.y) * t);
}

ImU32 WithAlpha(ImU32 color, float factor) {
    const unsigned int alpha = (color >> IM_COL32_A_SHIFT) & 0xffU;
    const unsigned int scaled = static_cast<unsigned int>(
        std::clamp(static_cast<float>(alpha) * Clamp01(factor), 0.0f, 255.0f));
    return (color & ~(0xffU << IM_COL32_A_SHIFT)) | (scaled << IM_COL32_A_SHIFT);
}

ImU32 PlayerColor(const RenderStyle& style,
                  SemanticTone tone,
                  bool visible) {
    switch (tone) {
        case SemanticTone::Accent:
            return style.colors.accent;
        case SemanticTone::Caution:
            return style.colors.caution;
        case SemanticTone::Danger:
            return style.colors.danger;
        case SemanticTone::Ally:
            return style.colors.ally;
        case SemanticTone::Muted:
            return style.colors.textMuted;
        case SemanticTone::Neutral:
        default:
            return visible ? style.colors.text : style.colors.textMuted;
    }
}

float ClampFinite(float value, float minimum, float maximum) {
    if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
        return std::isfinite(value) ? value : 0.0f;
    }
    if (minimum > maximum) {
        return (minimum + maximum) * 0.5f;
    }
    if (!std::isfinite(value)) {
        return (minimum + maximum) * 0.5f;
    }
    return std::clamp(value, minimum, maximum);
}

ImVec2 ClampPoint(const ImVec2& point, const ScreenRect& viewport, float margin) {
    if (!viewport.IsValid()) return point;
    const float maximumMargin =
        std::min(viewport.Width(), viewport.Height()) * 0.5f;
    const float safeMargin = std::min(
        std::isfinite(margin) ? std::max(0.0f, margin) : 0.0f,
        maximumMargin);
    return ImVec2(
        ClampFinite(point.x,
                    viewport.left + safeMargin,
                    viewport.right - safeMargin),
        ClampFinite(point.y,
                    viewport.top + safeMargin,
                    viewport.bottom - safeMargin));
}

void DrawText(ImDrawList* drawList,
              const ImVec2& position,
              ImU32 color,
              ImU32 shadow,
              float fontSize,
              const std::string& text) {
    if (drawList == nullptr || text.empty() || !Finite(position)) return;
    const float outlineOffset = render::TextOutlineOffset(fontSize);
    const ImU32 outlineColor = render::WithMinimumAlpha(
        shadow, render::kTextOutlineMinimumAlpha);
    const ImU32 textColor = render::WithMinimumAlpha(
        color, render::kTextMinimumAlpha);
    drawList->AddText(
        nullptr,
        fontSize,
        Add(position, ImVec2(outlineOffset, outlineOffset)),
        outlineColor,
        text.c_str());
    drawList->AddText(nullptr, fontSize, position, textColor, text.c_str());
}

ImVec2 TextExtent(const std::string& text, float fontSize) {
    if (text.empty()) return ImVec2(0.0f, 0.0f);
    ImFont* font = ImGui::GetFont();
    if (font == nullptr) return ImVec2(0.0f, 0.0f);
    return font->CalcTextSizeA(
        fontSize,
        std::numeric_limits<float>::max(),
        -1.0f,
        text.c_str());
}

std::string FormatDistance(float meters) {
    if (!std::isfinite(meters) || meters < 0.0f) return {};
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%.0fm", meters);
    return buffer;
}

std::string FormatValue(int value) {
    char buffer[32] = {};
    const int safeValue = std::max(value, 0);
    if (safeValue >= 1000000) {
        std::snprintf(buffer, sizeof(buffer), "%.1fM", safeValue / 1000000.0f);
    } else if (safeValue >= 1000) {
        std::snprintf(buffer, sizeof(buffer), "%.1fK", safeValue / 1000.0f);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%d", safeValue);
    }
    return buffer;
}

struct FittedText {
    std::string text;
    ImVec2 extent{};
};

FittedText FitTextWithExtent(const std::string& text,
                             float maximumWidth,
                             float fontSize) {
    if (text.empty() || maximumWidth <= 0.0f) return {};
    const ImVec2 originalExtent = TextExtent(text, fontSize);
    if (originalExtent.x <= maximumWidth) {
        return FittedText{text, originalExtent};
    }

    constexpr char suffix[] = "...";
    const ImVec2 suffixExtent = TextExtent(suffix, fontSize);
    if (suffixExtent.x >= maximumWidth) return {};

    std::size_t accepted = 0;
    ImVec2 acceptedExtent = suffixExtent;
    for (std::size_t offset = 0; offset < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t sequenceLength = 1;
        if ((lead & 0xe0U) == 0xc0U) sequenceLength = 2;
        else if ((lead & 0xf0U) == 0xe0U) sequenceLength = 3;
        else if ((lead & 0xf8U) == 0xf0U) sequenceLength = 4;
        const std::size_t next = std::min(text.size(), offset + sequenceLength);
        const std::string candidate = text.substr(0, next) + suffix;
        const ImVec2 candidateExtent = TextExtent(candidate, fontSize);
        if (candidateExtent.x > maximumWidth) break;
        accepted = next;
        acceptedExtent = candidateExtent;
        offset = next;
    }
    return FittedText{
        text.substr(0, accepted) + suffix,
        acceptedExtent,
    };
}

std::string FitText(const std::string& text,
                    float maximumWidth,
                    float fontSize) {
    return FitTextWithExtent(text, maximumWidth, fontSize).text;
}

void DrawOutlinedLine(ImDrawList* drawList,
                      const ImVec2& first,
                      const ImVec2& second,
                      ImU32 color,
                      ImU32 shadow,
                      float width,
                      float outlineWidth) {
    if (drawList == nullptr || !Finite(first) || !Finite(second)) return;
    drawList->AddLine(
        first,
        second,
        render::WithMinimumAlpha(shadow, 120),
        std::max(outlineWidth, width));
    drawList->AddLine(
        first,
        second,
        render::WithExactAlpha(color, render::kSolidAlpha),
        width);
}

void DrawDashedLine(ImDrawList* drawList,
                    const ImVec2& first,
                    const ImVec2& second,
                    ImU32 color,
                    ImU32 shadow,
                    float width,
                    float scale,
                    int maximumSegments = 9,
                    float duty = 0.58f) {
    if (drawList == nullptr || !Finite(first) || !Finite(second)) return;
    const float length = Length(Subtract(second, first));
    if (!std::isfinite(length) || length <= 1.0f) return;
    const float safeScale = SafeScale(scale);
    const int segmentCount = std::clamp(
        static_cast<int>(std::ceil(length / (24.0f * safeScale))),
        2,
        std::max(2, maximumSegments));
    const float visibleDuty = std::clamp(duty, 0.25f, 0.85f);
    const float outline = std::max(width + safeScale, width * 1.55f);
    for (int index = 0; index < segmentCount; ++index) {
        const float start = static_cast<float>(index) /
            static_cast<float>(segmentCount);
        const float end = std::min(
            1.0f,
            (static_cast<float>(index) + visibleDuty) /
                static_cast<float>(segmentCount));
        DrawOutlinedLine(
            drawList,
            Lerp(first, second, start),
            Lerp(first, second, end),
            color,
            shadow,
            width,
            outline);
    }
}

void DrawDiamond(ImDrawList* drawList,
                 const ImVec2& center,
                 float radius,
                 ImU32 color,
                 float width,
                 bool filled) {
    if (drawList == nullptr || !Finite(center) || !std::isfinite(radius) ||
        radius <= 0.0f) {
        return;
    }
    const std::array<ImVec2, 4> points{{
        ImVec2(center.x, center.y - radius),
        ImVec2(center.x + radius, center.y),
        ImVec2(center.x, center.y + radius),
        ImVec2(center.x - radius, center.y),
    }};
    if (filled) {
        drawList->AddConvexPolyFilled(
            points.data(), static_cast<int>(points.size()), color);
    } else {
        drawList->AddPolyline(
            points.data(),
            static_cast<int>(points.size()),
            color,
            ImDrawFlags_Closed,
            std::max(0.5f, width));
    }
}

void DrawArc(ImDrawList* drawList,
             const ImVec2& center,
             float radius,
             float startRadians,
             float endRadians,
             ImU32 color,
             float width,
             int segments) {
    if (drawList == nullptr || !Finite(center) || !std::isfinite(radius) ||
        radius <= 0.0f || !std::isfinite(startRadians) ||
        !std::isfinite(endRadians)) {
        return;
    }
    const int count = std::clamp(segments, 2, 32);
    ImVec2 previous(
        center.x + std::cos(startRadians) * radius,
        center.y + std::sin(startRadians) * radius);
    for (int index = 1; index <= count; ++index) {
        const float amount =
            static_cast<float>(index) / static_cast<float>(count);
        const float angle =
            startRadians + (endRadians - startRadians) * amount;
        const ImVec2 current(
            center.x + std::cos(angle) * radius,
            center.y + std::sin(angle) * radius);
        drawList->AddLine(previous, current, color, std::max(0.5f, width));
        previous = current;
    }
}

void DrawTickedRing(ImDrawList* drawList,
                    const ImVec2& center,
                    float radius,
                    ImU32 color,
                    float width,
                    int arcSegments = 8) {
    constexpr float kQuarter = kPi * 0.5f;
    constexpr float kGap = 0.16f;
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        const float start = quadrant * kQuarter + kGap;
        const float end = (quadrant + 1) * kQuarter - kGap;
        DrawArc(
            drawList,
            center,
            radius,
            start,
            end,
            color,
            width,
            arcSegments);
    }
}

void DrawPanel(ImDrawList* drawList,
               const ImVec2& minimum,
               const ImVec2& maximum,
               ImU32 surface,
               ImU32 border,
               ImU32 shadow,
               float rounding,
               float scale) {
    if (drawList == nullptr || !Finite(minimum) || !Finite(maximum) ||
        maximum.x <= minimum.x || maximum.y <= minimum.y) {
        return;
    }
    const float safeScale = SafeScale(scale);
    drawList->AddRectFilled(
        Add(minimum, ImVec2(1.5f * safeScale, 2.5f * safeScale)),
        Add(maximum, ImVec2(1.5f * safeScale, 2.5f * safeScale)),
        WithAlpha(shadow, 0.54f),
        rounding);
    drawList->AddRectFilled(minimum, maximum, surface, rounding);
    drawList->AddRect(
        minimum,
        maximum,
        border,
        rounding,
        0,
        std::max(1.0f, 0.9f * safeScale));
}

}  // namespace

RenderStyle RenderStyle::Default() {
    return RenderStyle{};
}

bool ScreenRect::IsValid() const {
    return std::isfinite(left) && std::isfinite(top) &&
           std::isfinite(right) && std::isfinite(bottom) &&
           right > left && bottom > top;
}

float ScreenRect::Width() const {
    return IsValid() ? right - left : 0.0f;
}

float ScreenRect::Height() const {
    return IsValid() ? bottom - top : 0.0f;
}

ImVec2 ScreenRect::Center() const {
    return ImVec2((left + right) * 0.5f, (top + bottom) * 0.5f);
}

OverlayRenderer::OverlayRenderer(RenderStyle style) : style_(style) {
    style_.metrics.scale = SafeScale(style_.metrics.scale);
}

const RenderStyle& OverlayRenderer::Style() const {
    return style_;
}

void OverlayRenderer::SetStyle(const RenderStyle& style) {
    style_ = style;
    style_.metrics.scale = SafeScale(style_.metrics.scale);
}

ImU32 OverlayRenderer::ToneColor(SemanticTone tone) const {
    switch (tone) {
        case SemanticTone::Accent: return style_.colors.accent;
        case SemanticTone::Caution: return style_.colors.caution;
        case SemanticTone::Danger: return style_.colors.danger;
        case SemanticTone::Ally: return style_.colors.ally;
        case SemanticTone::Muted: return style_.colors.textMuted;
        case SemanticTone::Neutral:
        default: return style_.colors.text;
    }
}

void OverlayRenderer::DrawPlayer(ImDrawList* drawList,
                                 const PlayerVisual& player,
                                 const ScreenRect& viewport) const {
    if (drawList == nullptr || !player.bounds.IsValid() || !viewport.IsValid()) return;
    if (player.drawTracer) {
        DrawTracer(drawList,
                   player.tracerOrigin,
                   render::TopTracerTarget(player.bounds),
                   player.tone,
                   player.visible);
    }
    if (player.drawCornerBox) {
        DrawCornerBox(
            drawList,
            player.bounds,
            player.coverHighlighted ? SemanticTone::Accent : player.tone,
            player.visible);
    }
    if (player.drawSkeleton) {
        DrawSkeleton(
            drawList,
            player.skeleton,
            player.coverHighlighted ? SemanticTone::Accent : player.tone,
            player.visible);
    }
    if (player.drawVitals) {
        DrawVitalBars(drawList, player.bounds, player.vitals);
    }
    if (player.drawPlate) {
        DrawPlayerPlate(drawList, player, viewport);
    }
}

void OverlayRenderer::DrawPlayerPlate(ImDrawList* drawList,
                                      const PlayerVisual& player,
                                      const ScreenRect& viewport) const {
    if (drawList == nullptr || !player.bounds.IsValid() || !viewport.IsValid()) return;
    const bool hasTitle = !player.name.empty();
    const bool hasDetail = !player.detail.empty();
    if (!hasTitle && !hasDetail) return;

    const float scale = style_.metrics.scale;
    const float titleSize = style_.metrics.fontSize * scale;
    const float detailSize = style_.metrics.smallFontSize * scale;
    const float margin = 6.0f * scale;
    const float paddingX = 8.0f * scale;
    const float paddingY = 5.0f * scale;
    const float accentWidth = std::max(2.0f, 2.0f * scale);
    const float textGap = hasTitle && hasDetail ? 2.0f * scale : 0.0f;
    const float maximumContentWidth = std::max(
        1.0f,
        std::min(
            250.0f * scale,
            viewport.Width() - margin * 2.0f - paddingX * 2.0f -
                accentWidth));
    const FittedText title = hasTitle
        ? FitTextWithExtent(player.name, maximumContentWidth, titleSize)
        : FittedText{};
    const FittedText detail = hasDetail
        ? FitTextWithExtent(player.detail, maximumContentWidth, detailSize)
        : FittedText{};
    const float contentWidth = std::max(title.extent.x, detail.extent.x);
    const float contentHeight =
        (title.text.empty() ? 0.0f : titleSize) +
        (detail.text.empty() ? 0.0f : detailSize) + textGap;
    if (contentWidth <= 0.0f || contentHeight <= 0.0f) return;

    const float cardWidth = std::min(
        viewport.Width() - margin * 2.0f,
        contentWidth + paddingX * 2.0f + accentWidth);
    const float cardHeight = contentHeight + paddingY * 2.0f;
    const float rightCandidate = player.bounds.right + 7.0f * scale;
    const float leftCandidate =
        player.bounds.left - cardWidth - 7.0f * scale;
    float cardLeft = rightCandidate + cardWidth <= viewport.right - margin
        ? rightCandidate
        : leftCandidate;
    cardLeft = ClampFinite(
        cardLeft,
        viewport.left + margin,
        viewport.right - margin - cardWidth);
    const float cardTop = ClampFinite(
        player.bounds.top + 2.0f * scale,
        viewport.top + margin,
        viewport.bottom - margin - cardHeight);
    const ImVec2 cardMinimum(cardLeft, cardTop);
    const ImVec2 cardMaximum(
        cardLeft + cardWidth, cardTop + cardHeight);
    const ImU32 accent = render::WithExactAlpha(
        PlayerColor(style_, player.tone, player.visible),
        render::kSolidAlpha);
    const ImU32 textShadow = WithAlpha(style_.colors.shadow, 0.62f);
    DrawPanel(
        drawList,
        cardMinimum,
        cardMaximum,
        WithAlpha(style_.colors.surfaceRaised, 0.78f),
        WithAlpha(accent, 0.58f),
        style_.colors.shadow,
        style_.metrics.panelRounding * scale,
        scale);
    drawList->AddRectFilled(
        ImVec2(cardMinimum.x, cardMinimum.y + 2.0f * scale),
        ImVec2(cardMinimum.x + accentWidth,
               cardMaximum.y - 2.0f * scale),
        accent,
        accentWidth * 0.5f);

    const float textLeft =
        cardMinimum.x + accentWidth + paddingX;
    float cursorY = cardMinimum.y + paddingY;
    if (!title.text.empty()) {
        DrawText(drawList,
                 ImVec2(textLeft, cursorY),
                 player.visible ? style_.colors.text : style_.colors.textMuted,
                 textShadow,
                 titleSize,
                 title.text);
        cursorY += titleSize + textGap;
    }
    if (!detail.text.empty()) {
        DrawText(drawList,
                 ImVec2(textLeft, cursorY),
                 style_.colors.textMuted,
                 textShadow,
                 detailSize,
                 detail.text);
    }
}

void OverlayRenderer::DrawCornerBox(ImDrawList* drawList,
                                    const ScreenRect& bounds,
                                    SemanticTone tone,
                                    bool visible) const {
    if (drawList == nullptr || !bounds.IsValid()) return;
    const float scale = style_.metrics.scale;
    const float width = render::PlayerStrokeWidth(
        style_.metrics.lineWidth, scale);
    const float outline = render::PlayerOutlineWidth(
        width, style_.metrics.outlineWidth, scale);
    const ImU32 color = render::WithExactAlpha(
        PlayerColor(style_, tone, visible), render::kSolidAlpha);
    const ImU32 shadow = render::WithExactAlpha(style_.colors.shadow, 170);
    const float rounding = std::clamp(
        std::min(bounds.Width(), bounds.Height()) * 0.08f,
        3.0f * scale,
        11.0f * scale);
    const ImVec2 minimum(bounds.left, bounds.top);
    const ImVec2 maximum(bounds.right, bounds.bottom);

    drawList->AddRectFilled(
        minimum,
        maximum,
        WithAlpha(style_.colors.surfaceSoft, visible ? 0.055f : 0.035f),
        rounding);
    drawList->AddRect(
        minimum,
        maximum,
        shadow,
        rounding,
        0,
        outline);
    drawList->AddRect(
        minimum,
        maximum,
        color,
        rounding,
        0,
        width);

    const float horizontalTick = std::min(
        style_.metrics.cornerLength * scale,
        bounds.Width() * 0.22f);
    const float verticalTick = std::min(
        style_.metrics.cornerLength * scale,
        bounds.Height() * 0.16f);
    const ImVec2 center = bounds.Center();
    const auto drawTick = [&](const ImVec2& first, const ImVec2& second) {
        DrawOutlinedLine(
            drawList, first, second, color, shadow, width, outline);
    };
    drawTick(
        ImVec2(bounds.left, center.y),
        ImVec2(bounds.left + horizontalTick, center.y));
    drawTick(
        ImVec2(bounds.right, center.y),
        ImVec2(bounds.right - horizontalTick, center.y));
    drawTick(
        ImVec2(center.x, bounds.top),
        ImVec2(center.x, bounds.top + verticalTick));
    drawTick(
        ImVec2(center.x, bounds.bottom),
        ImVec2(center.x, bounds.bottom - verticalTick));

    const float markerRadius = std::clamp(
        bounds.Width() * 0.045f, 2.5f * scale, 5.0f * scale);
    DrawDiamond(
        drawList,
        ImVec2(center.x, bounds.top),
        markerRadius,
        shadow,
        outline,
        true);
    DrawDiamond(
        drawList,
        ImVec2(center.x, bounds.top),
        markerRadius * 0.62f,
        color,
        width,
        true);
}

void OverlayRenderer::DrawSkeleton(ImDrawList* drawList,
                                   const SkeletonVisual& skeleton,
                                   SemanticTone tone,
                                   bool visible) const {
    if (drawList == nullptr || skeleton.joints.empty() || skeleton.links.empty()) return;
    const float scale = style_.metrics.scale;
    const ImU32 color = render::WithExactAlpha(
        PlayerColor(style_, tone, visible), render::kSolidAlpha);
    const float width = render::PlayerStrokeWidth(
        style_.metrics.lineWidth, scale);
    const float shadowWidth = std::max(width + scale, width * 1.45f);
    const ImU32 shadow = render::WithExactAlpha(style_.colors.shadow, 176);
    const auto endpointColor = [&](const BoneJoint& joint) {
        if (!skeleton.colorByVisibility) return color;
        switch (joint.visibility) {
            case game::VisibilityState::Visible:
                return render::WithExactAlpha(
                    style_.colors.accent, render::kSolidAlpha);
            case game::VisibilityState::Occluded:
                return render::WithExactAlpha(
                    style_.colors.danger, render::kSolidAlpha);
            case game::VisibilityState::Unavailable:
            default:
                return render::WithExactAlpha(
                    style_.colors.textMuted, render::kSolidAlpha);
        }
    };
    for (const BoneLink& link : skeleton.links) {
        if (link.first >= skeleton.joints.size() || link.second >= skeleton.joints.size()) continue;
        const BoneJoint& first = skeleton.joints[link.first];
        const BoneJoint& second = skeleton.joints[link.second];
        if (!first.valid || !second.valid || !Finite(first.position) || !Finite(second.position)) continue;
        const ImVec2 delta = Subtract(second.position, first.position);
        const float length = Length(delta);
        if (!std::isfinite(length) || length <= 1.0f) continue;
        const ImVec2 direction = Multiply(delta, 1.0f / length);
        const ImVec2 midpoint = Lerp(first.position, second.position, 0.5f);
        const float centerGap = std::min(1.8f * scale, length * 0.12f);
        const ImVec2 firstEnd =
            Subtract(midpoint, Multiply(direction, centerGap));
        const ImVec2 secondStart =
            Add(midpoint, Multiply(direction, centerGap));
        const ImU32 firstColor = endpointColor(first);
        const ImU32 secondColor = endpointColor(second);
        drawList->AddLine(
            first.position, second.position, shadow, shadowWidth);
        drawList->AddLine(
            first.position,
            firstEnd,
            render::WithExactAlpha(firstColor, render::kSolidAlpha),
            width);
        drawList->AddLine(
            secondStart,
            second.position,
            render::WithExactAlpha(secondColor, render::kSolidAlpha),
            width);
    }
    const float nodeRadius = std::max(1.4f, 1.65f * scale);
    for (const BoneJoint& joint : skeleton.joints) {
        if (!joint.valid || !Finite(joint.position)) continue;
        DrawDiamond(
            drawList,
            joint.position,
            nodeRadius,
            shadow,
            shadowWidth,
            true);
        DrawDiamond(
            drawList,
            joint.position,
            nodeRadius * 0.58f,
            endpointColor(joint),
            width,
            true);
    }
    if (skeleton.selectedJoint >= 0 &&
        static_cast<std::size_t>(skeleton.selectedJoint) <
            skeleton.joints.size()) {
        const BoneJoint& selected =
            skeleton.joints[static_cast<std::size_t>(skeleton.selectedJoint)];
        if (selected.valid && Finite(selected.position)) {
            DrawDiamond(
                drawList,
                selected.position,
                5.5f * scale,
                shadow,
                shadowWidth,
                false);
            DrawDiamond(
                drawList,
                selected.position,
                4.1f * scale,
                render::WithExactAlpha(
                    style_.colors.caution, render::kSolidAlpha),
                width,
                false);
        }
    }
}

void OverlayRenderer::DrawVitalBars(ImDrawList* drawList,
                                    const ScreenRect& bounds,
                                    const VitalState& vitals) const {
    if (drawList == nullptr || !bounds.IsValid()) return;
    const float scale = style_.metrics.scale;
    const float healthRatio = Clamp01(
        vitals.health / std::max(vitals.maxHealth, 0.001f));
    const bool hasArmorTrack = vitals.maxArmor > 0.001f;
    const float armorRatio = Clamp01(
        vitals.armor / std::max(vitals.maxArmor, 0.001f));
    const ImU32 healthColor = vitals.downed || healthRatio <= 0.33f
        ? style_.colors.danger
        : (healthRatio <= 0.66f ? style_.colors.caution : style_.colors.accent);
    const float trackWidth = std::max(bounds.Width(), 12.0f * scale);
    const float trackHeight = std::max(2.0f, 2.4f * scale);
    const float gap = std::max(1.0f, 1.2f * scale);
    const float left = bounds.Center().x - trackWidth * 0.5f;
    const float right = left + trackWidth;
    const float top = bounds.bottom + 4.0f * scale;
    const float rounding = trackHeight * 0.5f;
    const ImVec2 healthMin(left, top);
    const ImVec2 healthMax(right, top + trackHeight);
    drawList->AddRectFilled(
        healthMin,
        healthMax,
        WithAlpha(style_.colors.surface, 0.90f),
        rounding);
    if (healthRatio > 0.0f) {
        drawList->AddRectFilled(
            healthMin,
            ImVec2(left + trackWidth * healthRatio, healthMax.y),
            render::WithExactAlpha(healthColor, render::kSolidAlpha),
            rounding);
    }
    if (!hasArmorTrack) return;

    const ImVec2 armorMin(left, healthMax.y + gap);
    const ImVec2 armorMax(right, armorMin.y + trackHeight);
    drawList->AddRectFilled(
        armorMin,
        armorMax,
        WithAlpha(style_.colors.surface, 0.90f),
        rounding);
    if (armorRatio > 0.0f) {
        drawList->AddRectFilled(
            armorMin,
            ImVec2(left + trackWidth * armorRatio, armorMax.y),
            render::WithExactAlpha(
                style_.colors.ally, render::kSolidAlpha),
            rounding);
    }
}

void OverlayRenderer::DrawTracer(ImDrawList* drawList,
                                 const ImVec2& origin,
                                 const ImVec2& target,
                                 SemanticTone tone,
                                 bool visible) const {
    if (drawList == nullptr || !Finite(origin) || !Finite(target)) return;
    const float scale = style_.metrics.scale;
    const ImU32 color = PlayerColor(style_, tone, visible);
    if (Length(Subtract(target, origin)) <= 1.0f) return;
    const float width = render::PlayerStrokeWidth(
        style_.metrics.lineWidth, scale);
    DrawDashedLine(
        drawList,
        origin,
        target,
        render::WithExactAlpha(color, render::kSolidAlpha),
        render::WithExactAlpha(style_.colors.shadow, 150),
        width,
        scale,
        8,
        0.56f);
    DrawDiamond(
        drawList,
        target,
        3.0f * scale,
        render::WithExactAlpha(color, render::kSolidAlpha),
        width,
        false);
}

void OverlayRenderer::DrawPlayerSignal(ImDrawList* drawList,
                                       const PlayerSignalVisual& signal,
                                       const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() ||
        !Finite(signal.start) || !Finite(signal.end)) {
        return;
    }

    const float scale = style_.metrics.scale;
    const ImVec2 start = ClampPoint(signal.start, viewport, 3.0f * scale);
    const ImVec2 end = ClampPoint(signal.end, viewport, 3.0f * scale);
    const ImVec2 delta = Subtract(end, start);
    const float length = Length(delta);
    if (length <= 1.0f) return;

    const ImU32 base = signal.kind == PlayerSignalKind::AimWarning
        ? style_.colors.danger
        : ToneColor(signal.tone);
    const float width = render::PlayerStrokeWidth(
        style_.metrics.lineWidth, scale);
    DrawDashedLine(
        drawList,
        start,
        end,
        render::WithExactAlpha(base, render::kSolidAlpha),
        render::WithExactAlpha(style_.colors.shadow, 154),
        width,
        scale,
        signal.kind == PlayerSignalKind::AimWarning ? 10 : 7,
        signal.kind == PlayerSignalKind::AimWarning ? 0.64f : 0.46f);

    const ImVec2 direction = Normalize(delta);
    const ImVec2 side = Perpendicular(direction);
    const float markerSize =
        (signal.kind == PlayerSignalKind::AimWarning ? 5.0f : 3.2f) *
        scale;
    DrawDiamond(
        drawList,
        end,
        markerSize,
        render::WithExactAlpha(base, render::kSolidAlpha),
        width,
        signal.kind == PlayerSignalKind::AimWarning);
    if (signal.kind == PlayerSignalKind::AimWarning) {
        const ImVec2 neck = Subtract(end, Multiply(direction, 11.0f * scale));
        drawList->AddLine(
            neck,
            Add(neck, Multiply(side, 5.0f * scale)),
            base,
            width);
        drawList->AddLine(
            neck,
            Subtract(neck, Multiply(side, 5.0f * scale)),
            base,
            width);
    }
}

void OverlayRenderer::DrawModelGeometry(
    ImDrawList* drawList,
    const GeometryModelVisual& model,
    const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || model.segments.empty()) {
        return;
    }

    const float scale = style_.metrics.scale;
    const float width = std::max(
        1.0f, style_.metrics.lineWidth * 0.46f * scale);
    const ImU32 primary = WithAlpha(ToneColor(model.tone), 0.62f);
    const ImU32 secondary = WithAlpha(style_.colors.grid, 0.76f);
    for (std::size_t index = 0; index < model.segments.size(); ++index) {
        const GeometrySegmentVisual& segment = model.segments[index];
        if (!Finite(segment.start) || !Finite(segment.end)) continue;
        drawList->AddLine(
            segment.start,
            segment.end,
            index % 3 == 0 ? primary : secondary,
            width);
    }
}

void OverlayRenderer::DrawOffscreenWarning(ImDrawList* drawList,
                                           const OffscreenMarker& marker,
                                           const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(marker.direction)) return;
    const float scale = style_.metrics.scale;
    const float markerScale = ClampFinite(
        marker.markerScale,
        0.25f,
        3.0f);
    const float shapeScale = scale * markerScale;
    const ImVec2 direction = Normalize(marker.direction);
    const ImVec2 center = viewport.Center();
    const float margin = 28.0f * shapeScale;
    const float halfWidth = std::max(1.0f, viewport.Width() * 0.5f - margin);
    const float halfHeight = std::max(1.0f, viewport.Height() * 0.5f - margin);
    const float xFactor = std::fabs(direction.x) > 0.0001f
        ? halfWidth / std::fabs(direction.x)
        : std::numeric_limits<float>::max();
    const float yFactor = std::fabs(direction.y) > 0.0001f
        ? halfHeight / std::fabs(direction.y)
        : std::numeric_limits<float>::max();
    const float maximumRadius = std::min(xFactor, yFactor);
    const float requestedRadius =
        std::isfinite(marker.radiusPixels) && marker.radiusPixels > 0.0f
        ? marker.radiusPixels
        : maximumRadius;
    const float radius = ClampFinite(
        requestedRadius,
        std::min(20.0f * scale, maximumRadius),
        maximumRadius);
    const ImVec2 anchor = Add(center, Multiply(direction, radius));
    const ImVec2 side = Perpendicular(direction);
    const ImU32 color = WithAlpha(ToneColor(marker.tone), 0.96f);
    const ImU32 shadow =
        render::WithExactAlpha(style_.colors.shadow, 180);
    const float width = render::PlayerStrokeWidth(
        style_.metrics.lineWidth, scale);
    const float outline = std::max(width + scale, width * 1.5f);
    const float bracketLength = 12.0f * shapeScale;
    const float bracketWidth = 7.0f * shapeScale;
    const ImVec2 outerBase =
        Subtract(anchor, Multiply(direction, bracketLength));
    const ImVec2 outerLeft =
        Add(outerBase, Multiply(side, bracketWidth));
    const ImVec2 outerRight =
        Subtract(outerBase, Multiply(side, bracketWidth));
    DrawOutlinedLine(
        drawList, outerLeft, anchor, color, shadow, width, outline);
    DrawOutlinedLine(
        drawList, anchor, outerRight, color, shadow, width, outline);

    const ImVec2 innerTip =
        Subtract(anchor, Multiply(direction, 5.5f * shapeScale));
    const ImVec2 innerBase =
        Subtract(innerTip, Multiply(direction, 6.5f * shapeScale));
    const ImVec2 innerLeft =
        Add(innerBase, Multiply(side, 3.8f * shapeScale));
    const ImVec2 innerRight =
        Subtract(innerBase, Multiply(side, 3.8f * shapeScale));
    drawList->AddLine(innerLeft, innerTip, WithAlpha(color, 0.58f), width);
    drawList->AddLine(innerTip, innerRight, WithAlpha(color, 0.58f), width);

    std::string caption = marker.label;
    const std::string distance = marker.distanceMeters > 0.0f
        ? FormatDistance(marker.distanceMeters)
        : std::string{};
    if (!distance.empty()) {
        if (!caption.empty()) caption += "  ";
        caption += distance;
    }
    if (!caption.empty()) {
        const float fontSize = style_.metrics.smallFontSize * scale;
        const ImVec2 extent = TextExtent(caption, fontSize);
        const float availableWidth =
            std::max(0.0f, viewport.Width() - 8.0f * scale);
        const float textWidth = std::min(extent.x, availableWidth);
        if (textWidth <= 0.0f) return;
        const ImVec2 inward = Subtract(
            anchor, Multiply(direction, 29.0f * shapeScale));
        ImVec2 position(inward.x - textWidth * 0.5f,
                        inward.y - fontSize * 0.5f);
        position.x = ClampFinite(
            position.x,
            viewport.left + 4.0f * scale,
            viewport.right - textWidth - 4.0f * scale);
        position.y = ClampFinite(
            position.y,
            viewport.top + 4.0f * scale,
            viewport.bottom - fontSize - 4.0f * scale);
        DrawText(drawList,
                 position,
                 color,
                 shadow,
                 fontSize,
                 FitText(caption, textWidth, fontSize));
    }
}

void OverlayRenderer::DrawProjectile(ImDrawList* drawList,
                                     const ProjectileVisual& projectile,
                                     const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(projectile.center)) return;
    const float scale = style_.metrics.scale;
    const ImU32 color = projectile.colorOverride != 0
        ? projectile.colorOverride
        : ToneColor(projectile.tone);
    const float rangeWidth = std::max(
        1.0f, style_.metrics.lineWidth * 0.48f * scale);
    for (const GeometrySegmentVisual& segment : projectile.rangeSegments) {
        if (!Finite(segment.start) || !Finite(segment.end)) continue;
        drawList->AddLine(
            segment.start,
            Lerp(segment.start, segment.end, 0.64f),
            WithAlpha(color, 0.64f),
            rangeWidth);
    }

    if (projectile.trajectory.size() >= 2) {
        for (std::size_t index = 1; index < projectile.trajectory.size(); ++index) {
            const ImVec2& first = projectile.trajectory[index - 1];
            const ImVec2& second = projectile.trajectory[index];
            if (!Finite(first) || !Finite(second)) continue;
            drawList->AddLine(
                first,
                Lerp(first, second, 0.68f),
                WithAlpha(color, 0.78f),
                std::max(
                    1.0f,
                    style_.metrics.lineWidth * 0.48f * scale));
            if (index % 6 == 1) {
                DrawDiamond(
                    drawList,
                    second,
                    1.9f * scale,
                    WithAlpha(color, 0.80f),
                    1.0f,
                    true);
            }
        }
    }
    DrawTickedRing(
        drawList,
        projectile.center,
        7.0f * scale,
        WithAlpha(color, 0.92f),
        std::max(1.0f, style_.metrics.lineWidth * 0.55f * scale),
        3);
    DrawDiamond(
        drawList,
        projectile.center,
        2.6f * scale,
        color,
        1.0f,
        true);

    std::string caption = projectile.label;
    const std::string distance = FormatDistance(projectile.distanceMeters);
    if (!distance.empty()) {
        if (!caption.empty()) caption += "  ";
        caption += distance;
    }
    if (!caption.empty()) {
        const float fontSize = style_.metrics.fontSize * scale;
        const float margin = 6.0f * scale;
        const float paddingX = 7.0f * scale;
        const float paddingY = 4.0f * scale;
        const float maximumWidth = std::max(
            1.0f,
            std::min(
                280.0f * scale,
                viewport.Width() - margin * 2.0f - paddingX * 2.0f));
        const FittedText fitted =
            FitTextWithExtent(caption, maximumWidth, fontSize);
        if (fitted.text.empty()) return;
        const float panelWidth = fitted.extent.x + paddingX * 2.0f;
        const float panelHeight = fontSize + paddingY * 2.0f;
        const float x = ClampFinite(
            projectile.center.x - panelWidth * 0.5f,
            viewport.left + margin,
            viewport.right - margin - panelWidth);
        const float y = ClampFinite(
            projectile.center.y - panelHeight - 12.0f * scale,
            viewport.top + margin,
            viewport.bottom - margin - panelHeight);
        DrawPanel(
            drawList,
            ImVec2(x, y),
            ImVec2(x + panelWidth, y + panelHeight),
            WithAlpha(style_.colors.surfaceRaised, 0.82f),
            WithAlpha(color, 0.72f),
            style_.colors.shadow,
            style_.metrics.panelRounding * scale,
            scale);
        DrawText(drawList,
                 ImVec2(x + paddingX, y + paddingY),
                 color,
                 WithAlpha(style_.colors.shadow, 0.62f),
                 fontSize,
                 fitted.text);
    }
}

void OverlayRenderer::DrawCrosshair(ImDrawList* drawList,
                                    const CrosshairVisual& crosshair) const {
    if (drawList == nullptr || !Finite(crosshair.center)) return;
    const float scale = style_.metrics.scale;
    const float half = ClampFinite(
        crosshair.armLength,
        1.0f,
        500.0f) * scale;
    const float width = ClampFinite(
        crosshair.thickness,
        0.5f,
        20.0f) * scale;
    const float gap = std::min(
        half * 0.82f,
        ClampFinite(crosshair.gap, 0.0f, 500.0f) * scale);
    const ImU32 color = render::WithExactAlpha(
        ToneColor(crosshair.tone), render::kSolidAlpha);
    const ImU32 shadow =
        render::WithExactAlpha(style_.colors.shadow, 180);
    const float outline = std::max(width + scale, width * 1.55f);
    const ImVec2 center = crosshair.center;
    const auto drawArm = [&](const ImVec2& inner,
                             const ImVec2& outer,
                             const ImVec2& capDirection) {
        DrawOutlinedLine(
            drawList, inner, outer, color, shadow, width, outline);
        const float cap = std::max(2.0f, 2.8f * scale);
        DrawOutlinedLine(
            drawList,
            Subtract(outer, Multiply(capDirection, cap)),
            Add(outer, Multiply(capDirection, cap)),
            color,
            shadow,
            width,
            outline);
    };
    drawArm(
        ImVec2(center.x - gap, center.y),
        ImVec2(center.x - half, center.y),
        ImVec2(0.0f, 1.0f));
    drawArm(
        ImVec2(center.x + gap, center.y),
        ImVec2(center.x + half, center.y),
        ImVec2(0.0f, 1.0f));
    drawArm(
        ImVec2(center.x, center.y - gap),
        ImVec2(center.x, center.y - half),
        ImVec2(1.0f, 0.0f));
    drawArm(
        ImVec2(center.x, center.y + gap),
        ImVec2(center.x, center.y + half),
        ImVec2(1.0f, 0.0f));
    if (crosshair.centerDot) {
        DrawDiamond(
            drawList,
            center,
            std::max(1.6f, 1.9f * scale),
            shadow,
            outline,
            true);
        DrawDiamond(
            drawList,
            center,
            std::max(0.9f, 1.05f * scale),
            color,
            width,
            true);
    }
}

void OverlayRenderer::DrawAimGuide(ImDrawList* drawList,
                                   const AimGuide& guide,
                                   const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(guide.center)) return;
    const float scale = style_.metrics.scale;
    if (guide.drawCircle && std::isfinite(guide.radius) && guide.radius >= 50.0f) {
        const float radius = std::min(
            guide.radius,
            std::max(viewport.Width(), viewport.Height()));
        DrawTickedRing(
            drawList,
            guide.center,
            radius,
            WithAlpha(style_.colors.accent, 0.66f),
            std::max(1.0f, 1.15f * scale),
            18);
    }
    if (guide.drawTargetRay && guide.targetValid && Finite(guide.target)) {
        const ImVec2 target = ClampPoint(guide.target, viewport, 5.0f * scale);
        const ImU32 color = guide.locked
            ? WithAlpha(style_.colors.danger, 0.96f)
            : WithAlpha(style_.colors.caution, 0.94f);
        const float width = std::max(1.0f, 1.35f * scale);
        DrawDashedLine(
            drawList,
            guide.center,
            target,
            color,
            render::WithExactAlpha(style_.colors.shadow, 170),
            width,
            scale,
            10,
            guide.locked ? 0.68f : 0.48f);
        DrawDiamond(
            drawList,
            target,
            5.0f * scale,
            render::WithExactAlpha(style_.colors.shadow, 180),
            width + scale,
            true);
        DrawDiamond(
            drawList,
            target,
            guide.locked ? 3.4f * scale : 2.8f * scale,
            color,
            width,
            guide.locked);
    }
}

void OverlayRenderer::DrawTouchRegion(ImDrawList* drawList,
                                      const TouchRegionVisual& region,
                                      const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(region.center) ||
        !std::isfinite(region.halfExtent) || region.halfExtent <= 0.0f) {
        return;
    }
    const float scale = style_.metrics.scale;
    const float maximumExtent =
        std::min(viewport.Width(), viewport.Height()) * 0.5f;
    if (maximumExtent <= 0.0f) return;
    const float extent = ClampFinite(
        region.halfExtent,
        std::min(20.0f * scale, maximumExtent),
        maximumExtent);
    const ScreenRect bounds{
        std::max(viewport.left, region.center.x - extent),
        std::max(viewport.top, region.center.y - extent),
        std::min(viewport.right, region.center.x + extent),
        std::min(viewport.bottom, region.center.y + extent),
    };
    if (!bounds.IsValid()) return;

    const ImU32 outerColor = WithAlpha(style_.colors.ally, 0.52f);
    const ImU32 innerColor = WithAlpha(style_.colors.accent, 0.42f);
    const float width = std::max(
        1.0f, style_.metrics.lineWidth * 0.52f * scale);
    drawList->PushClipRect(
        ImVec2(viewport.left, viewport.top),
        ImVec2(viewport.right, viewport.bottom),
        true);
    DrawTickedRing(
        drawList,
        region.center,
        extent,
        outerColor,
        width,
        16);
    DrawTickedRing(
        drawList,
        region.center,
        extent * 0.36f,
        innerColor,
        width,
        8);
    const float half = 6.0f * scale;
    drawList->AddLine(
        ImVec2(region.center.x - half, region.center.y),
        ImVec2(region.center.x + half, region.center.y),
        WithAlpha(style_.colors.text, 0.62f),
        width);
    drawList->AddLine(
        ImVec2(region.center.x, region.center.y - half),
        ImVec2(region.center.x, region.center.y + half),
        WithAlpha(style_.colors.text, 0.62f),
        width);
    drawList->PopClipRect();
}

void OverlayRenderer::DrawRadar(ImDrawList* drawList,
                                const RadarVisual& radar,
                                const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(radar.center)) return;
    const float scale = style_.metrics.scale;
    const float maximumRadius =
        std::min(viewport.Width(), viewport.Height()) * 0.45f;
    if (maximumRadius <= 0.0f) return;
    const float requestedRadius =
        std::isfinite(radar.radius) && radar.radius > 0.0f
        ? radar.radius
        : style_.metrics.radarRadius * scale;
    const float radius = ClampFinite(
        requestedRadius,
        std::min(45.0f * scale, maximumRadius),
        maximumRadius);
    const ImVec2 center = ClampPoint(radar.center, viewport, radius + 4.0f * scale);
    const float insetRadius = std::max(1.0f, radius - 8.0f * scale);
    const float lineWidth = std::max(
        1.0f, style_.metrics.lineWidth * 0.52f * scale);
    drawList->AddCircleFilled(
        Add(center, ImVec2(1.5f * scale, 2.5f * scale)),
        radius + 1.5f * scale,
        WithAlpha(style_.colors.shadow, 0.46f),
        48);
    drawList->AddCircleFilled(
        center,
        radius,
        WithAlpha(style_.colors.surface, 0.72f),
        48);
    drawList->AddCircle(
        center,
        radius,
        WithAlpha(style_.colors.border, 0.92f),
        48,
        lineWidth);
    DrawTickedRing(
        drawList,
        center,
        radius - 3.5f * scale,
        WithAlpha(style_.colors.accent, 0.62f),
        lineWidth,
        7);
    drawList->AddCircle(
        center,
        insetRadius * 0.66f,
        WithAlpha(style_.colors.grid, 0.62f),
        36,
        lineWidth);
    drawList->AddCircle(
        center,
        insetRadius * 0.33f,
        WithAlpha(style_.colors.grid, 0.50f),
        28,
        lineWidth);
    drawList->AddLine(
        ImVec2(center.x - insetRadius, center.y),
        ImVec2(center.x + insetRadius, center.y),
        WithAlpha(style_.colors.grid, 0.48f),
        lineWidth);
    drawList->AddLine(
        ImVec2(center.x, center.y - insetRadius),
        ImVec2(center.x, center.y + insetRadius),
        WithAlpha(style_.colors.grid, 0.48f),
        lineWidth);

    for (const RadarBlip& blip : radar.blips) {
        if (!Finite(blip.normalizedPosition)) continue;
        ImVec2 normalized(
            std::clamp(blip.normalizedPosition.x, -1.0f, 1.0f),
            std::clamp(blip.normalizedPosition.y, -1.0f, 1.0f));
        const float normalizedLength = Length(normalized);
        if (normalizedLength > 1.0f) normalized = Multiply(normalized, 1.0f / normalizedLength);
        const ImVec2 position = Add(center, Multiply(normalized, insetRadius));
        const ImU32 color = ToneColor(blip.tone);
        const float size = 3.0f * scale;

        const bool headingUsable =
            blip.headingValid &&
            std::isfinite(blip.headingRadians) &&
            std::isfinite(radar.viewHeadingRadians);
        if ((blip.kind == RadarBlipKind::Self ||
             blip.kind == RadarBlipKind::Player) &&
            headingUsable) {
            const float heading = blip.headingRadians - radar.viewHeadingRadians - kPi * 0.5f;
            const ImVec2 forward(std::cos(heading), std::sin(heading));
            const ImVec2 side = Perpendicular(forward);
            const ImVec2 tip = Add(position, Multiply(forward, size * 2.4f));
            const ImVec2 rear = Subtract(position, Multiply(forward, size));
            const std::array<ImVec2, 3> points{{
                tip,
                Add(rear, Multiply(side, size)),
                Subtract(rear, Multiply(side, size)),
            }};
            if (blip.kind == RadarBlipKind::Player) {
                drawList->AddConvexPolyFilled(
                    points.data(), static_cast<int>(points.size()), color);
            } else {
                drawList->AddPolyline(
                    points.data(),
                    static_cast<int>(points.size()),
                    color,
                    ImDrawFlags_Closed,
                    lineWidth);
            }
        } else if (blip.kind == RadarBlipKind::Bot) {
            DrawDiamond(
                drawList, position, size * 1.15f, color, lineWidth, false);
            if (headingUsable) {
                const float heading =
                    blip.headingRadians - radar.viewHeadingRadians - kPi * 0.5f;
                const ImVec2 forward(std::cos(heading), std::sin(heading));
                drawList->AddLine(
                    position,
                    Add(position, Multiply(forward, size * 1.8f)),
                    color,
                    lineWidth);
            }
        } else {
            DrawDiamond(
                drawList,
                position,
                blip.kind == RadarBlipKind::Item
                    ? size * 0.72f
                    : size,
                color,
                lineWidth,
                true);
        }

        if (!blip.label.empty()) {
            DrawText(drawList,
                     Add(position, ImVec2(6.0f * scale, -6.0f * scale)),
                     style_.colors.textMuted,
                     style_.colors.shadow,
                     style_.metrics.smallFontSize * scale,
                     blip.label);
        }
    }

    if (radar.showSelf) {
        const float selfSize = 5.0f * scale;
        const std::array<ImVec2, 4> points{{
            ImVec2(center.x, center.y - selfSize * 1.45f),
            ImVec2(center.x + selfSize, center.y + selfSize),
            ImVec2(center.x, center.y + selfSize * 0.45f),
            ImVec2(center.x - selfSize, center.y + selfSize),
        }};
        drawList->AddConvexPolyFilled(
            points.data(),
            static_cast<int>(points.size()),
            style_.colors.accent);
        DrawDiamond(
            drawList,
            center,
            2.0f * scale,
            style_.colors.surface,
            lineWidth,
            true);
    }
}

void OverlayRenderer::DrawHudMap(ImDrawList* drawList,
                                 const HudMapVisual& map,
                                 const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid()) return;
    const float scale = style_.metrics.scale;
    const float size = ClampFinite(
        map.markerSize * scale,
        1.0f,
        40.0f * scale);
    const float labelSize = ClampFinite(
        map.fontSize * scale,
        0.0f,
        80.0f * scale);

    for (const HudMapMarker& marker : map.markers) {
        if (!Finite(marker.position) ||
            marker.position.x < viewport.left - size * 2.0f ||
            marker.position.x > viewport.right + size * 2.0f ||
            marker.position.y < viewport.top - size * 2.0f ||
            marker.position.y > viewport.bottom + size * 2.0f) {
            continue;
        }

        const ImU32 color = ToneColor(marker.tone);
        const float width = std::max(1.0f, style_.metrics.lineWidth * 0.5f * scale);
        if (marker.drawDirection && Finite(marker.directionEnd)) {
            DrawDashedLine(
                drawList,
                marker.position,
                marker.directionEnd,
                WithAlpha(color, 0.86f),
                WithAlpha(style_.colors.shadow, 0.62f),
                width,
                scale,
                4,
                0.62f);
            DrawDiamond(
                drawList,
                marker.directionEnd,
                std::max(1.5f, size * 0.30f),
                WithAlpha(color, 0.90f),
                width,
                true);
        }

        if (marker.kind == RadarBlipKind::Self) {
            const std::array<ImVec2, 4> points{{
                ImVec2(marker.position.x, marker.position.y - size * 1.25f),
                ImVec2(marker.position.x + size, marker.position.y + size),
                ImVec2(marker.position.x, marker.position.y + size * 0.45f),
                ImVec2(marker.position.x - size, marker.position.y + size),
            }};
            drawList->AddConvexPolyFilled(
                points.data(), static_cast<int>(points.size()), color);
        } else if (marker.kind == RadarBlipKind::Bot) {
            DrawDiamond(
                drawList, marker.position, size, color, width, false);
        } else {
            DrawDiamond(
                drawList,
                marker.position,
                marker.kind == RadarBlipKind::Item
                    ? size * 0.62f
                    : size * 0.82f,
                color,
                width,
                true);
            if (marker.kind == RadarBlipKind::Player) {
                DrawDiamond(
                    drawList,
                    marker.position,
                    size * 0.30f,
                    style_.colors.surface,
                    width,
                    true);
            }
        }

        if (!marker.label.empty() && labelSize > 0.0f) {
            DrawText(drawList,
                     Add(marker.position, ImVec2(size + 4.0f * scale,
                                                 -labelSize * 0.5f)),
                     style_.colors.text,
                     style_.colors.shadow,
                     labelSize,
                     marker.label);
        }
    }
}

void OverlayRenderer::DrawWorldLabel(ImDrawList* drawList,
                                     const WorldLabel& label,
                                     const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(label.anchor) || label.title.empty()) return;
    const float scale = style_.metrics.scale;
    const bool screenAlert = label.kind == WorldLabelKind::ScreenAlert;
    float titleSize =
        std::isfinite(label.titleSizeOverride) && label.titleSizeOverride > 0.0f
        ? std::clamp(label.titleSizeOverride * scale, 8.0f * scale, 48.0f * scale)
        : style_.metrics.fontSize * scale;
    float detailSize = style_.metrics.smallFontSize * scale;
    if (screenAlert) {
        titleSize = std::max(titleSize, style_.metrics.fontSize * 1.22f * scale);
        detailSize = std::max(detailSize, style_.metrics.smallFontSize * scale);
    }
    const ImU32 color = label.colorOverride != 0
        ? label.colorOverride
        : ToneColor(label.tone);
    const float margin = 6.0f * scale;
    const float paddingX = (screenAlert ? 13.0f : 8.0f) * scale;
    const float paddingY = (screenAlert ? 7.0f : 5.0f) * scale;
    const float markerColumn = screenAlert ? 4.0f * scale : 8.0f * scale;
    const float maximumContentWidth = std::max(
        1.0f,
        std::min((screenAlert ? 420.0f : 260.0f) * scale,
                 viewport.Width() - margin * 2.0f - paddingX * 2.0f -
                     markerColumn));
    const FittedText title =
        FitTextWithExtent(label.title, maximumContentWidth, titleSize);
    const FittedText detail = label.detail.empty()
        ? FittedText{}
        : FitTextWithExtent(label.detail, maximumContentWidth, detailSize);
    if (title.text.empty()) return;
    const float gap = detail.text.empty() ? 0.0f : 2.0f * scale;
    const float contentHeight = titleSize +
        (detail.text.empty() ? 0.0f : detailSize + gap);
    const float contentWidth = std::max(title.extent.x, detail.extent.x);
    const float panelWidth = std::min(
        viewport.Width() - margin * 2.0f,
        contentWidth + paddingX * 2.0f + markerColumn);
    const float panelHeight = contentHeight + paddingY * 2.0f;
    const ImVec2 anchor = ClampPoint(label.anchor, viewport, margin);
    const float panelLeft = ClampFinite(
        anchor.x - panelWidth * 0.5f,
        viewport.left + margin,
        viewport.right - margin - panelWidth);
    const float requestedTop = screenAlert
        ? anchor.y - panelHeight * 0.5f
        : anchor.y - panelHeight - 8.0f * scale;
    const float panelTop = ClampFinite(
        requestedTop,
        viewport.top + margin,
        viewport.bottom - margin - panelHeight);
    const ImVec2 panelMinimum(panelLeft, panelTop);
    const ImVec2 panelMaximum(
        panelLeft + panelWidth, panelTop + panelHeight);
    const ImU32 textShadow = WithAlpha(style_.colors.shadow, 0.62f);
    if (!screenAlert) {
        const ImVec2 stemEnd(
            ClampFinite(
                anchor.x,
                panelMinimum.x + 10.0f * scale,
                panelMaximum.x - 10.0f * scale),
            panelMaximum.y);
        drawList->AddLine(
            anchor,
            stemEnd,
            WithAlpha(color, 0.48f),
            std::max(1.0f, style_.metrics.lineWidth * 0.45f * scale));
    }
    DrawPanel(
        drawList,
        panelMinimum,
        panelMaximum,
        WithAlpha(
            screenAlert ? style_.colors.surfaceRaised : style_.colors.surface,
            screenAlert ? 0.90f : 0.78f),
        WithAlpha(color, label.emphasized ? 0.92f : 0.62f),
        style_.colors.shadow,
        style_.metrics.panelRounding * scale,
        scale);
    if (screenAlert) {
        drawList->AddRectFilled(
            ImVec2(panelMinimum.x, panelMinimum.y),
            ImVec2(panelMinimum.x + markerColumn, panelMaximum.y),
            color,
            style_.metrics.panelRounding * scale);
        drawList->AddRectFilled(
            ImVec2(panelMaximum.x - markerColumn, panelMinimum.y),
            ImVec2(panelMaximum.x, panelMaximum.y),
            color,
            style_.metrics.panelRounding * scale);
    } else {
        DrawDiamond(
            drawList,
            ImVec2(
                panelMinimum.x + markerColumn * 0.5f + 2.0f * scale,
                panelMinimum.y + panelHeight * 0.5f),
            label.emphasized ? 3.6f * scale : 2.8f * scale,
            color,
            1.0f,
            label.kind == WorldLabelKind::Item || label.emphasized);
    }

    const float textStartX = panelMinimum.x + markerColumn + paddingX;
    const float titleX = screenAlert
        ? panelMinimum.x + (panelWidth - title.extent.x) * 0.5f
        : textStartX;
    float textY = panelMinimum.y + paddingY;
    DrawText(drawList,
             ImVec2(titleX, textY),
             color,
             textShadow,
             titleSize,
             title.text);
    if (!detail.text.empty()) {
        textY += titleSize + gap;
        const float detailX = screenAlert
            ? panelMinimum.x + (panelWidth - detail.extent.x) * 0.5f
            : textStartX;
        DrawText(drawList,
                 ImVec2(detailX, textY),
                 screenAlert ? style_.colors.text : style_.colors.textMuted,
                 textShadow,
                 detailSize,
                 detail.text);
    }
}

void OverlayRenderer::DrawHighValueList(ImDrawList* drawList,
                                        const HighValueList& list,
                                        const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || list.entries.empty() || list.maxRows <= 0) return;
    const float scale = style_.metrics.scale;
    const float titleSize = style_.metrics.fontSize * scale;
    const float rowFontSize = style_.metrics.smallFontSize * scale;
    const float padding = 9.0f * scale;
    const float headerHeight =
        list.title.empty() ? 0.0f : titleSize + 8.0f * scale;
    const float rowHeight = rowFontSize + 7.0f * scale;
    const float availableWidth = viewport.Width() - 16.0f * scale;
    const float availableHeight = viewport.Height() - 16.0f * scale;
    if (availableWidth < 80.0f * scale ||
        availableHeight < rowHeight + padding * 2.0f) {
        return;
    }
    const float width = std::min(
        std::max(list.width * scale, 210.0f * scale),
        availableWidth);
    const int rowsByHeight = std::max(
        0,
        static_cast<int>(
            (availableHeight - headerHeight - padding * 2.0f) /
            rowHeight));
    const int rowCount = std::min(
        {list.maxRows, static_cast<int>(list.entries.size()), rowsByHeight});
    if (rowCount <= 0) return;
    const float height =
        padding * 2.0f + headerHeight + rowHeight * rowCount;
    ImVec2 origin = ClampPoint(list.origin, viewport, 8.0f * scale);
    origin.x = ClampFinite(
        origin.x,
        viewport.left + 8.0f * scale,
        viewport.right - width - 8.0f * scale);
    origin.y = ClampFinite(
        origin.y,
        viewport.top + 8.0f * scale,
        viewport.bottom - height - 8.0f * scale);
    const ImVec2 panelMaximum(origin.x + width, origin.y + height);
    const float contentLeft = origin.x + padding;
    const float contentRight = panelMaximum.x - padding;
    const ImU32 textShadow = WithAlpha(style_.colors.shadow, 0.62f);
    DrawPanel(
        drawList,
        origin,
        panelMaximum,
        WithAlpha(style_.colors.surfaceRaised, 0.88f),
        WithAlpha(style_.colors.border, 0.96f),
        style_.colors.shadow,
        style_.metrics.panelRounding * scale,
        scale);
    drawList->AddRectFilled(
        origin,
        ImVec2(panelMaximum.x, origin.y + 2.5f * scale),
        style_.colors.accent,
        style_.metrics.panelRounding * scale);

    float rowY = origin.y + padding;
    if (!list.title.empty()) {
        DrawDiamond(
            drawList,
            ImVec2(contentLeft + 3.0f * scale,
                   rowY + titleSize * 0.5f),
            3.0f * scale,
            style_.colors.accent,
            1.0f,
            true);
        DrawText(drawList,
                 ImVec2(contentLeft + 11.0f * scale, rowY),
                 style_.colors.accent,
                 textShadow,
                 titleSize,
                 FitText(
                     list.title,
                     std::max(
                         0.0f,
                         contentRight - contentLeft - 11.0f * scale),
                     titleSize));
        rowY += headerHeight;
    }

    for (int index = 0; index < rowCount; ++index) {
        const HighValueEntry& entry = list.entries[static_cast<std::size_t>(index)];
        const ImU32 color = ToneColor(entry.tone);
        const std::string value = FormatValue(entry.value);
        const std::string distance = FormatDistance(entry.distanceMeters);
        const ImVec2 valueExtent = TextExtent(value, rowFontSize);
        const ImVec2 distanceExtent = TextExtent(distance, rowFontSize);
        const float valueX = contentRight - valueExtent.x;
        const float distanceX = valueX - 10.0f * scale - distanceExtent.x;
        const float nameX = contentLeft + 10.0f * scale;
        const float nameWidth = std::max(0.0f, distanceX - nameX - 8.0f * scale);
        if (index % 2 == 0) {
            drawList->AddRectFilled(
                ImVec2(contentLeft - 3.0f * scale, rowY),
                ImVec2(contentRight + 3.0f * scale, rowY + rowHeight),
                WithAlpha(style_.colors.surfaceSoft, 0.48f),
                3.0f * scale);
        }
        DrawDiamond(
            drawList,
            ImVec2(
                contentLeft + 2.5f * scale,
                rowY + rowHeight * 0.5f),
            2.4f * scale,
            color,
            1.0f,
            true);
        const float textY =
            rowY + (rowHeight - rowFontSize) * 0.5f;
        DrawText(drawList,
                 ImVec2(nameX, textY),
                 style_.colors.text,
                 textShadow,
                 rowFontSize,
                 FitText(entry.name, nameWidth, rowFontSize));
        if (!distance.empty()) {
            DrawText(drawList,
                     ImVec2(distanceX, textY),
                     style_.colors.textMuted,
                     textShadow,
                     rowFontSize,
                     distance);
        }
        DrawText(drawList,
                 ImVec2(valueX, textY),
                 color,
                 textShadow,
                 rowFontSize,
                 value);
        rowY += rowHeight;
    }
}

}  // namespace lengjing
