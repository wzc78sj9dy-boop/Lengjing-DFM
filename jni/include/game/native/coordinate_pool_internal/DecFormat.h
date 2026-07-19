#pragma once

#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace lengjing::game::native::coordinate_pool_internal {
namespace coordinate_pool_format {
namespace detail {

inline std::string ExtractTypeSpec(const std::string& spec) {
    std::string out;
    for (char c : spec) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            out.push_back(c);
        }
    }
    return out;
}

template <typename T>
std::string ToStringOne(const T& value, const std::string& spec) {
    std::ostringstream os;
    const std::string type = ExtractTypeSpec(spec);

    if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
        if (type == "x" || type == "X") {
            if (type == "X") {
                os << std::uppercase;
            }
            os << std::hex << static_cast<unsigned long long>(value);
        } else {
            os << value;
        }
    } else if constexpr (std::is_pointer_v<T>) {
        if (type == "x" || type == "X") {
            if (type == "X") {
                os << std::uppercase;
            }
            os << std::hex << reinterpret_cast<uintptr_t>(value);
        } else {
            os << value;
        }
    } else {
        os << value;
    }

    return os.str();
}

inline void CollectFormatters(std::vector<std::function<std::string(const std::string&)>>&) {
}

template <typename T, typename... Rest>
void CollectFormatters(std::vector<std::function<std::string(const std::string&)>>& formatters,
                       T&& value,
                       Rest&&... rest) {
    using U = std::decay_t<T>;
    formatters.emplace_back([v = U(std::forward<T>(value))](const std::string& spec) {
        return ToStringOne(v, spec);
    });
    CollectFormatters(formatters, std::forward<Rest>(rest)...);
}

} // namespace detail

template <typename... Args>
std::string Format(const std::string& fmt, Args&&... args) {
    std::vector<std::function<std::string(const std::string&)>> formatters;
    formatters.reserve(sizeof...(Args));
    detail::CollectFormatters(formatters, std::forward<Args>(args)...);

    std::string out;
    out.reserve(fmt.size() + 32);
    std::size_t arg_index = 0;

    for (std::size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '{' && i + 1 < fmt.size()) {
            if (fmt[i + 1] == '{') {
                out.push_back('{');
                ++i;
                continue;
            }

            const std::size_t end = fmt.find('}', i + 1);
            if (end != std::string::npos) {
                const std::string inside = fmt.substr(i + 1, end - i - 1);
                std::string spec;
                const std::size_t colon = inside.find(':');
                if (colon != std::string::npos) {
                    spec = inside.substr(colon + 1);
                }
                if (arg_index < formatters.size()) {
                    out += formatters[arg_index++](spec);
                }
                i = end;
                continue;
            }
        }

        if (fmt[i] == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
            out.push_back('}');
            ++i;
            continue;
        }

        out.push_back(fmt[i]);
    }

    return out;
}

}  // namespace coordinate_pool_format
}  // namespace lengjing::game::native::coordinate_pool_internal
