#pragma once

#include <algorithm>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>

namespace lengjing::auth::input {

inline void TrimCardKey(std::string& value) {
    const auto isWhitespace = [](unsigned char character) {
        return character == ' ' || character == '\t' ||
            character == '\r' || character == '\n';
    };
    while (!value.empty() &&
           isWhitespace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    const auto first = std::find_if_not(
        value.begin(), value.end(),
        [&](char character) {
            return isWhitespace(static_cast<unsigned char>(character));
        });
    value.erase(value.begin(), first);
}

inline std::string ReadCardKeyFromStream(
    std::istream& input,
    std::ostream& output,
    bool inputIsTerminal,
    std::string_view cachedCardKey) {
    if (!inputIsTerminal) {
        std::string value;
        if (!std::getline(input, value)) {
            return {};
        }
        TrimCardKey(value);
        return value;
    }

    while (true) {
        output << "请输入卡密";
        if (!cachedCardKey.empty()) {
            output << "(输入y使用上次登录卡密)";
        }
        output << ": " << std::flush;

        std::string value;
        if (!std::getline(input, value)) {
            return {};
        }
        TrimCardKey(value);
        if (value.empty()) {
            output << "卡密不能为空\n";
            continue;
        }
        if (!cachedCardKey.empty() && (value == "y" || value == "Y")) {
            return std::string(cachedCardKey);
        }
        return value;
    }
}

}  // namespace lengjing::auth::input
