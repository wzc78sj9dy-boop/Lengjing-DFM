#include "render/overlay_renderer.h"
#include "render/OverlayContrastPolicy.h"

#include <algorithm>
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
            return style.colors.text;
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
    const float outlineOffset = std::clamp(fontSize * 0.045f, 0.75f, 1.5f);
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

std::string FitText(const std::string& text, float maximumWidth, float fontSize) {
    if (text.empty() || maximumWidth <= 0.0f) return {};
    if (TextExtent(text, fontSize).x <= maximumWidth) {
        return text;
    }

    constexpr char suffix[] = "...";
    const float suffixWidth = TextExtent(suffix, fontSize).x;
    if (suffixWidth >= maximumWidth) return {};

    std::size_t accepted = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t sequenceLength = 1;
        if ((lead & 0xe0U) == 0xc0U) sequenceLength = 2;
        else if ((lead & 0xf0U) == 0xe0U) sequenceLength = 3;
        else if ((lead & 0xf8U) == 0xf0U) sequenceLength = 4;
        const std::size_t next = std::min(text.size(), offset + sequenceLength);
        const std::string candidate = text.substr(0, next) + suffix;
        if (TextExtent(candidate, fontSize).x > maximumWidth) break;
        accepted = next;
        offset = next;
    }
    return text.substr(0, accepted) + suffix;
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
                   ImVec2(player.bounds.Center().x, player.bounds.bottom),
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
    const float margin = 4.0f * scale;
    const float textGap = hasTitle && hasDetail ? 1.0f * scale : 0.0f;
    const float maximumTextWidth = std::max(
        1.0f,
        std::min(300.0f * scale, viewport.Width() - margin * 2.0f));
    const std::string title = hasTitle
        ? FitText(player.name, maximumTextWidth, titleSize)
        : std::string{};
    const std::string detail = hasDetail
        ? FitText(player.detail, maximumTextWidth, detailSize)
        : std::string{};
    const ImVec2 titleExtent = TextExtent(title, titleSize);
    const ImVec2 detailExtent = TextExtent(detail, detailSize);
    const float textHeight =
        (title.empty() ? 0.0f : titleSize) +
        (detail.empty() ? 0.0f : detailSize) + textGap;
    const float textTop = std::clamp(
        player.bounds.top - textHeight - 5.0f * scale,
        viewport.top + margin,
        std::max(viewport.top + margin,
                 viewport.bottom - margin - textHeight));
    const float centerX = player.bounds.Center().x;
    const ImU32 textShadow = WithAlpha(style_.colors.shadow, 0.62f);
    const ImU32 titleColor = player.isBot
        ? style_.colors.text
        : (player.visible ? style_.colors.text : style_.colors.textMuted);
    const ImU32 detailColor = player.isBot
        ? style_.colors.text
        : style_.colors.textMuted;

    float cursorY = textTop;
    if (!title.empty()) {
        const float titleX = std::clamp(
            centerX - titleExtent.x * 0.5f,
            viewport.left + margin,
            viewport.right - margin - titleExtent.x);
        DrawText(drawList,
                 ImVec2(titleX, cursorY),
                 titleColor,
                 textShadow,
                 titleSize,
                 title);
        cursorY += titleSize + textGap;
    }
    if (!detail.empty()) {
        const float detailX = std::clamp(
            centerX - detailExtent.x * 0.5f,
            viewport.left + margin,
            viewport.right - margin - detailExtent.x);
        DrawText(drawList,
                 ImVec2(detailX, cursorY),
                 detailColor,
                 textShadow,
                 detailSize,
                 detail);
    }

}

