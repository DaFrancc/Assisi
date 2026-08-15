/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Reflect/ComponentMaskJson.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/Reflect/ComponentRegistry.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace Assisi::Core::Reflect
{

nlohmann::json SerializeComponentMask(const ComponentMask &mask)
{
    nlohmann::json array = nlohmann::json::array();

    // Ascending by ordinal, which is ascending by component name — so an
    // unchanged mask always writes the same bytes. A save that reordered this
    // would show phantom diffs and dirty scenes nobody edited.
    const std::span<const ComponentMeta *const> replicable = ComponentRegistry::Instance().ReplicableComponents();
    for (std::size_t ordinal = 0; ordinal < replicable.size(); ++ordinal)
    {
        if (mask.Test(ordinal))
            array.push_back(replicable[ordinal]->name);
    }
    return array;
}

ComponentMask DeserializeComponentMask(const nlohmann::json &value)
{
    ComponentMask mask;
    if (value.is_null())
        return mask; // absent field: nothing excluded, the default

    if (!value.is_array())
    {
        Core::Log::Warn("ComponentMask: expected an array of component names, got '{}'. Reading it as no "
                        "exclusions.",
                        value.type_name());
        return mask;
    }

    const ComponentRegistry &registry = ComponentRegistry::Instance();
    for (const nlohmann::json &element : value)
    {
        if (!element.is_string())
        {
            Core::Log::Warn("ComponentMask: exclusion entries must be component names; skipping a '{}'.",
                            element.type_name());
            continue;
        }

        const std::string name = element.get<std::string>();
        const ComponentMeta *meta = registry.Find(name);
        // Two distinct failures, deliberately distinguished: a name nobody
        // registered is probably a typo or a renamed type, while a name that
        // resolves but is not replicable is a stale exclusion left behind when a
        // component lost its capability. Neither has a bit to live in, so both
        // are dropped — but the reader deserves to know which happened.
        if (meta == nullptr)
        {
            Core::Log::Warn("ComponentMask: no component named '{}' is registered — dropping the exclusion. "
                            "Was the type renamed?",
                            name);
            continue;
        }
        const std::size_t ordinal = registry.ReplicableOrdinalOf(meta->id);
        if (ordinal == ComponentRegistry::kInvalidOrdinal)
        {
            Core::Log::Warn("ComponentMask: '{}' is not ACOMP(replicable), so excluding it means nothing — "
                            "dropping it. Nothing about this component crosses the wire anyway.",
                            name);
            continue;
        }

        mask.Set(ordinal);
    }
    return mask;
}

} // namespace Assisi::Core::Reflect
