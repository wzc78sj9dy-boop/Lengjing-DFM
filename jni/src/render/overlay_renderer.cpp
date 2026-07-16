#include "render/overlay_renderer.h"

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

ImVec2 ClampPoint(const ImVec2& point, const ScreenRect& viewport, float margin) {
    return ImVec2(
        std::clamp(point.x, viewport.left + margin, viewport.right - margin),
        std::clamp(point.y, viewport.top + margin, viewport.bottom - margin));
}

void DrawText(ImDrawList* drawList,
              const ImVec2& position,
              ImU32 color,
              ImU32 shadow,
              float fontSize,
              const std::string& text) {
    if (drawList == nullptr || text.empty() || !Finite(position)) return;
    drawList->AddText(nullptr, fontSize, Add(position, ImVec2(1.0f, 1.0f)), shadow, text.c_str());
    drawList->AddText(nullptr, fontSize, position, color, text.c_str());
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
    drawList->AddLine(first, second, shadow, std::max(outlineWidth, width));
    drawList->AddLine(first, second, color, width);
}

void DrawChamferedPanel(ImDrawList* drawList,
                        const ImVec2& minimum,
                        const ImVec2& maximum,
                        ImU32 fill,
                        ImU32 border,
                        float cut,
                        float borderWidth) {
    if (drawList == nullptr || !Finite(minimum) || !Finite(maximum) ||
        maximum.x <= minimum.x || maximum.y <= minimum.y) {
        return;
    }
    const float safeCut = std::clamp(
        cut,
        0.0f,
        std::min(maximum.x - minimum.x, maximum.y - minimum.y) * 0.35f);
    const ImVec2 points[6] = {
        minimum,
        ImVec2(maximum.x - safeCut, minimum.y),
        ImVec2(maximum.x, minimum.y + safeCut),
        maximum,
        ImVec2(minimum.x + safeCut, maximum.y),
        ImVec2(minimum.x, maximum.y - safeCut),
    };
    drawList->AddConvexPolyFilled(points, 6, fill);
    drawList->AddPolyline(points, 6, border, ImDrawFlags_Closed, borderWidth);
}

void DrawSegmentedTrack(ImDrawList* drawList,
                        const ImVec2& minimum,
                        const ImVec2& maximum,
                        float ratio,
                        ImU32 track,
                        ImU32 fill,
                        ImU32 separator,
                        float rounding,
                        int segments) {
    if (drawList == nullptr || !Finite(minimum) || !Finite(maximum) ||
        maximum.x <= minimum.x || maximum.y <= minimum.y) {
        return;
    }
    drawList->AddRectFilled(minimum, maximum, track, rounding);
    const float clamped = Clamp01(ratio);
    if (clamped > 0.0f) {
        drawList->AddRectFilled(
            minimum,
            ImVec2(minimum.x + (maximum.x - minimum.x) * clamped, maximum.y),
            fill,
            rounding);
    }
    if (segments > 1) {
        for (int index = 1; index < segments; ++index) {
            const float x = minimum.x +
                (maximum.x - minimum.x) * static_cast<float>(index) /
                    static_cast<float>(segments);
            drawList->AddLine(ImVec2(x, minimum.y + 1.0f),
                              ImVec2(x, maximum.y - 1.0f),
                              separator,
                              1.0f);
        }
    }
}

ImVec2 RotateVector(const ImVec2& value, float radians) {
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    return ImVec2(value.x * cosine - value.y * sine,
                  value.x * sine + value.y * cosine);
}