void OverlayRenderer::DrawCornerBox(ImDrawList* drawList,
                                    const ScreenRect& bounds,
                                    SemanticTone tone,
                                    bool visible) const {
    if (drawList == nullptr || !bounds.IsValid()) return;
    const float scale = style_.metrics.scale;
    const float horizontalLength = bounds.Width() * 0.25f;
    const float verticalLength = bounds.Height() * 0.25f;
    const float width = render::PlayerStrokeWidth(
        style_.metrics.lineWidth, scale);
    const float outline = render::PlayerOutlineWidth(
        width, style_.metrics.outlineWidth, scale);
    const ImU32 color = render::WithExactAlpha(
        PlayerColor(style_, tone, visible), render::kSolidAlpha);
    const ImU32 shadow = render::WithExactAlpha(style_.colors.shadow, 140);

    const ImVec2 topLeft(bounds.left, bounds.top);
    const ImVec2 topRight(bounds.right, bounds.top);
    const ImVec2 bottomLeft(bounds.left, bounds.bottom);
    const ImVec2 bottomRight(bounds.right, bounds.bottom);

    const auto drawSegment = [&](const ImVec2& first, const ImVec2& second) {
        DrawOutlinedLine(
            drawList, first, second, color, shadow, width, outline);
    };

    drawSegment(topLeft, ImVec2(topLeft.x + horizontalLength, topLeft.y));
    drawSegment(topLeft, ImVec2(topLeft.x, topLeft.y + verticalLength));
    drawSegment(topRight, ImVec2(topRight.x - horizontalLength, topRight.y));
    drawSegment(topRight, ImVec2(topRight.x, topRight.y + verticalLength));
    drawSegment(bottomLeft, ImVec2(bottomLeft.x + horizontalLength, bottomLeft.y));
    drawSegment(bottomLeft, ImVec2(bottomLeft.x, bottomLeft.y - verticalLength));
    drawSegment(bottomRight, ImVec2(bottomRight.x - horizontalLength, bottomRight.y));
    drawSegment(bottomRight, ImVec2(bottomRight.x, bottomRight.y - verticalLength));
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
    const float outline = render::PlayerOutlineWidth(
        width, style_.metrics.outlineWidth, scale);
    const ImU32 shadow = render::WithExactAlpha(style_.colors.shadow, 140);
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
        const ImU32 firstColor = endpointColor(first);
        const ImU32 secondColor = endpointColor(second);
        if (firstColor == secondColor) {
            DrawOutlinedLine(
                drawList,
                first.position,
                second.position,
                firstColor,
                shadow,
                width,
                outline);
        } else {
            const ImVec2 midpoint{
                (first.position.x + second.position.x) * 0.5f,
                (first.position.y + second.position.y) * 0.5f};
            DrawOutlinedLine(
                drawList,
                first.position,
                midpoint,
                firstColor,
                shadow,
                width,
                outline);
            DrawOutlinedLine(
                drawList,
                midpoint,
                second.position,
                secondColor,
                shadow,
                width,
                outline);
        }
    }
    if (skeleton.selectedJoint >= 0 &&
        static_cast<std::size_t>(skeleton.selectedJoint) <
            skeleton.joints.size()) {
        const BoneJoint& selected =
            skeleton.joints[static_cast<std::size_t>(skeleton.selectedJoint)];
        if (selected.valid && Finite(selected.position)) {
            drawList->AddCircle(
                selected.position,
                3.5f * scale,
                shadow,
                0,
                outline);
            drawList->AddCircle(
                selected.position,
                3.5f * scale,
                render::WithExactAlpha(
                    style_.colors.caution, render::kSolidAlpha),
                0,
                width);
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
    const float height = std::max(bounds.Height(), 8.0f * scale);
    const float healthWidth = std::max(2.0f, 3.0f * scale);
    const float armorWidth = std::max(2.0f, 3.0f * scale);
    const float gap = std::max(1.0f, 1.0f * scale);
    const float right = bounds.left - 3.0f * scale;
    const float top = bounds.bottom - height;
    const ImVec2 healthMin(right - healthWidth, top);
    const ImVec2 healthMax(right, bounds.bottom);
    drawList->AddRectFilled(
        healthMin,
        healthMax,
        IM_COL32(0, 0, 0, 128));
    if (healthRatio > 0.0f) {
        drawList->AddRectFilled(
            ImVec2(healthMin.x, bounds.bottom - height * healthRatio),
            healthMax,
            render::WithExactAlpha(healthColor, render::kSolidAlpha));
    }
    if (!hasArmorTrack) return;

    const ImVec2 armorMax(healthMin.x - gap, bounds.bottom);
    const ImVec2 armorMin(armorMax.x - armorWidth, top);
    drawList->AddRectFilled(
        armorMin,
        armorMax,
        IM_COL32(0, 0, 0, 128));
    if (armorRatio > 0.0f) {
        drawList->AddRectFilled(
            ImVec2(armorMin.x, bounds.bottom - height * armorRatio),
            armorMax,
            render::WithExactAlpha(
                style_.colors.ally, render::kSolidAlpha));
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
    drawList->AddLine(
        origin,
        target,
        render::WithExactAlpha(color, render::kSolidAlpha),
        width);
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
    drawList->AddLine(
        start,
        end,
        render::WithExactAlpha(base, render::kSolidAlpha),
        width);
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
        1.0f, style_.metrics.lineWidth * 0.5f * scale);
    const ImU32 color = WithAlpha(ToneColor(model.tone), 0.66f);
    for (const GeometrySegmentVisual& segment : model.segments) {
        if (!Finite(segment.start) || !Finite(segment.end)) continue;
        drawList->AddLine(segment.start, segment.end, color, width);
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
    const ImU32 color = WithAlpha(style_.colors.text, 0.92f);
    const float arrowLength = 10.0f * shapeScale;
    const float arrowWidth = 6.0f * shapeScale;
    const ImVec2 base = Subtract(anchor, Multiply(direction, arrowLength));
    const ImVec2 left = Add(base, Multiply(side, arrowWidth));
    const ImVec2 right = Subtract(base, Multiply(side, arrowWidth));
    drawList->AddTriangleFilled(anchor, left, right, color);

    std::string caption = marker.label;
    if (!caption.empty()) {
        const float fontSize = style_.metrics.smallFontSize * scale;
        const ImVec2 extent = TextExtent(caption, fontSize);
        const float availableWidth =
            std::max(0.0f, viewport.Width() - 8.0f * scale);
        const float textWidth = std::min(extent.x, availableWidth);
        if (textWidth <= 0.0f) return;
        const ImVec2 inward = Subtract(
            anchor, Multiply(direction, 22.0f * shapeScale));
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
                 style_.colors.shadow,
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
    if (std::isfinite(projectile.rangeRadius) && projectile.rangeRadius > 1.0f) {
        const float radius = std::min(projectile.rangeRadius,
                                      std::max(viewport.Width(), viewport.Height()));
        drawList->AddCircle(projectile.center,
                            radius,
                            WithAlpha(color, 0.38f),
                            0,
                            std::max(1.0f, style_.metrics.lineWidth * 0.42f * scale));
    }

    if (projectile.trajectory.size() >= 2) {
        for (std::size_t index = 1; index < projectile.trajectory.size(); ++index) {
            const ImVec2& first = projectile.trajectory[index - 1];
            const ImVec2& second = projectile.trajectory[index];
            if (!Finite(first) || !Finite(second)) continue;
            drawList->AddLine(first,
                              second,
                              WithAlpha(color, 0.70f),
                              std::max(1.0f, style_.metrics.lineWidth * 0.42f * scale));
        }
    }

    std::string caption = projectile.label;
    const std::string distance = FormatDistance(projectile.distanceMeters);
    if (!distance.empty()) {
        if (!caption.empty()) caption += "  ";
        caption += distance;
    }
    if (!caption.empty()) {
        const float fontSize = style_.metrics.fontSize * scale;
        const float margin = 4.0f * scale;
        const float maximumWidth = std::max(
            1.0f,
            std::min(300.0f * scale, viewport.Width() - margin * 2.0f));
        const std::string fitted = FitText(caption, maximumWidth, fontSize);
        const ImVec2 extent = TextExtent(fitted, fontSize);
        const float x = std::clamp(
            projectile.center.x - extent.x * 0.5f,
            viewport.left + margin,
            viewport.right - margin - extent.x);
        const float y = std::clamp(
            projectile.center.y - fontSize - 9.0f * scale,
            viewport.top + margin,
            viewport.bottom - margin - fontSize);
        DrawText(drawList,
                 ImVec2(x, y),
                 color,
                 WithAlpha(style_.colors.shadow, 0.62f),
                 fontSize,
                 fitted);
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
    const ImU32 color = IM_COL32(255, 255, 255, 255);
    const ImVec2 center = crosshair.center;
    drawList->AddLine(ImVec2(center.x - half, center.y),
                      ImVec2(center.x + half, center.y),
                      color,
                      width);
    drawList->AddLine(ImVec2(center.x, center.y - half),
                      ImVec2(center.x, center.y + half),
                      color,
                      width);
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
        drawList->AddCircle(guide.center,
                            radius,
                            IM_COL32(255, 0, 0, 220),
                            0,
                            std::max(1.0f, 1.25f * scale));
    }
    if (guide.drawTargetRay && guide.targetValid && Finite(guide.target)) {
        const ImVec2 target = ClampPoint(guide.target, viewport, 5.0f * scale);
        const ImU32 color = guide.locked
            ? IM_COL32(255, 0, 0, 235)
            : IM_COL32(255, 165, 0, 235);
        drawList->AddLine(guide.center,
                          target,
                          color,
                          std::max(1.0f, 1.5f * scale));
        drawList->AddCircleFilled(target, 3.0f * scale, color, 10);
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

    const ImU32 color = WithAlpha(style_.colors.ally, 0.38f);
    const float width = std::max(1.0f, style_.metrics.lineWidth * 0.5f * scale);
    drawList->AddRect(ImVec2(bounds.left, bounds.top),
                      ImVec2(bounds.right, bounds.bottom),
                      color,
                      0.0f,
                      0,
                      width);
    const float half = 5.0f * scale;
    drawList->AddLine(ImVec2(region.center.x - half, region.center.y),
                      ImVec2(region.center.x + half, region.center.y),
                      WithAlpha(style_.colors.text, 0.58f),
                      width);
    drawList->AddLine(ImVec2(region.center.x, region.center.y - half),
                      ImVec2(region.center.x, region.center.y + half),
                      WithAlpha(style_.colors.text, 0.58f),
                      width);
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
    const ImVec2 minimum(center.x - radius, center.y - radius);
    const ImVec2 maximum(center.x + radius, center.y + radius);
    const float insetRadius = std::max(1.0f, radius - 4.0f * scale);
    const float lineWidth = std::max(1.0f, style_.metrics.lineWidth * 0.5f * scale);
    drawList->AddRect(minimum,
                      maximum,
                      WithAlpha(style_.colors.textMuted, 0.46f),
                      0.0f,
                      0,
                      lineWidth);
    drawList->AddLine(ImVec2(center.x, minimum.y),
                      ImVec2(center.x, maximum.y),
                      WithAlpha(style_.colors.textMuted, 0.20f),
                      lineWidth);
    drawList->AddLine(ImVec2(minimum.x, center.y),
                      ImVec2(maximum.x, center.y),
                      WithAlpha(style_.colors.textMuted, 0.20f),
                      lineWidth);
    drawList->AddCircle(center,
                        insetRadius * 0.5f,
                        WithAlpha(style_.colors.textMuted, 0.16f),
                        48,
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
            drawList->AddLine(position,
                              Add(position, Multiply(forward, size * 2.0f)),
                              color,
                              lineWidth);
            drawList->AddCircleFilled(position, size * 0.65f, color, 10);
        } else if (blip.kind == RadarBlipKind::Bot) {
            drawList->AddCircle(position, size, color, 12, lineWidth);
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
            drawList->AddCircleFilled(position,
                                      blip.kind == RadarBlipKind::Item
                                          ? size * 0.55f
                                          : size * 0.75f,
                                      color,
                                      10);
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
        const float selfSize = 4.0f * scale;
        const ImVec2 points[3] = {
            ImVec2(center.x, center.y - selfSize),
            ImVec2(center.x + selfSize * 0.8f, center.y + selfSize),
            ImVec2(center.x - selfSize * 0.8f, center.y + selfSize),
        };
        drawList->AddPolyline(points,
                              3,
                              style_.colors.accent,
                              ImDrawFlags_Closed,
                              lineWidth);
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
            drawList->AddLine(marker.position,
                              marker.directionEnd,
                              WithAlpha(color, 0.82f),
                              width);
        }

        if (marker.kind == RadarBlipKind::Self) {
            const ImVec2 points[3] = {
                ImVec2(marker.position.x, marker.position.y - size * 1.25f),
                ImVec2(marker.position.x + size, marker.position.y + size * 0.85f),
                ImVec2(marker.position.x - size, marker.position.y + size * 0.85f),
            };
            drawList->AddPolyline(points, 3, color, ImDrawFlags_Closed, width);
        } else if (marker.kind == RadarBlipKind::Bot) {
            drawList->AddCircle(marker.position, size, color, 12, width);
        } else {
            drawList->AddCircleFilled(marker.position, size * 0.58f, color, 10);
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
    const float maximumWidth = std::max(
        1.0f,
        std::min((screenAlert ? 420.0f : 260.0f) * scale,
                 viewport.Width() - margin * 2.0f));
    const std::string title = FitText(label.title, maximumWidth, titleSize);
    const std::string detail = label.detail.empty()
        ? std::string{}
        : FitText(label.detail, maximumWidth, detailSize);
    const ImVec2 titleExtent = TextExtent(title, titleSize);
    const ImVec2 detailExtent = TextExtent(detail, detailSize);
    const float gap = detail.empty() ? 0.0f : 1.0f * scale;
    const float textHeight = titleSize +
        (detail.empty() ? 0.0f : detailSize + gap);
    const ImVec2 anchor = ClampPoint(label.anchor, viewport, margin);
    const float top = ClampFinite(
        anchor.y - textHeight * 0.5f,
        viewport.top + margin,
        viewport.bottom - margin - textHeight);
    const ImU32 textShadow = WithAlpha(style_.colors.shadow, 0.62f);
    const float titleX = ClampFinite(
        anchor.x - titleExtent.x * 0.5f,
        viewport.left + margin,
        viewport.right - margin - titleExtent.x);
    DrawText(drawList,
             ImVec2(titleX, top),
             color,
             textShadow,
             titleSize,
             title);
    if (!detail.empty()) {
        const float detailX = ClampFinite(
            anchor.x - detailExtent.x * 0.5f,
            viewport.left + margin,
            viewport.right - margin - detailExtent.x);
        DrawText(drawList,
                 ImVec2(detailX, top + titleSize + gap),
                 screenAlert ? style_.colors.text : style_.colors.textMuted,
                 textShadow,
                 detailSize,
                 detail);
    }
}

void OverlayRenderer::DrawHighValueList(ImDrawList* drawList,
                                        const HighValueList& list,
                                        const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || list.entries.empty() || list.maxRows <= 0) return;
    const float scale = style_.metrics.scale;
    const float titleSize = style_.metrics.fontSize * scale;
    const float rowFontSize = style_.metrics.smallFontSize * scale;
    const float headerHeight = list.title.empty() ? 0.0f : titleSize + 5.0f * scale;
    const float rowHeight = rowFontSize + 5.0f * scale;
    const float availableWidth = viewport.Width() - 16.0f * scale;
    const float availableHeight = viewport.Height() - 16.0f * scale;
    if (availableWidth < 80.0f * scale || availableHeight < rowHeight) return;
    const float width = std::min(
        std::max(list.width * scale, 210.0f * scale),
        availableWidth);
    const int rowsByHeight = std::max(
        0,
        static_cast<int>((availableHeight - headerHeight) / rowHeight));
    const int rowCount = std::min(
        {list.maxRows, static_cast<int>(list.entries.size()), rowsByHeight});
    if (rowCount <= 0) return;
    const float height = headerHeight + rowHeight * rowCount;
    ImVec2 origin = ClampPoint(list.origin, viewport, 8.0f * scale);
    origin.x = std::clamp(origin.x, viewport.left + 8.0f * scale,
                          viewport.right - width - 8.0f * scale);
    origin.y = std::clamp(origin.y, viewport.top + 8.0f * scale,
                          viewport.bottom - height - 8.0f * scale);
    const float right = origin.x + width;
    const ImU32 textShadow = WithAlpha(style_.colors.shadow, 0.62f);

    float rowY = origin.y;
    if (!list.title.empty()) {
        DrawText(drawList,
                 origin,
                 style_.colors.accent,
                 textShadow,
                 titleSize,
                 FitText(list.title, width, titleSize));
        rowY += headerHeight;
    }

    for (int index = 0; index < rowCount; ++index) {
        const HighValueEntry& entry = list.entries[static_cast<std::size_t>(index)];
        const ImU32 color = ToneColor(entry.tone);
        const std::string value = FormatValue(entry.value);
        const std::string distance = FormatDistance(entry.distanceMeters);
        const ImVec2 valueExtent = TextExtent(value, rowFontSize);
        const ImVec2 distanceExtent = TextExtent(distance, rowFontSize);
        const float valueX = right - valueExtent.x;
        const float distanceX = valueX - 10.0f * scale - distanceExtent.x;
        const float nameX = origin.x;
        const float nameWidth = std::max(0.0f, distanceX - nameX - 8.0f * scale);
        DrawText(drawList,
                 ImVec2(nameX, rowY),
                 style_.colors.text,
                 textShadow,
                 rowFontSize,
                 FitText(entry.name, nameWidth, rowFontSize));
        if (!distance.empty()) {
            DrawText(drawList,
                     ImVec2(distanceX, rowY),
                     style_.colors.textMuted,
                     textShadow,
                     rowFontSize,
                     distance);
        }
        DrawText(drawList,
                 ImVec2(valueX, rowY),
                 color,
                 textShadow,
                 rowFontSize,
                 value);
        rowY += rowHeight;
    }
}

}  // namespace lengjing
