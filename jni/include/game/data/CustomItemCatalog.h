#pragma once

#include "ImGui/imgui.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lengjing::game::data {

struct CustomItemEntry {
    std::string classPattern;
    std::string displayName;
    ImU32 color = IM_COL32(246, 183, 74, 255);
    float fontSize = 0.0f;
};

class CustomItemCatalog final {
public:
    bool Load(const std::string& path, std::string* error = nullptr);
    std::optional<CustomItemEntry> Match(std::string_view className) const;
    std::size_t Size() const noexcept;
    void Clear();

private:
    std::vector<CustomItemEntry> entries_;
};

}  // namespace lengjing::game::data
