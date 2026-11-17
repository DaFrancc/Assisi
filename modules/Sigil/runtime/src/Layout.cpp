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

std::expected<uint32_t, ParamMiss> FindParam(const Layout &layout, std::string_view name, SlotType type)
{
    const std::optional<uint32_t> slot = FindSlot(layout, name);
    if (!slot.has_value())
    {
        return std::unexpected(ParamMiss::Missing);
    }
    if (layout.slots[*slot].kind != SlotKind::Param)
    {
        return std::unexpected(ParamMiss::NotParam);
    }
    if (layout.slots[*slot].type != type)
    {
        return std::unexpected(ParamMiss::OtherType);
    }
    return *slot;
}

} // namespace Assisi::Sigil
