/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AudioSystems.hpp
/// @brief The events that make an AudioEmitter play, stop, pause and resume, and
///        the system that answers them.
///
/// Every sound is asked for through these events, so gameplay and UI use one
/// path. Each names the entity whose emitter it is for; one that names none, or
/// an entity with no AudioEmitter, is logged and does nothing.
///
/// The system runs in PostUpdate, so an event pushed from Update or earlier is
/// answered the same frame. Within a frame, stops, pauses and resumes are
/// answered before plays: a stop and a play for one emitter restart it.
///
/// Emitters live in one world each, and these events name an entity only; a
/// game with several worlds running emitters at once can reach the wrong one.

#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/SoundOutput.hpp>
#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/ECS/Entity.hpp>
#include <Assisi/Runtime/AudioEmitter.hpp>

#include <memory>
#include <optional>

namespace Assisi::App
{

struct SystemContext;
struct World;

/// @brief Play the target's clip once, or as its emitter's `whenFull` says when
///        it already plays as many sounds as it may.
AEVENT()
struct PlaySound
{
    ECS::Entity target = ECS::NullEntity;
};

/// @brief Fade out everything the target's emitter plays, and forget what it has queued.
AEVENT()
struct StopEmitter
{
    ECS::Entity target = ECS::NullEntity;
};

/// @brief Pause everything the target's emitter plays, where it is. Sounds it
///        starts while paused start silent.
AEVENT()
struct PauseEmitter
{
    ECS::Entity target = ECS::NullEntity;
};

/// @brief Undo a PauseEmitter. An emitter paused by its world stays paused
///        until the world resumes too.
AEVENT()
struct ResumeEmitter
{
    ECS::Entity target = ECS::NullEntity;
};

/// @brief Answers the events above, keeps every emitter's sounds alive, starts
///        queued requests as slots free up, and pauses emitters with their world.
///
/// Asks for every emitter's clip each frame, so a clip is loading from the
/// moment its emitter is placed. A request that arrives before the clip has
/// loaded waits as a queued one does. Does nothing in a host with no audio.
///
/// A level or blueprint that places emitters names this system; without it
/// nothing holds their sounds, and every sound fades out as it starts.
ASYSTEM(PostUpdate, name = "AudioEmitters") void AudioEmitterSystem(SystemContext &ctx);

/// @brief Whether @p emitter has any sound playing, paused ones included.
[[nodiscard]] bool IsPlaying(const Runtime::AudioEmitter &emitter);

/// @brief The entity carrying Runtime::DefaultEmitter in @p world. Empty, with a
///        warning, when none does, when one has no AudioEmitter, or when more
///        than one does.
[[nodiscard]] std::optional<ECS::Entity> FindDefaultEmitter(World &world);

// The steps the system takes for one emitter, on their own so each can be
// driven directly. @p clip is null while the emitter's clip is loading or when
// it failed to load.

/// @brief Answer one PlaySound.
void RequestPlay(Runtime::AudioEmitter &emitter, std::shared_ptr<const Audio::PcmClip> clip,
                 Audio::SoundOutput &output);

/// @brief Answer one StopEmitter.
void StopAll(Runtime::AudioEmitter &emitter, Audio::SoundOutput &output);

/// @brief Bring every playing sound in line with the emitter's pause flags.
void ApplyPause(const Runtime::AudioEmitter &emitter, Audio::SoundOutput &output);

/// @brief Once a frame: forget finished sounds, hold and set the volume of the
///        rest, and start queued requests while there is room.
void AdvanceEmitter(Runtime::AudioEmitter &emitter, std::shared_ptr<const Audio::PcmClip> clip,
                    Audio::SoundOutput &output);

} // namespace Assisi::App
