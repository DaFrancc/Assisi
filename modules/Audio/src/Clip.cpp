/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/Clip.hpp>

#include <miniaudio.h>

namespace Assisi::Audio
{

namespace
{

/// @brief Frames decoded per read. The length a decoder reports up front is
/// zero for some encodings, so the clip is read until the decoder runs dry.
constexpr ma_uint64 kDecodeChunkFrames = 4096;

/// @brief Uninitialises a decoder on every path out of DecodeClip.
class DecoderGuard
{
public:
    explicit DecoderGuard(ma_decoder &decoder) noexcept : _decoder(decoder) {}
    DecoderGuard(const DecoderGuard &)            = delete;
    DecoderGuard &operator=(const DecoderGuard &) = delete;
    ~DecoderGuard() { ma_decoder_uninit(&_decoder); }

private:
    ma_decoder &_decoder;
};

} // namespace

std::expected<PcmClip, AudioError> DecodeClip(std::span<const std::byte> encoded)
{
    const ma_decoder_config config = ma_decoder_config_init(ma_format_f32, kChannelCount, kSampleRate);
    ma_decoder decoder;
    if (ma_decoder_init_memory(encoded.data(), encoded.size(), &config, &decoder) != MA_SUCCESS)
    {
        return std::unexpected(AudioError::UnsupportedEncoding);
    }
    const DecoderGuard guard(decoder);

    PcmClip clip;
    while (true)
    {
        const std::size_t offset = clip.samples.size();
        clip.samples.resize(offset + static_cast<std::size_t>(kDecodeChunkFrames) * kChannelCount);

        ma_uint64 framesRead = 0;
        const ma_result result =
            ma_decoder_read_pcm_frames(&decoder, clip.samples.data() + offset, kDecodeChunkFrames, &framesRead);
        clip.samples.resize(offset + static_cast<std::size_t>(framesRead) * kChannelCount);

        if (result == MA_AT_END || (result == MA_SUCCESS && framesRead == 0))
        {
            break;
        }
        if (result != MA_SUCCESS)
        {
            return std::unexpected(AudioError::DecodeFailed);
        }
    }
    clip.samples.shrink_to_fit();
    return clip;
}

} // namespace Assisi::Audio
