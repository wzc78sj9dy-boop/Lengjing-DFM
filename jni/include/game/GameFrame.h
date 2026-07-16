#pragma once

#include "render/render_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lengjing::game {

struct GameFrame {
    std::uint64_t sequence = 0;
    bool ready = false;
    int playerCount = 0;
    int botCount = 0;
    int nearbyEnemyCount = 0;
    bool dangerousObjectNearby = false;
    bool geometryAvailable = false;
    std::size_t geometryMeshCount = 0;
    std::size_t geometryTriangleCount = 0;
    std::uint64_t geometryGeneration = 0;
    std::vector<PlayerVisual> players;
    std::vector<PlayerSignalVisual> playerSignals;
    std::optional<GeometryModelVisual> modelGeometry;
    std::vector<std::string> aimWarningPlayers;
    std::vector<OffscreenMarker> offscreenMarkers;
    std::vector<ProjectileVisual> projectiles;
    std::vector<WorldLabel> worldLabels;
    std::optional<CrosshairVisual> crosshair;
    std::optional<AimGuide> aimGuide;
    std::optional<TouchRegionVisual> touchRegion;
    std::optional<RadarVisual> radar;
    std::optional<HudMapVisual> hudMap;
    std::optional<HighValueList> highValueList;
};

}  // namespace lengjing::game
