#pragma once

#include "ui/UiModel.h"

#include <array>
#include <cstddef>

namespace lengjing::game {

struct FeatureSettings {
    ui::VisualSettings visual{};
    ui::LootSettings loot{};
    ui::RadarSettings radar{};
    ui::AimSettings aim{};
};

}  // namespace lengjing::game
