/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Clipboard.hpp
/// @brief The system clipboard as the UI sees it: two functions the host
/// supplies, so the core never touches the window and a test can hand in a fake.

#include <functional>
#include <string>
#include <string_view>

namespace Assisi::Mondrian
{

/// @brief Reads and writes the clipboard's text, UTF-8. Either may be empty,
/// which reads as no text and drops writes.
struct Clipboard
{
    std::function<std::string()> read;
    std::function<void(std::string_view text)> write;

    [[nodiscard]] std::string Read() const { return read ? read() : std::string{}; }

    void Write(std::string_view text) const
    {
        if (write)
        {
            write(text);
        }
    }
};

} // namespace Assisi::Mondrian
