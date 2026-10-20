#pragma once
#include <charconv>
#include <cmath>
#include <string_view>

namespace xg {
inline bool ParseTuning(std::string_view text, float& shift, float& convergence)
{
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    auto trim = [&]() { while (!text.empty() && space(text.front())) text.remove_prefix(1); };
    auto number = [&](float& value) {
        trim();
        if (text.empty()) return false;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || result.ptr == text.data()) return false;
        text.remove_prefix(size_t(result.ptr - text.data()));
        return true;
    };
    float s = 0, c = 0;
    if (!number(s) || text.empty() || !space(text.front()) || !number(c)) return false;
    trim();
    if (!text.empty() || !std::isfinite(s) || !std::isfinite(c) || s < 0 || s > 64 || c < 0 || c > .25f || (s == 0 && c != 0)) return false;
    shift = s; convergence = c; return true;
}
} // namespace xg
