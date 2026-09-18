/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "LaunchArgs.hpp"

#include <charconv>
#include <cstddef>
#include <system_error>

namespace Game
{

bool ParsePositive(std::string_view text, std::int32_t &out)
{
    std::int32_t value = 0;
    const auto parsed  = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value <= 0)
    {
        return false;
    }
    out = value;
    return true;
}

bool ParseResolution(std::string_view text, std::int32_t &width, std::int32_t &height)
{
    const std::size_t separator = text.find('x');
    if (separator == std::string_view::npos)
    {
        return false;
    }

    const std::string_view left  = text.substr(0, separator);
    const std::string_view right = text.substr(separator + 1);

    std::int32_t parsedWidth  = 0;
    std::int32_t parsedHeight = 0;
    const auto first  = std::from_chars(left.data(), left.data() + left.size(), parsedWidth);
    const auto second = std::from_chars(right.data(), right.data() + right.size(), parsedHeight);
    if (first.ec != std::errc{} || first.ptr != left.data() + left.size() || second.ec != std::errc{} ||
        second.ptr != right.data() + right.size() || parsedWidth <= 0 || parsedHeight <= 0)
    {
        return false;
    }

    width  = parsedWidth;
    height = parsedHeight;
    return true;
}

bool ParseCameraPose(std::string_view text, std::array<float, 3> &eye, std::array<float, 3> &target)
{
    std::array<float, 6> values{};
    const char *cursor    = text.data();
    const char *const end = text.data() + text.size();
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        const auto parsed = std::from_chars(cursor, end, values[i]);
        if (parsed.ec != std::errc{})
        {
            return false;
        }
        cursor = parsed.ptr;
        const bool last = i + 1 == values.size();
        if (last ? cursor != end : (cursor == end || *cursor != ','))
        {
            return false;
        }
        if (!last)
        {
            ++cursor;
        }
    }
    eye    = {values[0], values[1], values[2]};
    target = {values[3], values[4], values[5]};
    return true;
}

bool ParseAddress(std::string_view text, std::string &outAddress, std::uint16_t &outPort)
{
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos)
    {
        outAddress = std::string(text);
        return !outAddress.empty();
    }

    const std::string_view host = text.substr(0, colon);
    const std::string_view port = text.substr(colon + 1);
    if (!host.empty())
    {
        outAddress = std::string(host);
    }

    // Widened past the port range on purpose: parsing into a uint16_t would wrap
    // 70000 to something valid-looking instead of refusing it.
    std::uint32_t parsedPort = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), parsedPort);
    constexpr std::uint32_t kMaxPort = 65535u;
    if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || parsedPort == 0 ||
        parsedPort > kMaxPort)
    {
        return false;
    }
    outPort = static_cast<std::uint16_t>(parsedPort);
    return true;
}

} // namespace Game
