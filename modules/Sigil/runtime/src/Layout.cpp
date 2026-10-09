/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Layout.hpp>

namespace Assisi::Sigil
{

std::optional<uint32_t> FindSlot(const Layout &layout, std::string_view name)
{
    for (std::size_t slot = 0; slot < layout.slots.size(); ++slot)
    {
        if (layout.slots[slot].name == name)
        {
            return static_cast<uint32_t>(slot);
        }
    }
    return std::nullopt;
}

} // namespace Assisi::Sigil