void DrawBrokenRing(ImDrawList* drawList,
                    const ImVec2& center,
                    float radius,
                    ImU32 color,
                    float width,
                    int segments) {
    if (drawList == nullptr || !Finite(center) || !std::isfinite(radius) || radius <= 0.0f) {
        return;
    }
    const float gap = 0.13f;
    for (int quadrant = 0; quadrant < 4; ++quadrant) {
        const float start = static_cast<float>(quadrant) * kPi * 0.5f + gap;
        const float end = static_cast<float>(quadrant + 1) * kPi * 0.5f - gap;
        drawList->PathArcTo(center, radius, start, end, std::max(4, segments / 4));
        drawList->PathStroke(color, ImDrawFlags_None, width);
    }
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
        DrawCornerBox(drawList, player.bounds, player.tone, player.visible);
    }
    if (player.drawSkeleton) {
        DrawSkeleton(drawList, player.skeleton, player.tone, player.visible);
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
    if (player.name.empty() && player.detail.empty()) return;

    const float scale = style_.metrics.scale;
    const float titleSize = style_.metrics.fontSize * scale;
    const float detailSize = style_.metrics.smallFontSize * scale;
    const float paddingX = 9.0f * scale;
    const float paddingY = 5.0f * scale;
    const float gap = player.detail.empty() ? 0.0f : 1.0f * scale;
    const ImVec2 titleExtent = TextExtent(player.name, titleSize);
    const ImVec2 detailExtent = TextExtent(player.detail, detailSize);
    const float contentWidth = std::max(titleExtent.x, detailExtent.x);
    const float maximumWidth = std::max(
        1.0f,
        std::min(220.0f * scale, viewport.Width() - 8.0f * scale));
    const float width = std::min(
        std::max(contentWidth + paddingX * 2.0f, 76.0f * scale),
        maximumWidth);
    const float height = paddingY * 2.0f + titleSize +
                         (player.detail.empty() ? 0.0f : detailSize + gap);
    const float desiredX = player.bounds.Center().x - width * 0.5f;
    const float x = std::clamp(desiredX,
                               viewport.left + 4.0f * scale,
                               viewport.right - width - 4.0f * scale);
    const float y = std::max(viewport.top + 4.0f * scale,
                             player.bounds.top - height - 8.0f * scale);
    const ImVec2 minimum(x, y);
    const ImVec2 maximum(x + width, y + height);
    const ImU32 tone = player.visible
        ? ToneColor(player.tone)
        : style_.colors.textMuted;

    const float cut = 6.0f * scale;
    DrawChamferedPanel(drawList,
                       Add(minimum, ImVec2(1.0f * scale, 2.0f * scale)),
                       Add(maximum, ImVec2(1.0f * scale, 2.0f * scale)),
                       WithAlpha(style_.colors.shadow, 0.58f),
                       WithAlpha(style_.colors.shadow, 0.58f),
                       cut,
                       1.0f * scale);
    DrawChamferedPanel(drawList,
                       minimum,
                       maximum,
                       style_.colors.surfaceRaised,
                       style_.colors.border,
                       cut,
                       1.0f * scale);
    drawList->AddLine(
        ImVec2(minimum.x + 1.0f * scale, minimum.y),
        ImVec2(minimum.x + std::min(width * 0.58f, 68.0f * scale), minimum.y),
        tone,
        2.0f * scale);
    drawList->AddLine(
        ImVec2(maximum.x - 19.0f * scale, maximum.y),
        ImVec2(maximum.x - cut, maximum.y),
        WithAlpha(style_.colors.ally, 0.88f),
        1.5f * scale);
    drawList->AddRectFilled(
        ImVec2(minimum.x + 4.0f * scale, minimum.y + 6.0f * scale),
        ImVec2(minimum.x + 6.0f * scale, maximum.y - 6.0f * scale),
        WithAlpha(tone, 0.86f));

    const float textWidth = width - paddingX * 2.0f;
    const std::string title = FitText(player.name, textWidth, titleSize);
    DrawText(drawList,
             ImVec2(x + paddingX, y + paddingY),
             player.visible ? style_.colors.text : style_.colors.textMuted,
             style_.colors.shadow,
             titleSize,
             title);
    if (!player.detail.empty()) {
        const std::string detail = FitText(player.detail, textWidth, detailSize);
        DrawText(drawList,
                 ImVec2(x + paddingX, y + paddingY + titleSize + gap),
                 style_.colors.textMuted,
                 style_.colors.shadow,
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
    const float horizontalRail = std::clamp(
        bounds.Width() * 0.26f,
        7.0f * scale,
        30.0f * scale);
    const float verticalRail = std::clamp(
        bounds.Height() * 0.18f,
        9.0f * scale,
        32.0f * scale);
    const float bevel = std::clamp(
        style_.metrics.cornerLength * 0.26f * scale,
        2.5f * scale,
        5.0f * scale);
    const float width = std::max(1.0f, style_.metrics.lineWidth * scale);
    const float outline = std::max(width + 1.5f * scale,
                                   style_.metrics.outlineWidth * scale);
    const ImU32 color = visible ? ToneColor(tone) : style_.colors.textMuted;
    const ImU32 sideColor = WithAlpha(color, visible ? 0.72f : 0.58f);
    const ImVec2 center = bounds.Center();

    const ImVec2 topLeft(center.x - horizontalRail, bounds.top);
    const ImVec2 topRight(center.x + horizontalRail, bounds.top);
    DrawOutlinedLine(drawList,
                     ImVec2(topLeft.x - bevel, topLeft.y + bevel),
                     topLeft,
                     color,
                     style_.colors.shadow,
                     width,
                     outline);
    DrawOutlinedLine(drawList, topLeft, topRight,
                     color, style_.colors.shadow, width, outline);
    DrawOutlinedLine(drawList,
                     topRight,
                     ImVec2(topRight.x + bevel, topRight.y + bevel),
                     color,
                     style_.colors.shadow,
                     width,
                     outline);

    const ImVec2 bottomLeft(center.x - horizontalRail * 0.72f, bounds.bottom);
    const ImVec2 bottomRight(center.x + horizontalRail * 0.72f, bounds.bottom);
    DrawOutlinedLine(drawList,
                     ImVec2(bottomLeft.x - bevel, bottomLeft.y - bevel),
                     bottomLeft,
                     sideColor,
                     style_.colors.shadow,
                     width,
                     outline);
    DrawOutlinedLine(drawList, bottomLeft, bottomRight,
                     sideColor, style_.colors.shadow, width, outline);
    DrawOutlinedLine(drawList,
                     bottomRight,
                     ImVec2(bottomRight.x + bevel, bottomRight.y - bevel),
                     sideColor,
                     style_.colors.shadow,
                     width,
                     outline);

    DrawOutlinedLine(drawList,
                     ImVec2(bounds.left, center.y - verticalRail),
                     ImVec2(bounds.left, center.y + verticalRail),
                     sideColor,
                     style_.colors.shadow,
                     width,
                     outline);
    DrawOutlinedLine(drawList,
                     ImVec2(bounds.right, center.y - verticalRail),
                     ImVec2(bounds.right, center.y + verticalRail),
                     sideColor,
                     style_.colors.shadow,
                     width,
                     outline);

    const float node = 3.0f * scale;
    const ImVec2 topNode(center.x, bounds.top);
    const ImVec2 diamond[4] = {
        ImVec2(topNode.x, topNode.y - node),
        ImVec2(topNode.x + node, topNode.y),
        ImVec2(topNode.x, topNode.y + node),
        ImVec2(topNode.x - node, topNode.y),
    };
    drawList->AddConvexPolyFilled(diamond, 4, style_.colors.shadow);
    const float innerNode = 1.7f * scale;
    const ImVec2 innerDiamond[4] = {
        ImVec2(topNode.x, topNode.y - innerNode),
        ImVec2(topNode.x + innerNode, topNode.y),
        ImVec2(topNode.x, topNode.y + innerNode),
        ImVec2(topNode.x - innerNode, topNode.y),
    };
    drawList->AddConvexPolyFilled(innerDiamond, 4,
                                  visible ? style_.colors.ally : style_.colors.textMuted);
}

void OverlayRenderer::DrawSkeleton(ImDrawList* drawList,
                                   const SkeletonVisual& skeleton,
                                   SemanticTone tone,
                                   bool visible) const {
    if (drawList == nullptr || skeleton.joints.empty() || skeleton.links.empty()) return;
    const float scale = style_.metrics.scale;
    const ImU32 color = visible ? ToneColor(tone) : style_.colors.textMuted;
    const float width = std::max(0.9f, style_.metrics.lineWidth * 0.72f * scale);
    std::vector<std::uint8_t> degrees(skeleton.joints.size(), 0);
    for (const BoneLink& link : skeleton.links) {
        if (link.first >= skeleton.joints.size() || link.second >= skeleton.joints.size()) continue;
        const BoneJoint& first = skeleton.joints[link.first];
        const BoneJoint& second = skeleton.joints[link.second];
        if (!first.valid || !second.valid || !Finite(first.position) || !Finite(second.position)) continue;
        degrees[link.first] = static_cast<std::uint8_t>(
            std::min<int>(degrees[link.first] + 1, 255));
        degrees[link.second] = static_cast<std::uint8_t>(
            std::min<int>(degrees[link.second] + 1, 255));
        DrawOutlinedLine(drawList, first.position, second.position,
                         WithAlpha(color, 0.82f), WithAlpha(style_.colors.shadow, 0.9f),
                         width, width + 2.0f * scale);
    }
    for (std::size_t index = 0; index < skeleton.joints.size(); ++index) {
        const BoneJoint& joint = skeleton.joints[index];
        if (!joint.valid || !Finite(joint.position) || degrees[index] < 3) continue;
        drawList->AddCircleFilled(joint.position, 2.4f * scale,
                                  WithAlpha(style_.colors.shadow, 0.9f), 12);
        drawList->AddCircleFilled(joint.position, 1.2f * scale,
                                  WithAlpha(color, 0.94f), 12);
    }
}

void OverlayRenderer::DrawVitalBars(ImDrawList* drawList,
                                    const ScreenRect& bounds,
                                    const VitalState& vitals) const {
    if (drawList == nullptr || !bounds.IsValid()) return;
    const float scale = style_.metrics.scale;
    const float healthRatio = Clamp01(vitals.health / std::max(vitals.maxHealth, 0.001f));
    const float armorRatio = Clamp01(vitals.armor / std::max(vitals.maxArmor, 0.001f));
    const ImU32 healthColor = vitals.downed || healthRatio < 0.25f
        ? style_.colors.danger
        : (healthRatio < 0.55f ? style_.colors.caution : style_.colors.accent);
    const float railWidth = std::clamp(bounds.Width(), 24.0f * scale, 126.0f * scale);
    const float x = bounds.Center().x - railWidth * 0.5f;
    const float healthHeight = 4.0f * scale;
    const float armorHeight = 2.5f * scale;
    const float gap = 2.0f * scale;
    const float top = bounds.bottom + 6.0f * scale;
    const ImVec2 healthMin(x, top);
    const ImVec2 healthMax(x + railWidth, top + healthHeight);
    DrawSegmentedTrack(drawList,
                       healthMin,
                       healthMax,
                       healthRatio,
                       style_.colors.surfaceRaised,
                       healthColor,
                       WithAlpha(style_.colors.shadow, 0.8f),
                       1.0f * scale,
                       5);
    drawList->AddRect(healthMin, healthMax, WithAlpha(style_.colors.border, 0.82f),
                      1.0f * scale, 0, 1.0f * scale);

    if (armorRatio > 0.0f) {
        const ImVec2 armorMin(x, healthMax.y + gap);
        const ImVec2 armorMax(x + railWidth, armorMin.y + armorHeight);
        DrawSegmentedTrack(drawList,
                           armorMin,
                           armorMax,
                           armorRatio,
                           style_.colors.surfaceRaised,
                           style_.colors.ally,
                           WithAlpha(style_.colors.shadow, 0.78f),
                           0.8f * scale,
                           4);
    }
}

void OverlayRenderer::DrawTracer(ImDrawList* drawList,
                                 const ImVec2& origin,
                                 const ImVec2& target,
                                 SemanticTone tone,
                                 bool visible) const {
    if (drawList == nullptr || !Finite(origin) || !Finite(target)) return;
    const float scale = style_.metrics.scale;
    const ImU32 color = visible ? ToneColor(tone) : style_.colors.textMuted;
    const ImVec2 delta = Subtract(target, origin);
    const float distance = Length(delta);
    if (distance <= 1.0f) return;
    const float width = std::max(0.8f, style_.metrics.lineWidth * 0.62f * scale);
    const float outline = width + 1.5f * scale;
    const ImVec2 firstEnd = Add(origin, Multiply(delta, 0.43f));
    const ImVec2 secondStart = Add(origin, Multiply(delta, 0.57f));
    DrawOutlinedLine(drawList, origin, firstEnd,
                     WithAlpha(color, 0.52f), WithAlpha(style_.colors.shadow, 0.58f),
                     width, outline);
    DrawOutlinedLine(drawList, secondStart, target,
                     WithAlpha(color, 0.74f), WithAlpha(style_.colors.shadow, 0.68f),
                     width, outline);
    const float node = 2.1f * scale;
    drawList->AddRectFilled(ImVec2(target.x - node, target.y - node),
                            ImVec2(target.x + node, target.y + node),
                            WithAlpha(style_.colors.shadow, 0.9f),
                            0.8f * scale);
    drawList->AddRectFilled(ImVec2(target.x - node * 0.55f, target.y - node * 0.55f),
                            ImVec2(target.x + node * 0.55f, target.y + node * 0.55f),
                            color,
                            0.5f * scale);
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

    const ImVec2 direction = Multiply(delta, 1.0f / length);
    const ImVec2 side = Perpendicular(direction);
    const ImU32 color = ToneColor(signal.tone);
    if (signal.kind == PlayerSignalKind::AimWarning) {
        DrawTracer(drawList, start, end, SemanticTone::Danger, true);
        DrawBrokenRing(drawList,
                       end,
                       7.0f * scale,
                       WithAlpha(style_.colors.danger, 0.94f),
                       1.6f * scale,
                       24);
        drawList->AddLine(
            Subtract(start, Multiply(side, 5.0f * scale)),
            Add(start, Multiply(side, 5.0f * scale)),
            WithAlpha(style_.colors.danger, 0.82f),
            1.8f * scale);
        return;
    }

    DrawOutlinedLine(drawList,
                     start,
                     end,
                     WithAlpha(color, 0.88f),
                     WithAlpha(style_.colors.shadow, 0.84f),
                     std::max(1.0f, style_.metrics.lineWidth * 0.76f * scale),
                     std::max(3.0f, style_.metrics.outlineWidth * 0.78f * scale));
    const float arrowLength = 6.0f * scale;
    const float arrowWidth = 3.0f * scale;
    const ImVec2 arrowBase = Subtract(end, Multiply(direction, arrowLength));
    drawList->AddTriangleFilled(
        end,
        Add(arrowBase, Multiply(side, arrowWidth)),
        Subtract(arrowBase, Multiply(side, arrowWidth)),
        color);
    drawList->AddCircleFilled(start, 2.2f * scale, style_.colors.ally, 12);
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
        0.75f, style_.metrics.lineWidth * 0.72f * scale);
    const ImU32 color = WithAlpha(ToneColor(model.tone), 0.82f);
    const ImU32 shadow = WithAlpha(style_.colors.shadow, 0.66f);
    for (const GeometrySegmentVisual& segment : model.segments) {
        if (!Finite(segment.start) || !Finite(segment.end)) continue;
        DrawOutlinedLine(
            drawList,
            segment.start,
            segment.end,
            color,
            shadow,
            width,
            width + 1.6f * scale);
    }
}

void OverlayRenderer::DrawOffscreenWarning(ImDrawList* drawList,
                                           const OffscreenMarker& marker,
                                           const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(marker.direction)) return;
    const float scale = style_.metrics.scale;
    const float markerScale =
        std::clamp(marker.markerScale, 0.25f, 3.0f);
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
    const float requestedRadius = marker.radiusPixels > 0.0f
        ? marker.radiusPixels
        : maximumRadius;
    const float radius = std::clamp(
        requestedRadius,
        std::min(20.0f * scale, maximumRadius),
        maximumRadius);
    const ImVec2 anchor = Add(center, Multiply(direction, radius));
    const ImVec2 side = Perpendicular(direction);
    const ImU32 color = ToneColor(marker.tone);
    const float railHalf = 10.0f * shapeScale;
    const ImVec2 railStart = Subtract(anchor, Multiply(side, railHalf));
    const ImVec2 railEnd = Add(anchor, Multiply(side, railHalf));
    DrawOutlinedLine(drawList,
                     railStart,
                     railEnd,
                     color,
                     style_.colors.shadow,
                     2.2f * shapeScale,
                     5.0f * shapeScale);
    DrawOutlinedLine(drawList,
                     Subtract(anchor, Multiply(direction, 3.0f * shapeScale)),
                     Add(anchor, Multiply(direction, 7.0f * shapeScale)),
                     style_.colors.ally,
                     style_.colors.shadow,
                     1.5f * shapeScale,
                     3.8f * shapeScale);
    const float node = 3.2f * shapeScale;
    const ImVec2 diamond[4] = {
        Subtract(anchor, Multiply(direction, node)),
        Add(anchor, Multiply(side, node)),
        Add(anchor, Multiply(direction, node)),
        Subtract(anchor, Multiply(side, node)),
    };
    drawList->AddConvexPolyFilled(diamond, 4, color);

    std::string caption = marker.label;
    const std::string distance = FormatDistance(marker.distanceMeters);
    if (!distance.empty()) {
        if (!caption.empty()) caption += "  ";
        caption += distance;
    }
    if (!caption.empty()) {
        const float fontSize = style_.metrics.smallFontSize * scale;
        const ImVec2 extent = TextExtent(caption, fontSize);
        const float panelWidth = std::min(extent.x + 12.0f * scale,
                                          viewport.Width() - 8.0f * scale);
        const float panelHeight = fontSize + 8.0f * scale;
        const ImVec2 inward = Subtract(
            anchor, Multiply(direction, 27.0f * shapeScale));
        ImVec2 minimum(inward.x - panelWidth * 0.5f,
                       inward.y - panelHeight * 0.5f);
        minimum.x = std::clamp(minimum.x,
                               viewport.left + 4.0f * scale,
                               viewport.right - panelWidth - 4.0f * scale);
        minimum.y = std::clamp(minimum.y,
                               viewport.top + 4.0f * scale,
                               viewport.bottom - panelHeight - 4.0f * scale);
        const ImVec2 maximum(minimum.x + panelWidth, minimum.y + panelHeight);
        DrawChamferedPanel(drawList,
                           minimum,
                           maximum,
                           WithAlpha(style_.colors.surfaceRaised, 0.9f),
                           WithAlpha(color, 0.7f),
                           4.0f * scale,
                           1.0f * scale);
        DrawText(drawList,
                 Add(minimum, ImVec2(6.0f * scale, 4.0f * scale)),
                 style_.colors.text,
                 style_.colors.shadow,
                 fontSize,
                 FitText(caption, panelWidth - 12.0f * scale, fontSize));
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
        drawList->AddCircleFilled(projectile.center, radius, WithAlpha(color, 0.045f), 72);
        DrawBrokenRing(drawList,
                       projectile.center,
                       radius,
                       WithAlpha(style_.colors.shadow, 0.86f),
                       4.0f * scale,
                       72);
        DrawBrokenRing(drawList,
                       projectile.center,
                       radius,
                       WithAlpha(color, 0.92f),
                       1.5f * scale,
                       72);
        const ImVec2 axes[4] = {
            ImVec2(0.0f, -1.0f), ImVec2(1.0f, 0.0f),
            ImVec2(0.0f, 1.0f), ImVec2(-1.0f, 0.0f),
        };
        for (const ImVec2& axis : axes) {
            drawList->AddLine(
                Add(projectile.center, Multiply(axis, radius - 4.0f * scale)),
                Add(projectile.center, Multiply(axis, radius + 4.0f * scale)),
                WithAlpha(style_.colors.ally, 0.82f),
                1.2f * scale);
        }
    }

    if (projectile.trajectory.size() >= 2) {
        for (std::size_t index = 1; index < projectile.trajectory.size(); ++index) {
            const ImVec2& first = projectile.trajectory[index - 1];
            const ImVec2& second = projectile.trajectory[index];
            if (!Finite(first) || !Finite(second)) continue;
            const ImVec2 visibleEnd = Add(first, Multiply(Subtract(second, first), 0.72f));
            DrawOutlinedLine(drawList,
                             first,
                             visibleEnd,
                             WithAlpha(color, index % 2 == 0 ? 0.92f : 0.68f),
                             WithAlpha(style_.colors.shadow, 0.82f),
                             1.5f * scale,
                             3.7f * scale);
        }
        const ImVec2 endpoint = projectile.trajectory.back();
        const float diamond = 4.5f * scale;
        const ImVec2 points[4] = {
            ImVec2(endpoint.x, endpoint.y - diamond),
            ImVec2(endpoint.x + diamond, endpoint.y),
            ImVec2(endpoint.x, endpoint.y + diamond),
            ImVec2(endpoint.x - diamond, endpoint.y),
        };
        drawList->AddConvexPolyFilled(points, 4, style_.colors.shadow);
        const float inner = 2.5f * scale;
        const ImVec2 innerPoints[4] = {
            ImVec2(endpoint.x, endpoint.y - inner),
            ImVec2(endpoint.x + inner, endpoint.y),
            ImVec2(endpoint.x, endpoint.y + inner),
            ImVec2(endpoint.x - inner, endpoint.y),
        };
        drawList->AddConvexPolyFilled(innerPoints, 4, color);
    }

    std::string caption = projectile.label;
    const std::string distance = FormatDistance(projectile.distanceMeters);
    if (!distance.empty()) {
        if (!caption.empty()) caption += "  ";
        caption += distance;
    }
    if (!caption.empty()) {
        const float fontSize = style_.metrics.fontSize * scale;
        const ImVec2 extent = TextExtent(caption, fontSize);
        const float width = std::min(extent.x + 14.0f * scale,
                                     viewport.Width() - 8.0f * scale);
        const float height = fontSize + 8.0f * scale;
        ImVec2 minimum(projectile.center.x - width * 0.5f,
                       projectile.center.y - height - 13.0f * scale);
        minimum.x = std::clamp(minimum.x, viewport.left + 4.0f * scale,
                               viewport.right - width - 4.0f * scale);
        minimum.y = std::clamp(minimum.y, viewport.top + 4.0f * scale,
                               viewport.bottom - height - 4.0f * scale);
        const ImVec2 maximum(minimum.x + width, minimum.y + height);
        DrawChamferedPanel(drawList,
                           minimum,
                           maximum,
                           WithAlpha(style_.colors.surfaceRaised, 0.88f),
                           WithAlpha(color, 0.72f),
                           4.0f * scale,
                           1.0f * scale);
        DrawText(drawList,
                 Add(minimum, ImVec2(7.0f * scale, 4.0f * scale)),
                 color,
                 style_.colors.shadow,
                 fontSize,
                 FitText(caption, width - 14.0f * scale, fontSize));
    }
}

