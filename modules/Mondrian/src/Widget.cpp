/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Widget.hpp>

#include <utility>

namespace Assisi::Mondrian
{

uint32_t WidgetRegistry::Register(WidgetType type)
{
    _types.push_back(type);
    // Ids start at one, so a node that names no control — the zero every node
    // starts with — can never reach a type by accident.
    return static_cast<uint32_t>(_types.size());
}

const WidgetType *WidgetRegistry::Get(uint32_t behaviour) const
{
    if (behaviour == 0 || behaviour > _types.size())
    {
        return nullptr;
    }
    return &_types[behaviour - 1];
}

} // namespace Assisi::Mondrian
