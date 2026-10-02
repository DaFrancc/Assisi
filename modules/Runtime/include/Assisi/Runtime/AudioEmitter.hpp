/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioEmitter.hpp
/// @brief The only source of sound: an entity with an AudioEmitter plays its
///        clip when an event asks it to.
///
/// The fields say what the emitter plays and how; App's AudioEmitters system
/// answers the events that make it play, stop, pause and resume. What the
/// emitter is playing is kept on the component itself, so it goes when the
/// entity does, and so do its sounds: nothing holds them any more, and the
/// mixer fades out whatever nobody holds.

#include <Assisi/Audio/SoundOutput.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/InternedString.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>

#include <cstdint>
#include <vector>

namespace Assisi::Runtime
{

/// @brief What an emitter does with a request to play when it already plays as
///        many sounds as it may.
AENUM()
enum class WhenFull : std::uint8_t
{
    /// The request is dropped.
    Drop,

    /// The request waits for a sound to end, up to the emitter's queue limit;
    /// past it, it is dropped.
    Queue,

    /// The sound that started first fades out and the new one starts.
    ReplaceOldest,

    /// The sound that started last fades out and the new one starts.
    ReplaceNewest,

    Count
};

/// @brief Where an emitter's sounds are heard from.
AENUM()
enum class EmitterSpace : std::uint8_t
{
    /// Played as it is, with no distance or direction: UI, music, narration.
    InEar,

    /// Placed in the world. Heard the same as InEar until the engine positions sounds.
    InWorld,

    Count
};

/// @brief Plays a sound clip when a PlaySound event names its entity.
ACOMP()
struct AudioEmitter
{
    // The unreflected members are what the emitter is doing, for the
    // AudioEmitters system alone: none of it means anything outside this run.

    /// Every sound the emitter has playing, the first started at the front.
    std::vector<Assisi::Audio::SoundHandle> playing;

    AFIELD() Assisi::Core::AssetId clip;

    /// The bus the emitter plays on, by name: "SFX", "Music", "UI", or one the
    /// game declares.
    AFIELD() Assisi::Core::InternedString bus { "SFX" };

    /// On top of the bus's volume, from 0 to 1.
    AFIELD(min = 0.0, max = 1.0) float volume = 1.0f;

    /// Sounds the emitter plays at once. 0 reads as 1.
    AFIELD() uint32_t maxConcurrentSounds = 1;

    /// Requests that wait for a free slot when `whenFull` is Queue.
    AFIELD() uint32_t maxQueuedRequests = 1;

    /// Requests waiting for a slot. A count, since every request is the same.
    std::uint32_t queued = 0;

    AFIELD() WhenFull whenFull = WhenFull::Drop;
    AFIELD() EmitterSpace space = EmitterSpace::InEar;
    AFIELD() bool looping = false;

    /// Whether the emitter plays on while its world is paused. A default emitter
    /// always does.
    AFIELD() bool ignoresWorldPause = false;

    /// Paused by a PauseEmitter event, and by its world being paused.
    bool pausedByRequest = false;
    bool pausedByWorld = false;

    /// Whether the emitter has already said why it could not play.
    bool warned = false;
};

/// @brief Marks the scene's general-purpose speaker: the entity that plays sounds
///        which belong to no place, such as the UI's.
///
/// Its entity also needs an AudioEmitter; App's FindDefaultEmitter finds it and
/// warns when there is none, more than one, or one with no emitter. A default
/// emitter plays on while its world is paused.
ACOMP()
struct DefaultEmitter
{
};

} // namespace Assisi::Runtime