void OverlayRenderer::DrawCrosshair(ImDrawList* drawList,
                                    const CrosshairVisual& crosshair) const {
    if (drawList == nullptr || !Finite(crosshair.center)) return;
    const float scale = style_.metrics.scale;
    const float gap = std::max(0.0f, crosshair.gap * scale);
    const float length = std::max(2.0f * scale, crosshair.armLength * scale);
    const float width = std::max(1.0f, crosshair.thickness * scale);
    const ImU32 color = ToneColor(crosshair.tone);
    const ImVec2 center = crosshair.center;
    const ImVec2 directions[4] = {
        ImVec2(-1.0f, 0.0f), ImVec2(1.0f, 0.0f),
        ImVec2(0.0f, -1.0f), ImVec2(0.0f, 1.0f),
    };
    for (int index = 0; index < 4; ++index) {
        const ImVec2& direction = directions[index];
        const ImVec2 side = Perpendicular(direction);
        const ImVec2 start = Add(center, Multiply(direction, gap));
        const ImVec2 end = Add(center, Multiply(direction, gap + length));
        const ImU32 armColor = index < 2
            ? color
            : WithAlpha(style_.colors.ally, 0.92f);
        DrawOutlinedLine(drawList,
                         start,
                         end,
                         armColor,
                         style_.colors.shadow,
                         width,
                         width + 2.0f * scale);
        drawList->AddLine(Subtract(end, Multiply(side, 2.5f * scale)),
                          Add(end, Multiply(side, 2.5f * scale)),
                          armColor,
                          width);
    }
    if (crosshair.centerDot) {
        const float outer = 2.8f * scale;
        const ImVec2 diamond[4] = {
            ImVec2(center.x, center.y - outer),
            ImVec2(center.x + outer, center.y),
            ImVec2(center.x, center.y + outer),
            ImVec2(center.x - outer, center.y),
        };
        drawList->AddConvexPolyFilled(diamond, 4, style_.colors.shadow);
        const float inner = 1.35f * scale;
        const ImVec2 innerDiamond[4] = {
            ImVec2(center.x, center.y - inner),
            ImVec2(center.x + inner, center.y),
            ImVec2(center.x, center.y + inner),
            ImVec2(center.x - inner, center.y),
        };
        drawList->AddConvexPolyFilled(innerDiamond, 4, color);
    }
}

