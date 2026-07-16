#pragma once

#include "render/render_types.h"

namespace lengjing {

class OverlayRenderer final {
public:
    explicit OverlayRenderer(RenderStyle style = RenderStyle::Default());

    const RenderStyle& Style() const;
    void SetStyle(const RenderStyle& style);

    void DrawPlayer(ImDrawList* drawList,
                    const PlayerVisual& player,
                    const ScreenRect& viewport) const;
    void DrawPlayerPlate(ImDrawList* drawList,
                         const PlayerVisual& player,
                         const ScreenRect& viewport) const;
    void DrawCornerBox(ImDrawList* drawList,
                       const ScreenRect& bounds,
                       SemanticTone tone,
                       bool visible = true) const;
    void DrawSkeleton(ImDrawList* drawList,
                      const SkeletonVisual& skeleton,
                      SemanticTone tone,
                      bool visible = true) const;
    void DrawVitalBars(ImDrawList* drawList,
                       const ScreenRect& bounds,
                       const VitalState& vitals) const;
    void DrawTracer(ImDrawList* drawList,
                    const ImVec2& origin,
                    const ImVec2& target,
                    SemanticTone tone,
                    bool visible = true) const;
    void DrawPlayerSignal(ImDrawList* drawList,
                          const PlayerSignalVisual& signal,
                          const ScreenRect& viewport) const;
    void DrawModelGeometry(ImDrawList* drawList,
                           const GeometryModelVisual& model,
                           const ScreenRect& viewport) const;

    void DrawOffscreenWarning(ImDrawList* drawList,
                              const OffscreenMarker& marker,
                              const ScreenRect& viewport) const;
    void DrawProjectile(ImDrawList* drawList,
                        const ProjectileVisual& projectile,
                        const ScreenRect& viewport) const;
    void DrawCrosshair(ImDrawList* drawList,
                       const CrosshairVisual& crosshair) const;
    void DrawAimGuide(ImDrawList* drawList,
                      const AimGuide& guide,
                      const ScreenRect& viewport) const;
    void DrawTouchRegion(ImDrawList* drawList,
                         const TouchRegionVisual& region,
                         const ScreenRect& viewport) const;
    void DrawRadar(ImDrawList* drawList,
                   const RadarVisual& radar,
                   const ScreenRect& viewport) const;
    void DrawHudMap(ImDrawList* drawList,
                    const HudMapVisual& map,
                    const ScreenRect& viewport) const;
    void DrawWorldLabel(ImDrawList* drawList,
                        const WorldLabel& label,
                        const ScreenRect& viewport) const;
    void DrawHighValueList(ImDrawList* drawList,
                           const HighValueList& list,
                           const ScreenRect& viewport) const;

private:
    ImU32 ToneColor(SemanticTone tone) const;
    RenderStyle style_{};
};

}  // namespace lengjing
