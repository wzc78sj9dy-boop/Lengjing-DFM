#pragma once

#include <algorithm>
#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <utility>

namespace lengjing::auth::input {

enum class CardInputStatus {
    Accepted,
    EndOfInput,
    Empty,
    Invalid,
};

struct CardInputResult {
    CardInputStatus status = CardInputStatus::EndOfInput;
    std::string value;

    explicit operator bool() const noexcept {
        return status == CardInputStatus::Accepted;
    }
};

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
        value.begin(), value.end(), [&](char character) {
            return isWhitespace(static_cast<unsigned char>(character));
        });
    value.erase(value.begin(), first);
}

inline CardInputResult ReadCardKeyFromStream(
    std::istream& input,
    std::ostream& output,
    bool inputIsTerminal) {
    if (inputIsTerminal) {
        output << "请输入卡密: " << std::flush;
    }

    std::string value;
    if (!std::getline(input, value)) {
        return {CardInputStatus::EndOfInput, {}};
    }
    TrimCardKey(value);
    if (value.empty()) {
        return {CardInputStatus::Empty, {}};
    }
    constexpr std::size_t kMaximumCardKeyLength = 256;
    const bool valid = value.size() <= kMaximumCardKeyLength &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x21U && byte <= 0x7eU;
        });
    if (!valid) {
        return {CardInputStatus::Invalid, {}};
    }
    return {CardInputStatus::Accepted, std::move(value)};
}

}  // namespace lengjing::auth::input
