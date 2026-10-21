/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file StrongIdFormat.hpp
/// @brief std::format writes a strong id as its value.
///
/// Its own header so the ids themselves stay off <format>: an id is included
/// almost everywhere and formatted almost nowhere. Logger.hpp includes this, so
/// an id given to a log line formats with nothing more.

#include <format>

#include <Assisi/Core/StrongId.hpp>

template <Assisi::Core::StrongId T> struct std::formatter<T> : std::formatter<decltype(T::value)>
{
    auto format(T id, std::format_context &ctx) const
    {
        return std::formatter<decltype(T::value)>::format(id.value, ctx);
    }
};
