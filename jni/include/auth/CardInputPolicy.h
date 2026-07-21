#pragma once

#include <algorithm>
#include <cstddef>
#include <istream>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace lengjing::auth::input {

enum class CardInputStatus {
    Accepted,
    EndOfInput,
    Empty,
    Invalid,
    ReuseUnavailable,
    TerminalError,
};

struct CardInputResult {
    CardInputStatus status = CardInputStatus::EndOfInput;
    std::string value;

    explicit operator bool() const noexcept {
        return status == CardInputStatus::Accepted;
    }
};

struct TerminalEchoControl {
    using Callback = bool (*)(void*) noexcept;

    void* context = nullptr;
    Callback disable = nullptr;
    Callback restore = nullptr;

    constexpr bool IsConfigured() const noexcept {
        return disable != nullptr && restore != nullptr;
    }
};

class TerminalEchoScope final {
public:
    TerminalEchoScope(const TerminalEchoControl& control,
                      bool requested) noexcept
        : control_(control), attempted_(requested && control.IsConfigured()) {
        if (attempted_) restorePending_ = control_.disable(control_.context);
    }

    ~TerminalEchoScope() {
        if (restorePending_) control_.restore(control_.context);
    }

    TerminalEchoScope(const TerminalEchoScope&) = delete;
    TerminalEchoScope& operator=(const TerminalEchoScope&) = delete;

    bool Attempted() const noexcept { return attempted_; }
    bool Disabled() const noexcept { return restorePending_; }

    bool Restore() noexcept {
        if (!restorePending_) return true;
        if (!control_.restore(control_.context)) return false;
        restorePending_ = false;
        return true;
    }

private:
    TerminalEchoControl control_{};
    bool attempted_ = false;
    bool restorePending_ = false;
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

inline bool IsValidCardKey(std::string_view value) noexcept {
    constexpr std::size_t kMaximumCardKeyLength = 256;
    return !value.empty() && value.size() <= kMaximumCardKeyLength &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return byte >= 0x21U && byte <= 0x7eU;
        });
}

inline CardInputResult ReadCardKeyFromStream(
    std::istream& input,
    std::ostream& output,
    bool inputIsTerminal,
    const TerminalEchoControl& terminalEcho = {},
    std::string_view reusableCardKey = {}) {
    const bool reuseAvailable = IsValidCardKey(reusableCardKey);
    if (inputIsTerminal) {
        output << (reuseAvailable
                ? "请输入卡密，输入 y 复用上次卡密: "
                : "请输入卡密: ")
               << std::flush;
    }

    std::string value;
    bool readSucceeded = false;
    bool echoWasDisabled = false;
    bool restoreSucceeded = true;
    {
        TerminalEchoScope echoScope(terminalEcho, inputIsTerminal);
        if (echoScope.Attempted() && !echoScope.Disabled()) {
            return {CardInputStatus::TerminalError, {}};
        }
        echoWasDisabled = echoScope.Disabled();
        readSucceeded = static_cast<bool>(std::getline(input, value));
        restoreSucceeded = echoScope.Restore();
    }
    if (echoWasDisabled) output << '\n' << std::flush;
    if (!restoreSucceeded) {
        std::fill(value.begin(), value.end(), '\0');
        value.clear();
        return {CardInputStatus::TerminalError, {}};
    }
    if (!readSucceeded) {
        return {CardInputStatus::EndOfInput, {}};
    }
    TrimCardKey(value);
    if (value.empty()) {
        return {CardInputStatus::Empty, {}};
    }
    if (value == "y" || value == "Y") {
        if (!reuseAvailable) {
            return {CardInputStatus::ReuseUnavailable, {}};
        }
        return {
            CardInputStatus::Accepted,
            std::string(reusableCardKey)};
    }
    if (!IsValidCardKey(value)) {
        return {CardInputStatus::Invalid, {}};
    }
    return {CardInputStatus::Accepted, std::move(value)};
}

}  // namespace lengjing::auth::input
