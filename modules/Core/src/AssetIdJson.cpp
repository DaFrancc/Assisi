/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetIdJson.hpp>

#include <utility>

#include <nlohmann/json.hpp>

namespace Assisi::Core
{
namespace
{
/// Editor-installed at startup; empty in a shipped build (which never saves).
AssetIdHintResolver gHintResolver;
} // namespace

void SetAssetIdHintResolver(AssetIdHintResolver resolver)
{
    gHintResolver = std::move(resolver);
}

nlohmann::json SerializeAssetId(const AssetId &id)
{
    nlohmann::json object;
    object["guid"] = id.ToString();
    if (gHintResolver)
    {
        std::string hint = gHintResolver(id);
        if (!hint.empty())
        {
            object["path"] = std::move(hint);
        }
    }
    return object;
}

AssetId DeserializeAssetId(const nlohmann::json &value)
{
    // Object form { "guid", "path"? } — read the guid, ignore the hint. A bare
    // guid string is tolerated for hand-authored / hint-less references.
    std::string guidText;
    if (value.is_object())
    {
        guidText = value.value("guid", std::string{});
    }
    else if (value.is_string())
    {
        guidText = value.get<std::string>();
    }
    return AssetId::Parse(guidText).value_or(AssetId{});
}

} // namespace Assisi::Core