void OverlayRenderer::DrawAimGuide(ImDrawList* drawList,
                                   const AimGuide& guide,
                                   const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(guide.center)) return;
    const float scale = style_.metrics.scale;
    const ImU32 color = guide.locked ? style_.colors.danger : style_.colors.accent;
    if (guide.drawCircle && std::isfinite(guide.radius) && guide.radius >= 1.0f) {
        const float radius = std::min(
            guide.radius,
            std::max(viewport.Width(), viewport.Height()));
        const float width = std::max(1.0f, style_.metrics.lineWidth * 0.82f * scale);
        DrawBrokenRing(drawList,
                       guide.center,
                       radius,
                       WithAlpha(style_.colors.shadow, 0.62f),
                       width + 1.8f * scale,
                       96);
        DrawBrokenRing(drawList,
                       guide.center,
                       radius,
                       WithAlpha(color, 0.68f),
                       width,
                       96);
        for (int index = 0; index < 4; ++index) {
            const ImVec2 axis = RotateVector(ImVec2(0.0f, -1.0f),
                                             static_cast<float>(index) * kPi * 0.5f);
            drawList->AddLine(
                Add(guide.center, Multiply(axis, radius - 3.0f * scale)),
                Add(guide.center, Multiply(axis, radius + 3.0f * scale)),
                WithAlpha(style_.colors.ally, guide.locked ? 0.56f : 0.78f),
                1.2f * scale);
        }
    }
    if (guide.drawTargetRay && guide.targetValid && Finite(guide.target)) {
        const ImVec2 target = ClampPoint(guide.target, viewport, 5.0f * scale);
        const ImVec2 delta = Subtract(target, guide.center);
        DrawOutlinedLine(
            drawList,
            Add(guide.center, Multiply(delta, 0.08f)),
            Add(guide.center, Multiply(delta, 0.76f)),
            WithAlpha(color, 0.76f),
            WithAlpha(style_.colors.shadow, 0.72f),
            std::max(1.0f, style_.metrics.lineWidth * 0.82f * scale),
            std::max(2.0f, style_.metrics.outlineWidth * 0.84f * scale));
        const float marker = guide.locked ? 6.0f * scale : 4.5f * scale;
        drawList->AddRect(ImVec2(target.x - marker, target.y - marker),
                          ImVec2(target.x + marker, target.y + marker),
                          style_.colors.shadow,
                          1.0f * scale,
                          0,
                          3.5f * scale);
        drawList->AddRect(ImVec2(target.x - marker, target.y - marker),
                          ImVec2(target.x + marker, target.y + marker),
                          color,
                          1.0f * scale,
                          0,
                          1.4f * scale);
        drawList->AddCircleFilled(target, 1.4f * scale, style_.colors.ally, 12);
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
    const float extent = std::clamp(
        region.halfExtent,
        20.0f * scale,
        std::max(viewport.Width(), viewport.Height()));
    const ScreenRect bounds{
        std::max(viewport.left, region.center.x - extent),
        std::max(viewport.top, region.center.y - extent),
        std::min(viewport.right, region.center.x + extent),
        std::min(viewport.bottom, region.center.y + extent),
    };
    if (!bounds.IsValid()) return;

    const ImU32 blue = WithAlpha(style_.colors.ally, 0.66f);
    const ImU32 green = WithAlpha(style_.colors.accent, 0.58f);
    DrawChamferedPanel(drawList,
                       ImVec2(bounds.left, bounds.top),
                       ImVec2(bounds.right, bounds.bottom),
                       WithAlpha(style_.colors.ally, 0.025f),
                       WithAlpha(style_.colors.ally, 0.18f),
                       14.0f * scale,
                       1.0f * scale);

    const ImVec2 center = bounds.Center();
    const float horizontalRail = std::clamp(
        bounds.Width() * 0.18f,
        22.0f * scale,
        72.0f * scale);
    const float verticalRail = std::clamp(
        bounds.Height() * 0.18f,
        22.0f * scale,
        72.0f * scale);
    const float width = std::max(1.0f, style_.metrics.lineWidth * 0.68f * scale);
    drawList->AddLine(ImVec2(center.x - horizontalRail, bounds.top),
                      ImVec2(center.x + horizontalRail, bounds.top),
                      blue,
                      width);
    drawList->AddLine(ImVec2(center.x - horizontalRail, bounds.bottom),
                      ImVec2(center.x + horizontalRail, bounds.bottom),
                      blue,
                      width);
    drawList->AddLine(ImVec2(bounds.left, center.y - verticalRail),
                      ImVec2(bounds.left, center.y + verticalRail),
                      green,
                      width);
    drawList->AddLine(ImVec2(bounds.right, center.y - verticalRail),
                      ImVec2(bounds.right, center.y + verticalRail),
                      green,
                      width);

    const float innerGap = 5.0f * scale;
    const float innerLength = 8.0f * scale;
    drawList->AddLine(ImVec2(region.center.x - innerGap - innerLength, region.center.y),
                      ImVec2(region.center.x - innerGap, region.center.y),
                      blue,
                      width);
    drawList->AddLine(ImVec2(region.center.x + innerGap, region.center.y),
                      ImVec2(region.center.x + innerGap + innerLength, region.center.y),
                      blue,
                      width);
    drawList->AddLine(ImVec2(region.center.x, region.center.y - innerGap - innerLength),
                      ImVec2(region.center.x, region.center.y - innerGap),
                      green,
                      width);
    drawList->AddLine(ImVec2(region.center.x, region.center.y + innerGap),
                      ImVec2(region.center.x, region.center.y + innerGap + innerLength),
                      green,
                      width);
    drawList->AddCircleFilled(region.center, 1.8f * scale, style_.colors.text, 12);
}

