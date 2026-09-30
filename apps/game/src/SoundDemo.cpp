/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "SoundDemo.hpp"

#include <Assisi/App/World.hpp>
#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/SoundOutput.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>

#include <expected>
#include <memory>

namespace Game
{

void SoundDemoSystem(Assisi::App::SystemContext &ctx)
{
    if (ctx.assets == nullptr || ctx.mixer == nullptr)
    {
        return;
    }

    for (auto [entity, demo] : ctx.world.scene.Query<SoundDemo>())
    {
        (void)entity;
        if (demo.played)
        {
            continue;
        }

        // Null until the load lands, and for good if the sound is missing.
        const std::shared_ptr<const Assisi::Audio::PcmClip> clip =
            ctx.assets->Resolve<Assisi::Audio::PcmClip>(demo.clip);
        if (clip == nullptr)
        {
            continue;
        }

        demo.played = true;
        const std::expected<Assisi::Audio::SoundHandle, Assisi::Audio::AudioError> sound =
            ctx.mixer->Attach(clip, Assisi::Audio::ToBusId(Assisi::Audio::DefaultBus::Sfx));
        if (!sound)
        {
            Assisi::Core::Log::Warn("SoundDemo: cannot play a sound ({}).", Assisi::Audio::ToString(sound.error()));
        }
    }
}

} // namespace Game
