#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

namespace Assisi::Core::Reflect
{

ComponentRegistry &ComponentRegistry::Instance()
{
    static ComponentRegistry instance;
    return instance;
}

void ComponentRegistry::Register(ComponentMeta meta)
{
    _metas.push_back(std::move(meta));
}

const ComponentMeta *ComponentRegistry::Find(std::string_view name) const
{
    for (const auto &m : _metas)
        if (m.name == name)
            return &m;
    return nullptr;
}

std::span<const ComponentMeta> ComponentRegistry::All() const
{
    return _metas;
}

} // namespace Assisi::Core::Reflect