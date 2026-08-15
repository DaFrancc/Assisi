/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/LooseFileProvider.hpp>

#include <Assisi/Core/AssetDatabase.hpp>
#include <Assisi/Core/AssetSystem.hpp>

namespace Assisi::Core
{

LooseFileProvider::LooseFileProvider(const AssetDatabase &database) noexcept : _database(&database) {}

std::expected<std::vector<std::byte>, AssetError> LooseFileProvider::Open(AssetId id) const
{
    // Reserved built-ins are primitives resolved above the provider, not byte
    // payloads on disk — this backend does not serve them.
    if (id.IsReserved())
    {
        return std::unexpected(AssetError::UnknownAssetId);
    }

    const std::optional<std::string> path = _database->PathFor(id);
    if (!path.has_value())
    {
        return std::unexpected(AssetError::UnknownAssetId);
    }

    return AssetSystem::ReadBinary(*path);
}

} // namespace Assisi::Core
