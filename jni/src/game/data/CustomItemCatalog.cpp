#include "game/data/CustomItemCatalog.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace lengjing::game::data {
namespace {

std::string Trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](char character) {
        return character == ' ' || character == '\t' || character == '\r';
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char character) {
        return character == ' ' || character == '\t' || character == '\r';
    }).base();
    return first < last ? std::string(first, last) : std::string{};
}

ImU32 ParseColor(std::string value) {
    value = Trim(std::move(value));
    if (value == "红色") return IM_COL32(244, 82, 82, 255);
    if (value == "绿色") return IM_COL32(74, 222, 128, 255);
    if (value == "黄色") return IM_COL32(250, 204, 21, 255);
    if (value == "青色") return IM_COL32(34, 211, 238, 255);
    if (value == "白色") return IM_COL32(242, 247, 245, 255);
    if (value == "品红") return IM_COL32(232, 121, 249, 255);
    if (value == "橙色") return IM_COL32(251, 146, 60, 255);
    if (value == "蓝色") return IM_COL32(96, 165, 250, 255);
    if (value == "紫色") return IM_COL32(192, 132, 252, 255);

    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    if (value.size() != 6 && value.size() != 8) {
        return IM_COL32(246, 183, 74, 255);
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long encoded = std::strtoul(value.c_str(), &end, 16);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return IM_COL32(246, 183, 74, 255);
    }
    const unsigned int red = value.size() == 8
        ? static_cast<unsigned int>((encoded >> 16U) & 0xffU)
        : static_cast<unsigned int>((encoded >> 16U) & 0xffU);
    const unsigned int green = static_cast<unsigned int>((encoded >> 8U) & 0xffU);
    const unsigned int blue = static_cast<unsigned int>(encoded & 0xffU);
    const unsigned int alpha = value.size() == 8
        ? static_cast<unsigned int>((encoded >> 24U) & 0xffU)
        : 255U;
    return IM_COL32(red, green, blue, alpha);
}

float ParseFontSize(const std::string& value) {
    if (value.empty()) return 0.0f;
    char* end = nullptr;
    errno = 0;
    const float size = std::strtof(value.c_str(), &end);
    return errno == 0 && end != nullptr && *end == '\0'
        ? std::clamp(size, 0.0f, 48.0f)
        : 0.0f;
}

}  // namespace

bool CustomItemCatalog::Load(const std::string& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        entries_.clear();
        if (error != nullptr && errno != ENOENT) {
            *error = std::string("无法读取自定义物资: ") + std::strerror(errno);
        }
        return errno == ENOENT;
    }

    std::vector<CustomItemEntry> loaded;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line.front() == '#') continue;

        std::vector<std::string> fields;
        std::istringstream stream(line);
        std::string field;
        while (std::getline(stream, field, '/')) {
            fields.push_back(Trim(std::move(field)));
        }
        if (fields.size() < 2 || fields[0].empty() || fields[1].empty()) continue;
        loaded.push_back(CustomItemEntry{
            std::move(fields[0]),
            std::move(fields[1]),
            fields.size() >= 3 ? ParseColor(fields[2]) : IM_COL32(246, 183, 74, 255),
            fields.size() >= 4 ? ParseFontSize(fields[3]) : 0.0f,
        });
    }
    entries_ = std::move(loaded);
    return true;
}

std::optional<CustomItemEntry> CustomItemCatalog::Match(
    std::string_view className) const {
    for (const CustomItemEntry& entry : entries_) {
        if (className.find(entry.classPattern) != std::string_view::npos) {
            return entry;
        }
    }
    return std::nullopt;
}

std::size_t CustomItemCatalog::Size() const noexcept {
    return entries_.size();
}

void CustomItemCatalog::Clear() {
    entries_.clear();
}

}  // namespace lengjing::game::data
