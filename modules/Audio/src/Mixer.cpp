/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/Mixer.hpp>

#include <miniaudio.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

namespace Assisi::Audio
{

namespace
{

/// @brief A bus: sums everything attached to its input and scales it by its volume.
///
/// The game thread writes `target`; the audio thread ramps `gain` to it
/// linearly over kVolumeRampFrames. Everything but `target` belongs to the
/// audio thread once the bus is built.
///
/// miniaudio's own gainer is not used: its vectorised path advances the gain
/// half as fast as it should and then jumps to catch up, which clicks.
struct BusNode
{
    /// First, so a pointer to this is a pointer to the backend's node.
    ma_node_base base{};
    std::atomic<float> target{1.0f};
    float gain                   = 1.0f;
    float rampGoal               = 1.0f;
    float rampStep               = 0.0f;
    std::uint32_t rampFramesLeft = 0;
    bool initialised             = false;
};

/// @brief One playing clip: copies its samples out and fades them when stopped.
///
/// `clip`, `samples` and `frames` are written by the game thread only while the
/// node is detached; attaching it publishes them to the audio thread, and
/// detaching waits until the audio thread has let go. The audio thread owns
/// `cursor` and `fadeLevel` while attached.
struct SoundNode
{
    /// First, so a pointer to this is a pointer to the backend's node.
    ma_node_base base{};
    std::shared_ptr<const PcmClip> clip;
    const float *samples   = nullptr;
    std::uint64_t frames   = 0;
    std::uint64_t cursor   = 0;
    float fadeLevel        = 1.0f;
    std::uint32_t generation = 0;
    std::atomic<bool> stopping{false};
    std::atomic<bool> finished{false};
    bool attached    = false;
    bool initialised = false;
};

/// @brief How far a stopped sound's level falls each frame.
constexpr float kFadeStep = 1.0f / static_cast<float>(kSoundFadeFrames);

void ProcessBus(ma_node *node, const float **framesIn, ma_uint32 * /*frameCountIn*/, float **framesOut,
                ma_uint32 *frameCountOut)
{
    BusNode *bus       = static_cast<BusNode *>(node);
    const float target = bus->target.load(std::memory_order_relaxed);
    if (target != bus->rampGoal)
    {
        bus->rampGoal       = target;
        bus->rampFramesLeft = kVolumeRampFrames;
        bus->rampStep       = (target - bus->gain) / static_cast<float>(kVolumeRampFrames);
    }

    const float *in        = framesIn[0];
    float *out             = framesOut[0];
    const ma_uint32 frames = *frameCountOut;
    for (ma_uint32 frame = 0; frame < frames; ++frame)
    {
        if (bus->rampFramesLeft > 0)
        {
            --bus->rampFramesLeft;
            // The last step lands exactly on the goal, whatever rounding the steps gathered.
            bus->gain = bus->rampFramesLeft == 0 ? bus->rampGoal : bus->gain + bus->rampStep;
        }
        for (std::uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            out[frame * kChannelCount + channel] = in[frame * kChannelCount + channel] * bus->gain;
        }
    }
}

void ProcessSound(ma_node *node, const float ** /*framesIn*/, ma_uint32 * /*frameCountIn*/, float **framesOut,
                  ma_uint32 *frameCountOut)
{
    SoundNode *sound       = static_cast<SoundNode *>(node);
    float *out             = framesOut[0];
    const ma_uint32 frames = *frameCountOut;

    const std::uint64_t remaining = sound->frames - std::min(sound->cursor, sound->frames);
    const std::uint64_t copied    = std::min<std::uint64_t>(remaining, frames);
    if (copied > 0)
    {
        std::memcpy(out, sound->samples + sound->cursor * kChannelCount,
                    static_cast<std::size_t>(copied) * kChannelCount * sizeof(float));
    }
    std::fill(out + copied * kChannelCount, out + static_cast<std::uint64_t>(frames) * kChannelCount, 0.0f);
    sound->cursor += copied;

    if (sound->stopping.load(std::memory_order_relaxed))
    {
        for (ma_uint32 frame = 0; frame < frames; ++frame)
        {
            sound->fadeLevel = std::max(0.0f, sound->fadeLevel - kFadeStep);
            for (std::uint32_t channel = 0; channel < kChannelCount; ++channel)
            {
                out[frame * kChannelCount + channel] *= sound->fadeLevel;
            }
        }
    }

    if (sound->cursor >= sound->frames || sound->fadeLevel <= 0.0f)
    {
        sound->finished.store(true, std::memory_order_release);
    }
}

const ma_node_vtable kBusVtable{
    .onProcess                    = ProcessBus,
    .onGetRequiredInputFrameCount = nullptr,
    .inputBusCount                = 1,
    .outputBusCount               = 1,
    // Processed with nothing attached too, so a quiet bus's ramp still advances.
    .flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING,
};

const ma_node_vtable kSoundVtable{
    .onProcess                    = ProcessSound,
    .onGetRequiredInputFrameCount = nullptr,
    .inputBusCount                = 0,
    .outputBusCount               = 1,
    .flags                        = 0,
};

constexpr std::array<ma_uint32, 1> kStereo{kChannelCount};

} // namespace

struct Mixer::Impl
{
    /// Never moved once initialised: every node holds its address.
    ma_node_graph graph{};
    BusLayout layout;
    /// Sized once, before any node is initialised, and never resized: the
    /// backend holds every node's address.
    std::vector<BusNode> buses;
    std::array<SoundNode, kMaxSounds> sounds{};
    bool graphInitialised = false;