void OverlayRenderer::DrawRadar(ImDrawList* drawList,
                                const RadarVisual& radar,
                                const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || !Finite(radar.center)) return;
    const float scale = style_.metrics.scale;
    const float radius = std::clamp(
        radar.radius > 0.0f ? radar.radius : style_.metrics.radarRadius * scale,
        45.0f * scale,
        std::min(viewport.Width(), viewport.Height()) * 0.45f);
    const ImVec2 center = ClampPoint(radar.center, viewport, radius + 4.0f * scale);
    const ImVec2 minimum(center.x - radius, center.y - radius);
    const ImVec2 maximum(center.x + radius, center.y + radius);
    const float cut = 12.0f * scale;
    DrawChamferedPanel(drawList,
                       Add(minimum, ImVec2(2.0f * scale, 3.0f * scale)),
                       Add(maximum, ImVec2(2.0f * scale, 3.0f * scale)),
                       WithAlpha(style_.colors.shadow, 0.68f),
                       WithAlpha(style_.colors.shadow, 0.68f),
                       cut,
                       1.0f * scale);
    DrawChamferedPanel(drawList,
                       minimum,
                       maximum,
                       style_.colors.surface,
                       style_.colors.border,
                       cut,
                       1.2f * scale);
    drawList->AddLine(ImVec2(minimum.x + 1.0f * scale, minimum.y),
                      ImVec2(center.x - 10.0f * scale, minimum.y),
                      WithAlpha(style_.colors.accent, 0.92f),
                      2.0f * scale);
    drawList->AddLine(ImVec2(center.x + 24.0f * scale, maximum.y),
                      ImVec2(maximum.x - cut, maximum.y),
                      WithAlpha(style_.colors.ally, 0.86f),
                      1.6f * scale);

    const float insetRadius = radius - 10.0f * scale;
    for (int index = 1; index < 4; ++index) {
        const float offset = -insetRadius +
            insetRadius * 2.0f * static_cast<float>(index) / 4.0f;
        drawList->AddLine(ImVec2(center.x + offset, minimum.y + cut),
                          ImVec2(center.x + offset, maximum.y - cut),
                          style_.colors.grid,
                          1.0f * scale);
        drawList->AddLine(ImVec2(minimum.x + cut, center.y + offset),
                          ImVec2(maximum.x - cut, center.y + offset),
                          style_.colors.grid,
                          1.0f * scale);
    }
    const ImVec2 diamond[4] = {
        ImVec2(center.x, center.y - insetRadius * 0.5f),
        ImVec2(center.x + insetRadius * 0.5f, center.y),
        ImVec2(center.x, center.y + insetRadius * 0.5f),
        ImVec2(center.x - insetRadius * 0.5f, center.y),
    };
    drawList->AddPolyline(diamond, 4, WithAlpha(style_.colors.ally, 0.22f),
                          ImDrawFlags_Closed, 1.0f * scale);
    drawList->AddLine(center,
                      ImVec2(center.x, minimum.y + cut),
                      WithAlpha(style_.colors.accent, 0.26f),
                      1.2f * scale);

    for (const RadarBlip& blip : radar.blips) {
        if (!Finite(blip.normalizedPosition)) continue;
        ImVec2 normalized(
            std::clamp(blip.normalizedPosition.x, -1.0f, 1.0f),
            std::clamp(blip.normalizedPosition.y, -1.0f, 1.0f));
        const float normalizedLength = Length(normalized);
        if (normalizedLength > 1.0f) normalized = Multiply(normalized, 1.0f / normalizedLength);
        const ImVec2 position = Add(center, Multiply(normalized, insetRadius));
        const ImU32 color = ToneColor(blip.tone);
        const float size = 4.0f * scale;

        if (blip.kind == RadarBlipKind::Self ||
            (blip.kind == RadarBlipKind::Player && blip.headingValid)) {
            const float heading = blip.headingRadians - radar.viewHeadingRadians - kPi * 0.5f;
            const ImVec2 forward(std::cos(heading), std::sin(heading));
            DrawOutlinedLine(drawList,
                             Subtract(position, Multiply(forward, size * 0.7f)),
                             Add(position, Multiply(forward, size * 1.8f)),
                             color,
                             style_.colors.shadow,
                             1.4f * scale,
                             3.2f * scale);
            const float node = 2.5f * scale;
            drawList->AddRectFilled(ImVec2(position.x - node, position.y - node),
                                    ImVec2(position.x + node, position.y + node),
                                    color,
                                    0.8f * scale);
        } else if (blip.kind == RadarBlipKind::Player) {
            drawList->AddRectFilled(ImVec2(position.x - size * 0.75f,
                                           position.y - size * 0.75f),
                                    ImVec2(position.x + size * 0.75f,
                                           position.y + size * 0.75f),
                                    color,
                                    0.8f * scale);
        } else if (blip.kind == RadarBlipKind::Bot) {
            const ImVec2 botDiamond[4] = {
                ImVec2(position.x, position.y - size),
                ImVec2(position.x + size, position.y),
                ImVec2(position.x, position.y + size),
                ImVec2(position.x - size, position.y),
            };
            drawList->AddConvexPolyFilled(botDiamond, 4, style_.colors.shadow);
            drawList->AddPolyline(botDiamond, 4, color, ImDrawFlags_Closed, 1.3f * scale);
            if (blip.headingValid) {
                const float heading =
                    blip.headingRadians - radar.viewHeadingRadians - kPi * 0.5f;
                const ImVec2 forward(std::cos(heading), std::sin(heading));
                drawList->AddLine(
                    position,
                    Add(position, Multiply(forward, size * 1.8f)),
                    color,
                    1.2f * scale);
            }
        } else {
            drawList->AddRectFilled(ImVec2(position.x - size * 0.65f,
                                           position.y - size * 0.65f),
                                    ImVec2(position.x + size * 0.65f,
                                           position.y + size * 0.65f),
                                    color,
                                    1.0f * scale);
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
        const ImVec2 selfTip(center.x, center.y - selfSize * 1.4f);
        const ImVec2 selfLeft(center.x - selfSize, center.y + selfSize * 0.7f);
        const ImVec2 selfRight(center.x + selfSize, center.y + selfSize * 0.7f);
        DrawOutlinedLine(drawList, selfLeft, selfTip,
                         style_.colors.accent, style_.colors.shadow,
                         1.7f * scale, 3.8f * scale);
        DrawOutlinedLine(drawList, selfTip, selfRight,
                         style_.colors.accent, style_.colors.shadow,
                         1.7f * scale, 3.8f * scale);
        drawList->AddCircleFilled(center, 1.6f * scale, style_.colors.ally, 12);
    }
}

