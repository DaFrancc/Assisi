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
    case AudioError::Count:
        break;
    }
    return "unknown audio error";
}

} // namespace Assisi::Audio
