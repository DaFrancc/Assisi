/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Runtime/SkyResolve.hpp>

#include <Assisi/Runtime/LightComponents.hpp>
#include <Assisi/Runtime/SkyComponents.hpp>

namespace Assisi::Runtime
{

SkyResolution ResolveSky(ECS::Scene &scene)
{
    ECS::Entity sunEntity = ECS::NullEntity;
    const DirectionalLight *sunLight = nullptr;
    std::uint32_t directionalCount = 0;

    for (auto [entity, light] : scene.Query<DirectionalLight>())
    {
        ++directionalCount;
        if (directionalCount > 1)
        {
            // Kept walking rather than broken out of, because the count is the
            // answer and stopping at two would only save a handful of entities.
            continue;
        }
        sunEntity = entity;
        sunLight = &light;
    }

    if (directionalCount == 0)
    {
        return SkyResolution{.status = SkyStatus::NoDirectionalLight, .sun = {}, .settings = {}};
    }
    if (directionalCount > 1)
    {
        return SkyResolution{.status = SkyStatus::MultipleDirectionalLights, .sun = {}, .settings = {}};
    }

    const Skybox *skybox = scene.Get<Skybox>(sunEntity);
    if (skybox == nullptr)
    {
        return SkyResolution{.status = SkyStatus::NoSkybox, .sun = {}, .settings = {}};
    }

    return SkyResolution{.status = SkyStatus::Ready,
                         // Reversed: the component stores where the light goes,
                         // and a sky is described by where the sun is.
                         // Under SunColorSource::Sky the authored colour is not
                         // read anywhere — the inspector greys it out, and a
                         // greyed field that still tinted the sky would make that
                         // grey a lie. The sun above the air is white, and the
                         // atmosphere does the rest.
                         .sun = Render::SkySun{.directionToSun = Render::SafeSkyDirection(-sunLight->direction),
                                               .color = sunLight->colorSource == SunColorSource::Sky
                                                            ? glm::vec3(1.f)
                                                            : sunLight->color,
                                               .intensity = sunLight->intensity},
                         .settings = ToSkySettings(*skybox)};
}

} // namespace Assisi::Runtime
