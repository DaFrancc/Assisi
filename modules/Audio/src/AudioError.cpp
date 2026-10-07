/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/AudioError.hpp>

namespace Assisi::Audio
{

std::string_view ToString(AudioError error) noexcept
{
    switch (error)
    {
    case AudioError::BackendUnavailable:
        return "no audio backend could be initialised";
    case AudioError::DeviceInitFailed:
        return "the audio device could not be opened";
    case AudioError::DeviceStartFailed:
        return "the audio device could not be started";
    case AudioError::AlreadyStarted:
        return "the audio device is already running";
    case AudioError::UnsupportedEncoding:
        return "not audio in any format this build decodes";
    case AudioError::DecodeFailed:
        return "the audio could not be decoded";
    case AudioError::EnumerationFailed:
        return "the audio devices could not be listed";
    case AudioError::MixerInitFailed:
        return "the mixer could not be built";
    case AudioError::TooManySounds:
        return "every sound slot is playing";
    case AudioError::UnknownBus:
        return "no bus with that id";
    case AudioError::NoClip:
        return "no clip to play";
    case AudioError::TooManyBuses:
        return "more buses are declared than the mixer holds";
    case AudioError::DuplicateBus:
        return "a bus is declared twice, or declared with a default bus's name";
    case AudioError::UnknownParentBus:
        return "a bus's parent is not a default bus or one declared before it";
    case AudioError::Count_:
        break;
    }
    return "unknown audio error";
}

} // namespace Assisi::Audio
