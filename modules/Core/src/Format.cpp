/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Format.hpp>

namespace Assisi::Core
{

std::string VFormat(std::string_view format, std::format_args args)
{
    return std::vformat(format, args);
}

} // namespace Assisi::Core
