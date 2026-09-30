/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/App/AudioSystems.hpp>

#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string_view>
#include <utility>

namespace Assisi::App
{

namespace
{

/// The fewest sounds an emitter plays at once, whatever its field says: an
/// emitter that could play none would only ever drop or queue.
constexpr std::uint32_t kMinConcurrentSounds = 1;

std::uint32_t Capacity(const Runtime::AudioEmitter &emitter)
{
    return std::max(emitter.maxConcurrentSounds, kMinConcurrentSounds);
}

bool IsPaused(const Runtime::AudioEmitter &emitter)
{
    return emitter.pausedByRequest || emitter.pausedByWorld;
}

/// Logs why @p emitter cannot play, the first time only.
void WarnOnce(Runtime::AudioEmitter &emitter, std::string_view why)
{
    if (!emitter.warned)
    {
        emitter.warned = true;
        Core::Log::Warn("AudioEmitter: cannot play a sound on bus '{}' ({}).", emitter.bus.View(), why);
    }
}

/// Starts @p clip on @p emitter, set up as the emitter says. False when it could not.
bool StartSound(Runtime::AudioEmitter &emitter, std::shared_ptr<const Audio::PcmClip> clip, Audio::SoundOutput &output)
{
    const std::optional<Audio::BusId> bus = output.FindBus(emitter.bus.View());
    if (!bus)
    {
        WarnOnce(emitter, "the mixer has no bus by that name");
        return false;
    }
    const std::expected<Audio::SoundHandle, Audio::AudioError> claimed = output.Claim(std::move(clip), *bus);
    if (!claimed)
    {
        WarnOnce(emitter, Audio::ToString(claimed.error()));
        return false;
    }
    const Audio::SoundHandle sound = *claimed;
    output.SetVolume(sound, emitter.volume);
    output.SetLooping(sound, emitter.looping);
    if (IsPaused(emitter))
    {
        output.Pause(sound);
    }
    output.Play(sound);
    emitter.playing.push_back(sound);
    return true;
}

/// Waits for a slot, unless the queue is full.
void Enqueue(Runtime::AudioEmitter &emitter)
{
    if (emitter.queued < emitter.maxQueuedRequests)
    {
        ++emitter.queued;
    }
}

/// The target's emitter, or null, logged, when the event names no entity or one
/// with no emitter. @p event names the event for the log.
Runtime::AudioEmitter *EmitterOf(World &world, ECS::Entity target, std::string_view event)
{
    if (!target)
    {
        Core::Log::Warn("{}: names no entity, so no emitter answers it.", event);
        return nullptr;
    }
    Runtime::AudioEmitter *emitter =
        world.scene.IsAlive(target) ? world.scene.Get<Runtime::AudioEmitter>(target) : nullptr;
    if (emitter == nullptr)
    {
        Core::Log::Warn("{}: entity [{}:{}] has no AudioEmitter.", event, target.index, target.generation);
    }
    return emitter;
}

std::shared_ptr<const Audio::PcmClip> ClipOf(const Runtime::AudioEmitter &emitter, Core::AssetStore *assets)
{
    return assets != nullptr ? assets->Resolve<Audio::PcmClip>(emitter.clip) : nullptr;
}

} // namespace

bool IsPlaying(const Runtime::AudioEmitter &emitter)
{
    return !emitter.playing.empty();
}

std::optional<ECS::Entity> FindDefaultEmitter(World &world)
{
    std::optional<ECS::Entity> found;
    std::uint32_t count = 0;
    for (auto [entity, tag] : world.scene.Query<Runtime::DefaultEmitter>())
    {
        (void)tag;
        found = entity;
        ++count;
    }
    if (count == 0)
    {
        Core::Log::Warn("FindDefaultEmitter: no entity in world '{}' carries DefaultEmitter.", world.name);
        return std::nullopt;
    }
    if (count > 1)
    {
        Core::Log::Warn("FindDefaultEmitter: {} entities in world '{}' carry DefaultEmitter; one is expected.", count,
                        world.name);
        return std::nullopt;
    }
    if (!world.scene.Has<Runtime::AudioEmitter>(*found))
    {
        Core::Log::Warn("FindDefaultEmitter: the DefaultEmitter in world '{}' has no AudioEmitter to play from.",
                        world.name);
        return std::nullopt;
    }
    return found;
}

void RequestPlay(Runtime::AudioEmitter &emitter, std::shared_ptr<const Audio::PcmClip> clip, Audio::SoundOutput &output)
{
    if (clip == nullptr)
    {
        // Still loading: wait for it as for a slot.
        Enqueue(emitter);
        return;
    }
    if (emitter.playing.size() < Capacity(emitter))
    {
        (void)StartSound(emitter, std::move(clip), output);
        return;
    }

    switch (emitter.whenFull)
    {
    case Runtime::WhenFull::Queue:
        Enqueue(emitter);
        return;
    case Runtime::WhenFull::ReplaceOldest:
        output.Stop(emitter.playing.front());
        emitter.playing.erase(emitter.playing.begin());
        (void)StartSound(emitter, std::move(clip), output);
        return;
    case Runtime::WhenFull::ReplaceNewest:
        output.Stop(emitter.playing.back());
        emitter.playing.pop_back();
        (void)StartSound(emitter, std::move(clip), output);
        return;
    case Runtime::WhenFull::Drop:
    case Runtime::WhenFull::Count:
        return;
    }
}

void StopAll(Runtime::AudioEmitter &emitter, Audio::SoundOutput &output)
{
    for (const Audio::SoundHandle sound : emitter.playing)
    {
        output.Stop(sound);
    }
    emitter.playing.clear();
    emitter.queued = 0;
}

void ApplyPause(const Runtime::AudioEmitter &emitter, Audio::SoundOutput &output)
{
    const bool paused = IsPaused(emitter);
    for (const Audio::SoundHandle sound : emitter.playing)
    {
        if (paused)
        {
            output.Pause(sound);
        }
        else
        {
            output.Resume(sound);
        }
    }
}

void AdvanceEmitter(Runtime::AudioEmitter &emitter, std::shared_ptr<const Audio::PcmClip> clip,
                    Audio::SoundOutput &output)
{
    std::erase_if(emitter.playing, [&output](const Audio::SoundHandle sound) { return output.IsFinished(sound); });
    for (const Audio::SoundHandle sound : emitter.playing)
    {
        output.Hold(sound);
        // A volume changed in the inspector reaches what is already playing.
        output.SetVolume(sound, emitter.volume);
    }
    while (clip != nullptr && emitter.queued > 0 && emitter.playing.size() < Capacity(emitter))
    {
        --emitter.queued;
        if (!StartSound(emitter, clip, output))
        {
            // What kept this one from starting keeps the rest from starting too.
            emitter.queued = 0;
        }
    }
}

void AudioEmitterSystem(SystemContext &ctx)
{
    if (ctx.mixer == nullptr)
    {
        return;
    }
    Audio::SoundOutput &output = *ctx.mixer;
    ECS::Scene &scene = ctx.world.scene;

    // The world's pause first, so a sound started below starts paused if it should.
    for (auto [entity, emitter] : scene.Query<Runtime::AudioEmitter>())
    {
        const bool ignores = emitter.ignoresWorldPause || scene.Has<Runtime::DefaultEmitter>(entity);
        const bool pausedByWorld = ctx.world.paused && !ignores;
        if (pausedByWorld != emitter.pausedByWorld)
        {
            emitter.pausedByWorld = pausedByWorld;
            ApplyPause(emitter, output);
        }
    }

    for (const StopEmitter &event : ctx.events.Read<StopEmitter>())
    {
        if (Runtime::AudioEmitter *emitter = EmitterOf(ctx.world, event.target, "StopEmitter"))
        {
            StopAll(*emitter, output);
        }
    }
    for (const PauseEmitter &event : ctx.events.Read<PauseEmitter>())
    {
        if (Runtime::AudioEmitter *emitter = EmitterOf(ctx.world, event.target, "PauseEmitter"))
        {
            emitter->pausedByRequest = true;
            ApplyPause(*emitter, output);
        }
    }
    for (const ResumeEmitter &event : ctx.events.Read<ResumeEmitter>())
    {
        if (Runtime::AudioEmitter *emitter = EmitterOf(ctx.world, event.target, "ResumeEmitter"))
        {
            emitter->pausedByRequest = false;
            ApplyPause(*emitter, output);
        }
    }
    for (const PlaySound &event : ctx.events.Read<PlaySound>())
    {
        if (Runtime::AudioEmitter *emitter = EmitterOf(ctx.world, event.target, "PlaySound"))
        {
            RequestPlay(*emitter, ClipOf(*emitter, ctx.assets), output);
        }
    }

    // Every emitter, every frame: asking for the clip starts its load as soon as
    // the emitter is placed, and holding its sounds keeps them playing.
    for (auto [entity, emitter] : scene.Query<Runtime::AudioEmitter>())
    {
        (void)entity;
        AdvanceEmitter(emitter, ClipOf(emitter, ctx.assets), output);
    }
}

} // namespace Assisi::App
