/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetKind.hpp>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>

namespace Assisi::Core
{

AssetKindRegistry &AssetKindRegistry::Instance()
{
    static AssetKindRegistry instance;
    return instance;
}

bool AssetKindRegistry::Register(AssetKind kind)
{
    if (!BuiltInKindName(kind.id).empty())
    {
        Log::Error("AssetKind: '{}' is a kind the engine cooks itself; the registration is ignored.", kind.name);
        return false;
    }
    for (const AssetKind &existing : _kinds)
    {
        if (existing.id == kind.id)
        {
            Log::Error("AssetKind: '{}' is registered twice; the second registration is ignored.", kind.name);
            return false;
        }
        for (const std::string &extension : kind.extensions)
        {
            if (std::ranges::find(existing.extensions, extension) != existing.extensions.end())
            {
                Log::Error("AssetKind: '{}' claims '{}', which '{}' already has; the registration is ignored.",
                           kind.name, extension, existing.name);
                return false;
            }
        }
    }
    _kinds.push_back(std::move(kind));
    return true;
}

bool AssetKindRegistry::RegisterCookStep(AssetCookStep step)
{
    if (CookStepFor(step.kind) != nullptr)
    {
        Log::Error("AssetKind: {} has two cook steps; the second is ignored.", DescribeKind(step.kind));
        return false;
    }
    _cookSteps.push_back(std::move(step));
    return true;
}

const AssetKind *AssetKindRegistry::Find(AssetKindId id) const
{
    for (const AssetKind &kind : _kinds)
    {
        if (kind.id == id)
        {
            return &kind;
        }
    }
    return nullptr;
}

const AssetKind *AssetKindRegistry::ForPath(std::string_view vpath) const
{
    for (const AssetKind &kind : _kinds)
    {
        for (const std::string &extension : kind.extensions)
        {
            if (vpath.size() > extension.size() && vpath.ends_with(extension))
            {
                return &kind;
            }
        }
    }
    return nullptr;
}

const AssetCookStep *AssetKindRegistry::CookStepFor(AssetKindId id) const
{
    for (const AssetCookStep &step : _cookSteps)
    {
        if (step.kind == id)
        {
            return &step;
        }
    }
    return nullptr;
}

std::expected<std::vector<std::byte>, std::string> CookAssetBytes(const AssetKind &kind,
                                                                  std::span<const std::byte> source)
{
    BitWriter writer;
    WriteCookedHeader(writer, kind.id);

    const AssetCookStep *step = AssetKindRegistry::Instance().CookStepFor(kind.id);
    if (step == nullptr)
    {
        writer.WriteBytes(source);
    }
    else
    {
        const std::expected<std::vector<std::byte>, std::string> cooked = step->cook(source);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        writer.WriteBytes(*cooked);
    }

    const std::span<const std::byte> bytes = writer.Data();
    return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

} // namespace Assisi::Core