void OverlayRenderer::DrawHudMap(ImDrawList* drawList,
                                 const HudMapVisual& map,
                                 const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid()) return;
    const float scale = style_.metrics.scale;
    const float size = std::clamp(map.markerSize * scale, 1.0f, 40.0f * scale);
    const float labelSize = std::clamp(map.fontSize * scale, 0.0f, 80.0f * scale);

    for (const HudMapMarker& marker : map.markers) {
        if (!Finite(marker.position) ||
            marker.position.x < viewport.left - size * 2.0f ||
            marker.position.x > viewport.right + size * 2.0f ||
            marker.position.y < viewport.top - size * 2.0f ||
            marker.position.y > viewport.bottom + size * 2.0f) {
            continue;
        }

        const ImU32 color = ToneColor(marker.tone);
        if (marker.drawDirection && Finite(marker.directionEnd)) {
            DrawOutlinedLine(drawList,
                             marker.position,
                             marker.directionEnd,
                             color,
                             style_.colors.shadow,
                             1.6f * scale,
                             4.0f * scale);
            drawList->AddCircleFilled(marker.directionEnd,
                                      1.7f * scale,
                                      WithAlpha(color, 0.95f),
                                      10);
        }

        if (marker.kind == RadarBlipKind::Self) {
            const ImVec2 points[4] = {
                ImVec2(marker.position.x, marker.position.y - size * 1.25f),
                ImVec2(marker.position.x + size, marker.position.y + size * 0.85f),
                ImVec2(marker.position.x, marker.position.y + size * 0.45f),
                ImVec2(marker.position.x - size, marker.position.y + size * 0.85f),
            };
            drawList->AddConvexPolyFilled(points, 4, style_.colors.surfaceRaised);
            drawList->AddPolyline(points, 4, color, ImDrawFlags_Closed, 2.0f * scale);
            drawList->AddCircleFilled(marker.position, 1.8f * scale,
                                      style_.colors.ally, 10);
        } else if (marker.kind == RadarBlipKind::Bot) {
            const ImVec2 points[4] = {
                ImVec2(marker.position.x, marker.position.y - size),
                ImVec2(marker.position.x + size, marker.position.y),
                ImVec2(marker.position.x, marker.position.y + size),
                ImVec2(marker.position.x - size, marker.position.y),
            };
            drawList->AddConvexPolyFilled(points, 4, WithAlpha(style_.colors.surface, 0.86f));
            drawList->AddPolyline(points, 4, color, ImDrawFlags_Closed, 1.7f * scale);
            drawList->AddCircleFilled(marker.position, 1.5f * scale, color, 10);
        } else {
            const float cut = size * 0.38f;
            const ImVec2 points[8] = {
                ImVec2(marker.position.x - size + cut, marker.position.y - size),
                ImVec2(marker.position.x + size - cut, marker.position.y - size),
                ImVec2(marker.position.x + size, marker.position.y - size + cut),
                ImVec2(marker.position.x + size, marker.position.y + size - cut),
                ImVec2(marker.position.x + size - cut, marker.position.y + size),
                ImVec2(marker.position.x - size + cut, marker.position.y + size),
                ImVec2(marker.position.x - size, marker.position.y + size - cut),
                ImVec2(marker.position.x - size, marker.position.y - size + cut),
            };
            drawList->AddConvexPolyFilled(points, 8, WithAlpha(style_.colors.surface, 0.90f));
            drawList->AddPolyline(points, 8, color, ImDrawFlags_Closed, 1.8f * scale);
            drawList->AddRectFilled(
                ImVec2(marker.position.x - 1.4f * scale,
                       marker.position.y - 1.4f * scale),
                ImVec2(marker.position.x + 1.4f * scale,
                       marker.position.y + 1.4f * scale),
                color);
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
    const float titleSize = label.titleSizeOverride > 0.0f
        ? std::clamp(label.titleSizeOverride * scale, 8.0f * scale, 48.0f * scale)
        : style_.metrics.fontSize * scale;
    const float detailSize = style_.metrics.smallFontSize * scale;
    const float markerSize = label.emphasized ? 5.0f * scale : 3.8f * scale;
    const ImU32 color = label.colorOverride != 0
        ? label.colorOverride
        : ToneColor(label.tone);
    const ImVec2 anchor = ClampPoint(label.anchor, viewport, 6.0f * scale);

    if (label.kind == WorldLabelKind::ScreenAlert) {
        const float alertTitleSize = std::max(titleSize, style_.metrics.fontSize * 1.22f * scale);
        const float alertDetailSize = std::max(detailSize, style_.metrics.smallFontSize * scale);
        const ImVec2 titleExtent = TextExtent(label.title, alertTitleSize);
        const ImVec2 detailExtent = TextExtent(label.detail, alertDetailSize);
        const float paddingX = 14.0f * scale;
        const float paddingY = 7.0f * scale;
        const float gap = label.detail.empty() ? 0.0f : 2.0f * scale;
        const float width = std::clamp(
            std::max(titleExtent.x, detailExtent.x) + paddingX * 2.0f,
            132.0f * scale,
            std::max(132.0f * scale, viewport.Width() - 12.0f * scale));
        const float height = paddingY * 2.0f + alertTitleSize +
            (label.detail.empty() ? 0.0f : alertDetailSize + gap);
        ImVec2 minimum(anchor.x - width * 0.5f, anchor.y - height * 0.5f);
        minimum.x = std::clamp(minimum.x,
                               viewport.left + 6.0f * scale,
                               viewport.right - width - 6.0f * scale);
        minimum.y = std::clamp(minimum.y,
                               viewport.top + 6.0f * scale,
                               viewport.bottom - height - 6.0f * scale);
        const ImVec2 maximum(minimum.x + width, minimum.y + height);
        DrawChamferedPanel(drawList,
                           Add(minimum, ImVec2(2.0f * scale, 3.0f * scale)),
                           Add(maximum, ImVec2(2.0f * scale, 3.0f * scale)),
                           WithAlpha(style_.colors.shadow, 0.72f),
                           WithAlpha(style_.colors.shadow, 0.72f),
                           7.0f * scale,
                           1.0f * scale);
        DrawChamferedPanel(drawList,
                           minimum,
                           maximum,
                           WithAlpha(style_.colors.surfaceRaised, 0.95f),
                           WithAlpha(color, 0.82f),
                           7.0f * scale,
                           1.0f * scale);
        drawList->AddLine(ImVec2(minimum.x + 1.0f * scale, minimum.y),
                          ImVec2(minimum.x + width * 0.46f, minimum.y),
                          color,
                          2.0f * scale);
        const std::string fittedTitle = FitText(
            label.title, width - paddingX * 2.0f, alertTitleSize);
        const ImVec2 fittedTitleExtent = TextExtent(fittedTitle, alertTitleSize);
        DrawText(drawList,
                 ImVec2(minimum.x + (width - fittedTitleExtent.x) * 0.5f,
                        minimum.y + paddingY),
                 style_.colors.text,
                 style_.colors.shadow,
                 alertTitleSize,
                 fittedTitle);
        if (!label.detail.empty()) {
            const std::string fittedDetail = FitText(
                label.detail, width - paddingX * 2.0f, alertDetailSize);
            const ImVec2 fittedDetailExtent = TextExtent(fittedDetail, alertDetailSize);
            DrawText(drawList,
                     ImVec2(minimum.x + (width - fittedDetailExtent.x) * 0.5f,
                            minimum.y + paddingY + alertTitleSize + gap),
                     color,
                     style_.colors.shadow,
                     alertDetailSize,
                     fittedDetail);
        }
        return;
    }

    if (label.kind == WorldLabelKind::Container) {
        drawList->AddRect(
            ImVec2(anchor.x - markerSize, anchor.y - markerSize),
            ImVec2(anchor.x + markerSize, anchor.y + markerSize),
            style_.colors.shadow,
            1.0f * scale,
            0,
            3.0f * scale);
        drawList->AddRect(
            ImVec2(anchor.x - markerSize, anchor.y - markerSize),
            ImVec2(anchor.x + markerSize, anchor.y + markerSize),
            color,
            1.0f * scale,
            0,
            1.2f * scale);
        drawList->AddRectFilled(
            ImVec2(anchor.x - 1.2f * scale, anchor.y - 1.2f * scale),
            ImVec2(anchor.x + 1.2f * scale, anchor.y + 1.2f * scale),
            style_.colors.ally,
            0.4f * scale);
    } else {
        DrawOutlinedLine(drawList,
                         ImVec2(anchor.x - markerSize, anchor.y),
                         ImVec2(anchor.x + markerSize, anchor.y),
                         color,
                         style_.colors.shadow,
                         1.5f * scale,
                         3.4f * scale);
        drawList->AddCircleFilled(anchor, 1.5f * scale, style_.colors.ally, 10);
    }

    const ImVec2 titleExtent = TextExtent(label.title, titleSize);
    const ImVec2 detailExtent = TextExtent(label.detail, detailSize);
    const float paddingX = 6.0f * scale;
    const float paddingY = 4.0f * scale;
    const float width = std::min(
        std::max(titleExtent.x, detailExtent.x) + paddingX * 2.0f,
        std::max(1.0f,
                 std::min(240.0f * scale, viewport.Width() - 8.0f * scale)));
    const float height = paddingY * 2.0f + titleSize +
                         (label.detail.empty() ? 0.0f : detailSize + 1.0f * scale);
    ImVec2 minimum(anchor.x + 9.0f * scale, anchor.y - height * 0.5f);
    if (minimum.x + width > viewport.right - 4.0f * scale) {
        minimum.x = anchor.x - 9.0f * scale - width;
    }
    minimum.x = std::clamp(minimum.x,
                           viewport.left + 4.0f * scale,
                           viewport.right - width - 4.0f * scale);
    minimum.y = std::clamp(minimum.y,
                           viewport.top + 4.0f * scale,
                           viewport.bottom - height - 4.0f * scale);
    const ImVec2 maximum(minimum.x + width, minimum.y + height);
    const bool panelOnRight = minimum.x >= anchor.x;
    const ImVec2 connectorEnd(panelOnRight ? minimum.x : maximum.x,
                              std::clamp(anchor.y,
                                         minimum.y + 3.0f * scale,
                                         maximum.y - 3.0f * scale));
    drawList->AddLine(anchor,
                      connectorEnd,
                      WithAlpha(color, 0.62f),
                      1.0f * scale);
    DrawChamferedPanel(drawList,
                       minimum,
                       maximum,
                       WithAlpha(style_.colors.surfaceRaised, label.emphasized ? 0.94f : 0.86f),
                       label.emphasized ? WithAlpha(color, 0.74f) : style_.colors.border,
                       5.0f * scale,
                       1.0f * scale);
    drawList->AddLine(
        ImVec2(minimum.x + 1.0f * scale, minimum.y),
        ImVec2(minimum.x + std::min(width * 0.45f, 58.0f * scale), minimum.y),
        WithAlpha(color, label.emphasized ? 1.0f : 0.72f),
        label.emphasized ? 2.0f * scale : 1.4f * scale);

    const float textWidth = width - paddingX * 2.0f;
    DrawText(drawList,
             Add(minimum, ImVec2(paddingX, paddingY)),
             label.emphasized ? color : style_.colors.text,
             style_.colors.shadow,
             titleSize,
             FitText(label.title, textWidth, titleSize));
    if (!label.detail.empty()) {
        DrawText(drawList,
                 Add(minimum, ImVec2(paddingX, paddingY + titleSize + scale)),
                 style_.colors.textMuted,
                 style_.colors.shadow,
                 detailSize,
                 FitText(label.detail, textWidth, detailSize));
    }
}

void OverlayRenderer::DrawHighValueList(ImDrawList* drawList,
                                        const HighValueList& list,
                                        const ScreenRect& viewport) const {
    if (drawList == nullptr || !viewport.IsValid() || list.entries.empty() || list.maxRows <= 0) return;
    const float scale = style_.metrics.scale;
    const float headerHeight = list.title.empty() ? 0.0f : 30.0f * scale;
    const float rowHeight = 27.0f * scale;
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
    const ImVec2 maximum(origin.x + width, origin.y + height);

    const float cut = 8.0f * scale;
    DrawChamferedPanel(drawList,
                       Add(origin, ImVec2(2.0f * scale, 3.0f * scale)),
                       Add(maximum, ImVec2(2.0f * scale, 3.0f * scale)),
                       WithAlpha(style_.colors.shadow, 0.62f),
                       WithAlpha(style_.colors.shadow, 0.62f),
                       cut,
                       1.0f * scale);
    DrawChamferedPanel(drawList,
                       origin,
                       maximum,
                       style_.colors.surface,
                       style_.colors.border,
                       cut,
                       1.0f * scale);
    drawList->AddLine(ImVec2(origin.x + 1.0f * scale, origin.y),
                      ImVec2(origin.x + std::min(width * 0.36f, 96.0f * scale), origin.y),
                      style_.colors.accent,
                      2.0f * scale);
    drawList->AddLine(ImVec2(maximum.x - 48.0f * scale, maximum.y),
                      ImVec2(maximum.x - cut, maximum.y),
                      WithAlpha(style_.colors.ally, 0.82f),
                      1.5f * scale);

    float rowY = origin.y;
    if (!list.title.empty()) {
        DrawText(drawList,
                 ImVec2(origin.x + 12.0f * scale, origin.y + 7.0f * scale),
                 style_.colors.text,
                 style_.colors.shadow,
                 style_.metrics.fontSize * scale,
                 FitText(list.title, width - 24.0f * scale, style_.metrics.fontSize * scale));
        drawList->AddLine(ImVec2(origin.x + 10.0f * scale,
                                 origin.y + headerHeight),
                          ImVec2(maximum.x - 10.0f * scale,
                                 origin.y + headerHeight),
                          style_.colors.border,
                          1.0f * scale);
        rowY += headerHeight;
    }

    for (int index = 0; index < rowCount; ++index) {
        const HighValueEntry& entry = list.entries[static_cast<std::size_t>(index)];
        if (index > 0) {
            drawList->AddLine(ImVec2(origin.x + 12.0f * scale, rowY),
                              ImVec2(maximum.x - 12.0f * scale, rowY),
                              style_.colors.grid,
                              1.0f * scale);
        }
        const ImU32 color = ToneColor(entry.tone);
        const float markerX = origin.x + 11.0f * scale;
        const float middleY = rowY + rowHeight * 0.5f;
        drawList->AddRectFilled(
            ImVec2(markerX - 1.0f * scale, middleY - 5.0f * scale),
            ImVec2(markerX + 1.0f * scale, middleY + 5.0f * scale),
            color,
            0.8f * scale);

        const std::string value = FormatValue(entry.value);
        const std::string distance = FormatDistance(entry.distanceMeters);
        const ImVec2 valueExtent = TextExtent(value, style_.metrics.smallFontSize * scale);
        const ImVec2 distanceExtent = TextExtent(distance, style_.metrics.smallFontSize * scale);
        const float valueX = maximum.x - 9.0f * scale - valueExtent.x;
        const float distanceX = valueX - 10.0f * scale - distanceExtent.x;
        const float nameX = origin.x + 19.0f * scale;
        const float nameWidth = std::max(0.0f, distanceX - nameX - 8.0f * scale);
        DrawText(drawList,
                 ImVec2(nameX, rowY + 6.0f * scale),
                 style_.colors.text,
                 style_.colors.shadow,
                 style_.metrics.smallFontSize * scale,
                 FitText(entry.name, nameWidth, style_.metrics.smallFontSize * scale));
        if (!distance.empty()) {
            DrawText(drawList,
                     ImVec2(distanceX, rowY + 6.0f * scale),
                     style_.colors.textMuted,
                     style_.colors.shadow,
                     style_.metrics.smallFontSize * scale,
                     distance);
        }
        DrawText(drawList,
                 ImVec2(valueX, rowY + 6.0f * scale),
                 color,
                 style_.colors.shadow,
                 style_.metrics.smallFontSize * scale,
                 value);
        rowY += rowHeight;
    }
}

}  // namespace lengjing
