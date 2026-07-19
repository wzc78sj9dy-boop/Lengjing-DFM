#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ImGui/imgui.h"
#include "game/VisibilityState.h"

namespace lengjing {

enum class SemanticTone : std::uint8_t {
    Neutral,
    Accent,
    Caution,
    Danger,
    Ally,
    Muted,
};

enum class WorldLabelKind : std::uint8_t {
    Item,
    Container,
    ScreenAlert,
};

enum class RadarBlipKind : std::uint8_t {
    Self,
    Player,
    Bot,
    Item,
};

struct RenderPalette {
    ImU32 surface = IM_COL32(14, 18, 18, 226);
    ImU32 surfaceRaised = IM_COL32(21, 26, 26, 240);
    ImU32 surfaceSoft = IM_COL32(27, 33, 32, 210);
    ImU32 border = IM_COL32(46, 55, 52, 196);
    ImU32 grid = IM_COL32(71, 85, 80, 74);
    ImU32 text = IM_COL32(232, 238, 234, 255);
    ImU32 textMuted = IM_COL32(143, 158, 151, 255);
    ImU32 accent = IM_COL32(27, 181, 137, 255);
    ImU32 caution = IM_COL32(214, 164, 67, 255);
    ImU32 danger = IM_COL32(221, 83, 83, 255);
    ImU32 ally = IM_COL32(66, 153, 220, 255);
    ImU32 shadow = IM_COL32(0, 0, 0, 202);
};

struct RenderMetrics {
    float scale = 1.0f;
    float lineWidth = 2.0f;
    float outlineWidth = 4.0f;
    float cornerLength = 14.0f;
    float panelRounding = 4.0f;
    float fontSize = 22.0f;
    float smallFontSize = 19.0f;
    float radarRadius = 110.0f;
};

struct RenderStyle {
    RenderPalette colors{};
    RenderMetrics metrics{};

    static RenderStyle Default();
};

struct ScreenRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    bool IsValid() const;
    float Width() const;
    float Height() const;
    ImVec2 Center() const;
};

struct BoneJoint {
    ImVec2 position{};
    bool valid = false;
    game::VisibilityState visibility = game::VisibilityState::Unavailable;
};

struct BoneLink {
    std::uint16_t first = 0;
    std::uint16_t second = 0;
};

struct SkeletonVisual {
    std::vector<BoneJoint> joints;
    std::vector<BoneLink> links;
    bool colorByVisibility = false;
    int selectedJoint = -1;
};

struct VitalState {
    float health = 0.0f;
    float maxHealth = 100.0f;
    float armor = 0.0f;
    float maxArmor = 100.0f;
    bool downed = false;
};

struct PlayerVisual {
    std::uint64_t identity = 0;
    ScreenRect bounds{};
    SkeletonVisual skeleton{};
    VitalState vitals{};
    std::string name;
    std::string detail;
    SemanticTone tone = SemanticTone::Danger;
    ImVec2 tracerOrigin{};
    bool drawPlate = true;
    bool drawCornerBox = true;
    bool drawSkeleton = true;
    bool drawVitals = true;
    bool drawTracer = false;
    bool visible = true;
    bool coverHighlighted = false;
};

struct OffscreenMarker {
    ImVec2 direction{};
    float radiusPixels = 0.0f;
    float markerScale = 1.0f;
    float distanceMeters = 0.0f;
    std::string label;
    SemanticTone tone = SemanticTone::Danger;
};

enum class PlayerSignalKind : std::uint8_t {
    ViewDirection,
    AimWarning,
};

struct PlayerSignalVisual {
    ImVec2 start{};
    ImVec2 end{};
    PlayerSignalKind kind = PlayerSignalKind::ViewDirection;
    SemanticTone tone = SemanticTone::Danger;
};

struct GeometrySegmentVisual {
    ImVec2 start{};
    ImVec2 end{};
};

struct GeometryModelVisual {
    std::vector<GeometrySegmentVisual> segments;
    SemanticTone tone = SemanticTone::Ally;
};

struct ProjectileVisual {
    ImVec2 center{};
    float rangeRadius = 0.0f;
    std::vector<ImVec2> trajectory;
    std::string label;
    float distanceMeters = 0.0f;
    SemanticTone tone = SemanticTone::Caution;
    ImU32 colorOverride = 0;
};

struct CrosshairVisual {
    ImVec2 center{};
    float gap = 5.0f;
    float armLength = 9.0f;
    float thickness = 1.6f;
    bool centerDot = true;
    SemanticTone tone = SemanticTone::Accent;
};

struct AimGuide {
    ImVec2 center{};
    ImVec2 target{};
    float radius = 0.0f;
    bool drawCircle = false;
    bool drawTargetRay = false;
    bool targetValid = false;
    bool locked = false;
    int selectedBone = -1;
};

struct TouchRegionVisual {
    ImVec2 center{};
    float halfExtent = 0.0f;
};

struct RadarBlip {
    ImVec2 normalizedPosition{};
    float headingRadians = 0.0f;
    bool headingValid = false;
    std::string label;
    RadarBlipKind kind = RadarBlipKind::Player;
    SemanticTone tone = SemanticTone::Danger;
};

struct RadarVisual {
    ImVec2 center{};
    float radius = 0.0f;
    float maxDistanceMeters = 0.0f;
    float viewHeadingRadians = 0.0f;
    bool showSelf = false;
    std::vector<RadarBlip> blips;
};

struct HudMapMarker {
    ImVec2 position{};
    ImVec2 directionEnd{};
    std::string label;
    RadarBlipKind kind = RadarBlipKind::Player;
    SemanticTone tone = SemanticTone::Danger;
    bool drawDirection = false;
};

struct HudMapVisual {
    float markerSize = 6.0f;
    float fontSize = 15.0f;
    std::vector<HudMapMarker> markers;
};

struct WorldLabel {
    ImVec2 anchor{};
    std::string title;
    std::string detail;
    WorldLabelKind kind = WorldLabelKind::Item;
    SemanticTone tone = SemanticTone::Accent;
    bool emphasized = false;
    ImU32 colorOverride = 0;
    float titleSizeOverride = 0.0f;
};

struct HighValueEntry {
    std::string name;
    int value = 0;
    float distanceMeters = 0.0f;
    SemanticTone tone = SemanticTone::Caution;
};

struct HighValueList {
    ImVec2 origin{};
    float width = 300.0f;
    int maxRows = 8;
    std::string title;
    std::vector<HighValueEntry> entries;
};

}  // namespace lengjing