    explicit Impl(BusLayout busLayout) : layout(std::move(busLayout)), buses(layout.Count()) {}
    Impl(const Impl &)            = delete;
    Impl &operator=(const Impl &) = delete;

    ~Impl()
    {
        // Children before parents, so nothing is ever attached to a node already gone.
        for (SoundNode &sound : sounds)
        {
            if (sound.initialised)
            {
                ma_node_uninit(&sound, nullptr);
            }
        }
        for (std::size_t i = buses.size(); i-- > 0;)
        {
            if (buses[i].initialised)
            {
                ma_node_uninit(&buses[i], nullptr);
            }
        }
        if (graphInitialised)
        {
            ma_node_graph_uninit(&graph, nullptr);
        }
    }

    [[nodiscard]] bool Build();
    [[nodiscard]] bool IsLive(SoundHandle sound) const
    {
        return sound.index < sounds.size() && sounds[sound.index].attached &&
               sounds[sound.index].generation == sound.generation;
    }
};

bool Mixer::Impl::Build()
{
    const ma_node_graph_config graphConfig = ma_node_graph_config_init(kChannelCount);
    if (ma_node_graph_init(&graphConfig, nullptr, &graph) != MA_SUCCESS)
    {
        return false;
    }
    graphInitialised = true;

    ma_node_config busConfig            = ma_node_config_init();
    busConfig.vtable                    = &kBusVtable;
    busConfig.pInputChannels            = kStereo.data();
    busConfig.pOutputChannels           = kStereo.data();

    // A layout lists parents before children, so each bus's parent is built first.
    for (std::size_t i = 0; i < buses.size(); ++i)
    {
        BusNode &bus = buses[i];
        if (ma_node_init(&graph, &busConfig, nullptr, &bus) != MA_SUCCESS)
        {
            return false;
        }
        bus.initialised = true;

        // Starts at its volume rather than ramping there, since nothing has played yet.
        const BusId id{static_cast<std::uint16_t>(i)};
        const float volume = std::clamp(layout.DefaultVolume(id), kMinVolume, kMaxVolume);
        bus.target.store(volume, std::memory_order_relaxed);
        bus.gain     = volume;
        bus.rampGoal = volume;

        const std::optional<BusId> parent = layout.Parent(id);
        ma_node *into = parent.has_value() ? static_cast<ma_node *>(&buses[parent->index])
                                           : ma_node_graph_get_endpoint(&graph);
        if (ma_node_attach_output_bus(&bus, 0, into, 0) != MA_SUCCESS)
        {
            return false;
        }
    }

    ma_node_config soundConfig = ma_node_config_init();
    soundConfig.vtable         = &kSoundVtable;
    soundConfig.pOutputChannels = kStereo.data();
    for (SoundNode &sound : sounds)
    {
        if (ma_node_init(&graph, &soundConfig, nullptr, &sound) != MA_SUCCESS)
        {
            return false;
        }
        sound.initialised = true;
    }
    return true;
}

Mixer::Mixer(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
Mixer::~Mixer() = default;

std::expected<std::unique_ptr<Mixer>, AudioError> Mixer::Create(const BusLayout &layout)
{
    std::unique_ptr<Impl> impl = std::make_unique<Impl>(layout);
    if (!impl->Build())
    {
        return std::unexpected(AudioError::MixerInitFailed);
    }
    return std::unique_ptr<Mixer>(new Mixer(std::move(impl)));
}

void Mixer::SetBusVolume(BusId bus, float volume)
{
    if (bus.index >= _impl->buses.size())
    {
        return;
    }
    _impl->buses[bus.index].target.store(std::clamp(volume, kMinVolume, kMaxVolume), std::memory_order_relaxed);
}

float Mixer::BusVolume(BusId bus) const
{
    if (bus.index >= _impl->buses.size())
    {
        return kMinVolume;
    }
    return _impl->buses[bus.index].target.load(std::memory_order_relaxed);
}

const BusLayout &Mixer::Layout() const noexcept
{
    return _impl->layout;
}

std::expected<SoundHandle, AudioError> Mixer::Attach(std::shared_ptr<const PcmClip> clip, BusId bus)
{
    if (bus.index >= _impl->buses.size())
    {
        return std::unexpected(AudioError::UnknownBus);
    }
    if (clip == nullptr)
    {
        return std::unexpected(AudioError::NoClip);
    }

    for (std::size_t i = 0; i < _impl->sounds.size(); ++i)
    {
        SoundNode &sound = _impl->sounds[i];
        if (sound.attached)
        {
            continue;
        }

        sound.samples   = clip->samples.data();
        sound.frames    = clip->Frames();
        sound.clip      = std::move(clip);
        sound.cursor    = 0;
        sound.fadeLevel = 1.0f;
        sound.stopping.store(false, std::memory_order_relaxed);
        sound.finished.store(false, std::memory_order_relaxed);
        if (ma_node_attach_output_bus(&sound, 0, &_impl->buses[bus.index], 0) != MA_SUCCESS)
        {
            sound.clip.reset();
            return std::unexpected(AudioError::MixerInitFailed);
        }
        sound.attached = true;
        return SoundHandle{.index = static_cast<std::uint32_t>(i), .generation = sound.generation};
    }
    return std::unexpected(AudioError::TooManySounds);
}

void Mixer::Stop(SoundHandle sound)
{
    if (_impl->IsLive(sound))
    {
        _impl->sounds[sound.index].stopping.store(true, std::memory_order_relaxed);
    }
}

bool Mixer::IsFinished(SoundHandle sound) const
{
    return !_impl->IsLive(sound) || _impl->sounds[sound.index].finished.load(std::memory_order_acquire);
}

void Mixer::Update()
{
    for (SoundNode &sound : _impl->sounds)
    {
        if (!sound.attached || !sound.finished.load(std::memory_order_acquire))
        {
            continue;
        }
        // Returns only once the audio thread is done with the node, so its
        // fields and clip are the game thread's again.
        ma_node_detach_output_bus(&sound, 0);
        sound.attached = false;
        sound.clip.reset();
        sound.samples = nullptr;
        ++sound.generation;
    }
}

void Mixer::Render(std::span<float> interleaved) noexcept
{
    const ma_uint64 frames = FrameCount(interleaved.size());
    ma_uint64 read         = 0;
    ma_node_graph_read_pcm_frames(&_impl->graph, interleaved.data(), frames, &read);
    std::fill(interleaved.begin() + static_cast<std::ptrdiff_t>(read * kChannelCount), interleaved.end(), 0.0f);
}

} // namespace Assisi::Audio
