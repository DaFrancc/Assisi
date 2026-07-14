/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/Reflect/AssetTypeRegistry.hpp>

#include <utility>

namespace Assisi::Core::Reflect
{

AssetTypeRegistry &AssetTypeRegistry::Instance()
{
    static AssetTypeRegistry instance;
    return instance;
}

void AssetTypeRegistry::Register(AssetTypeMeta meta)
{
    _metas.push_back(std::move(meta));
}

const AssetTypeMeta *AssetTypeRegistry::Find(std::string_view name) const
{
    for (const AssetTypeMeta &meta : _metas)
    {
        if (meta.name == name)
        {
            return &meta;
        }
    }
    return nullptr;
}

const AssetTypeMeta *AssetTypeRegistry::Find(std::type_index type) const
{
    for (const AssetTypeMeta &meta : _metas)
    {
        if (meta.typeIndex == type)
        {
            return &meta;
        }
    }
    return nullptr;
}

std::span<const AssetTypeMeta> AssetTypeRegistry::All() const
{
    return _metas;
}

std::size_t AssetTypeRegistry::Count() const
{
    return _metas.size();
}

} // namespace Assisi::Core::Reflect
