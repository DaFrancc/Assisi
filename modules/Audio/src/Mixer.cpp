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

/// @brief A level the game thread sets and the audio thread moves to in a
///        straight line, so a change never clicks.
///
/// The game thread writes `target`, or calls Reset while nothing renders the
/// node; everything else belongs to the audio thread.
struct Ramp
{
    std::atomic<float> target{1.0f};
    float level = 1.0f;
    float goal = 1.0f;
    float step = 0.0f;
    std::uint32_t framesLeft = 0;

    /// Be at @p value at once. Only while nothing is rendering the node.
    void Reset(float value)
    {
        target.store(value, std::memory_order_relaxed);
        level = value;
        goal = value;
        step = 0.0f;
        framesLeft = 0;
    }

    /// Once per buffer: start a ramp over @p length frames if the target moved.
    void Begin(std::uint32_t length)
    {
        const float wanted = target.load(std::memory_order_relaxed);
        if (wanted != goal)
        {
            goal = wanted;
            framesLeft = length;
            step = (wanted - level) / static_cast<float>(length);
        }
    }

    /// Once per frame: the level for this frame.
    float Next()
    {
        if (framesLeft > 0)
        {
            --framesLeft;
            // The last step lands exactly on the goal, whatever rounding the steps gathered.
            level = framesLeft == 0 ? goal : level + step;
        }
        return level;
    }
};

/// @brief A bus: sums everything attached to its input and scales it by its volume.
///
/// miniaudio's own gainer is not used: its vectorised path advances the gain
/// half as fast as it should and then jumps to catch up, which clicks.
struct BusNode
{
    /// First, so a pointer to this is a pointer to the backend's node.
    ma_node_base base{};
    Ramp gain;
    bool initialised = false;
};

/// @brief One claimed clip: copies its samples out at its own volume, loops,
///        pauses and fades out when stopped.
///
/// `clip`, `samples`, `frames` and `bus` are written by the game thread only
/// while the node is detached; attaching it publishes them to the audio thread,
/// and detaching waits until the audio thread has let go. The audio thread owns
/// `cursor`, `fadeLevel` and the ramps' levels while attached. `claimed`,
/// `attached` and `held` are the game thread's alone.
struct SoundNode
{
    /// First, so a pointer to this is a pointer to the backend's node.
    ma_node_base base{};
    std::shared_ptr<const PcmClip> clip;
    const float *samples = nullptr;
    std::uint64_t frames = 0;
    std::uint64_t cursor = 0;
    Ramp volume;
    /// 1 while playing, 0 while paused.
    Ramp presence;
    float fadeLevel = 1.0f;
    std::uint32_t generation = 0;
    std::uint16_t bus = 0;
    std::atomic<bool> looping{false};
    std::atomic<bool> stopping{false};
    std::atomic<bool> finished{false};
    bool claimed = false;
    bool attached = false;
    bool held = false;
    bool initialised = false;
};

/// @brief How far a stopped sound's level falls each frame.
constexpr float kFadeStep = 1.0f / static_cast<float>(kSoundFadeFrames);

/// @brief A sound's presence while it plays and while it is paused.
constexpr float kPresent = 1.0f;
constexpr float kAbsent = 0.0f;

void ProcessBus(ma_node *node, const float **framesIn, ma_uint32 * /*frameCountIn*/, float **framesOut,
                ma_uint32 *frameCountOut)
{
    BusNode *bus = static_cast<BusNode *>(node);
    bus->gain.Begin(kVolumeRampFrames);

    const float *in = framesIn[0];
    float *out = framesOut[0];
    const ma_uint32 frames = *frameCountOut;
    for (ma_uint32 frame = 0; frame < frames; ++frame)
    {
        const float gain = bus->gain.Next();
        for (std::uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            out[frame * kChannelCount + channel] = in[frame * kChannelCount + channel] * gain;
        }
    }
}

void ProcessSound(ma_node *node, const float ** /*framesIn*/, ma_uint32 * /*frameCountIn*/, float **framesOut,
                  ma_uint32 *frameCountOut)
{
    SoundNode *sound = static_cast<SoundNode *>(node);
    float *out = framesOut[0];
    const ma_uint32 frames = *frameCountOut;
    const bool looping = sound->looping.load(std::memory_order_relaxed);
    const bool stopping = sound->stopping.load(std::memory_order_relaxed);
    sound->volume.Begin(kVolumeRampFrames);
    sound->presence.Begin(kSoundFadeFrames);

    for (ma_uint32 frame = 0; frame < frames; ++frame)
    {
        const float volume = sound->volume.Next();
        const float presence = sound->presence.Next();
        if (stopping)
        {
            sound->fadeLevel = std::max(0.0f, sound->fadeLevel - kFadeStep);
        }
        const float gain = volume * presence * sound->fadeLevel;

        // A paused sound that has faded all the way out holds its place.
        const bool advancing = presence > kAbsent && sound->cursor < sound->frames;
        for (std::uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            const float sample = advancing ? sound->samples[sound->cursor * kChannelCount + channel] : 0.0f;
            out[frame * kChannelCount + channel] = sample * gain;
        }
        if (advancing)
        {
            ++sound->cursor;
            if (looping && sound->cursor >= sound->frames)
            {
                sound->cursor = 0;
            }
        }
    }

    const bool ranOut = sound->cursor >= sound->frames && (!looping || sound->frames == 0);
    if (ranOut || sound->fadeLevel <= 0.0f)
    {
        sound->finished.store(true, std::memory_order_release);
    }
}

const ma_node_vtable kBusVtable{
    .onProcess = ProcessBus,
    .onGetRequiredInputFrameCount = nullptr,
    .inputBusCount = 1,
    .outputBusCount = 1,
    // Processed with nothing attached too, so a quiet bus's ramp still advances.
    .flags = MA_NODE_FLAG_CONTINUOUS_PROCESSING,
};

const ma_node_vtable kSoundVtable{
    .onProcess = ProcessSound,
    .onGetRequiredInputFrameCount = nullptr,
    .inputBusCount = 0,
    .outputBusCount = 1,
    .flags = 0,
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
    Impl(const Impl &) = delete;
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
        return sound.index < sounds.size() && sounds[sound.index].claimed &&
               sounds[sound.index].generation == sound.generation;
    }

    /// Give @p sound's slot back. Detaching returns only once the audio thread
    /// is done with the node, so its fields and clip are the game thread's again.
    static void Release(SoundNode &sound)
    {
        if (sound.attached)
        {
            ma_node_detach_output_bus(&sound, 0);
        }
        sound.attached = false;
        sound.claimed = false;
        sound.clip.reset();
        sound.samples = nullptr;
        ++sound.generation;
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

    ma_node_config busConfig = ma_node_config_init();
    busConfig.vtable = &kBusVtable;
    busConfig.pInputChannels = kStereo.data();
    busConfig.pOutputChannels = kStereo.data();

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
        bus.gain.Reset(volume);

        const std::optional<BusId> parent = layout.Parent(id);
        ma_node *into =
            parent.has_value() ? static_cast<ma_node *>(&buses[parent->index]) : ma_node_graph_get_endpoint(&graph);
        if (ma_node_attach_output_bus(&bus, 0, into, 0) != MA_SUCCESS)
        {
            return false;
        }
    }

    ma_node_config soundConfig = ma_node_config_init();
    soundConfig.vtable = &kSoundVtable;
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

Mixer::Mixer(std::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl))
{
}
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
    _impl->buses[bus.index].gain.target.store(std::clamp(volume, kMinVolume, kMaxVolume), std::memory_order_relaxed);
}

float Mixer::BusVolume(BusId bus) const
{
    if (bus.index >= _impl->buses.size())
    {
        return kMinVolume;
    }
    return _impl->buses[bus.index].gain.target.load(std::memory_order_relaxed);
}

const BusLayout &Mixer::Layout() const noexcept
{
    return _impl->layout;
}

std::optional<BusId> Mixer::FindBus(std::string_view name) const
{
    return _impl->layout.FindBus(name);
}

std::expected<SoundHandle, AudioError> Mixer::Claim(std::shared_ptr<const PcmClip> clip, BusId bus)
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
        if (sound.claimed)
        {
            continue;
        }

        sound.samples = clip->samples.data();
        sound.frames = clip->Frames();
        sound.clip = std::move(clip);
        sound.cursor = 0;
        sound.fadeLevel = 1.0f;
        sound.bus = bus.index;
        sound.volume.Reset(kMaxVolume);
        sound.presence.Reset(kPresent);
        sound.looping.store(false, std::memory_order_relaxed);
        sound.stopping.store(false, std::memory_order_relaxed);
        sound.finished.store(false, std::memory_order_relaxed);
        sound.claimed = true;
        sound.held = true;
        return SoundHandle{.index = static_cast<std::uint32_t>(i), .generation = sound.generation};
    }
    return std::unexpected(AudioError::TooManySounds);
}

void Mixer::Play(SoundHandle sound)
{
    if (!_impl->IsLive(sound))
    {
        return;
    }
    SoundNode &node = _impl->sounds[sound.index];
    if (node.attached)
    {
        return;
    }
    if (ma_node_attach_output_bus(&node, 0, &_impl->buses[node.bus], 0) != MA_SUCCESS)
    {
        // Reads as finished from here on; nothing else would ever release it.
        Impl::Release(node);
        return;
    }
    node.attached = true;
}

void Mixer::SetVolume(SoundHandle sound, float volume)
{
    if (!_impl->IsLive(sound))
    {
        return;
    }
    SoundNode &node = _impl->sounds[sound.index];
    const float clamped = std::clamp(volume, kMinVolume, kMaxVolume);
    if (node.attached)
    {
        node.volume.target.store(clamped, std::memory_order_relaxed);
    }
    else
    {
        node.volume.Reset(clamped);
    }
}

void Mixer::SetLooping(SoundHandle sound, bool looping)
{
    if (_impl->IsLive(sound))
    {
        _impl->sounds[sound.index].looping.store(looping, std::memory_order_relaxed);
    }
}

void Mixer::Pause(SoundHandle sound)
{
    if (!_impl->IsLive(sound))
    {
        return;
    }
    SoundNode &node = _impl->sounds[sound.index];
    if (node.attached)
    {
        node.presence.target.store(kAbsent, std::memory_order_relaxed);
    }
    else
    {
        node.presence.Reset(kAbsent);
    }
}

void Mixer::Resume(SoundHandle sound)
{
    if (!_impl->IsLive(sound))
    {
        return;
    }
    SoundNode &node = _impl->sounds[sound.index];
    if (node.attached)
    {
        node.presence.target.store(kPresent, std::memory_order_relaxed);
    }
    else
    {
        node.presence.Reset(kPresent);
    }
}

void Mixer::Hold(SoundHandle sound)
{
    if (_impl->IsLive(sound))
    {
        _impl->sounds[sound.index].held = true;
    }
}

void Mixer::Stop(SoundHandle sound)
{
    if (!_impl->IsLive(sound))
    {
        return;
    }
    SoundNode &node = _impl->sounds[sound.index];
    if (node.attached)
    {
        node.stopping.store(true, std::memory_order_relaxed);
    }
    else
    {
        // Never heard, so there is nothing to fade.
        Impl::Release(node);
    }
}

bool Mixer::IsFinished(SoundHandle sound) const
{
    return !_impl->IsLive(sound) || _impl->sounds[sound.index].finished.load(std::memory_order_acquire);
}

std::expected<SoundHandle, AudioError> Mixer::Attach(std::shared_ptr<const PcmClip> clip, BusId bus)
{
    const std::expected<SoundHandle, AudioError> sound = Claim(std::move(clip), bus);
    if (sound)
    {
        Play(*sound);
    }
    return sound;
}

void Mixer::Update()
{
    for (SoundNode &sound : _impl->sounds)
    {
        if (!sound.claimed)
        {
            continue;
        }
        if (!sound.held)
        {
            if (!sound.attached)
            {
                Impl::Release(sound);
                continue;
            }
            sound.stopping.store(true, std::memory_order_relaxed);
        }
        sound.held = false;
        if (sound.attached && sound.finished.load(std::memory_order_acquire))
        {
            Impl::Release(sound);
        }
    }
}

void Mixer::Render(std::span<float> interleaved) noexcept
{
    const ma_uint64 frames = FrameCount(interleaved.size());
    ma_uint64 read = 0;
    ma_node_graph_read_pcm_frames(&_impl->graph, interleaved.data(), frames, &read);
    std::fill(interleaved.begin() + static_cast<std::ptrdiff_t>(read * kChannelCount), interleaved.end(), 0.0f);
}

} // namespace Assisi::Audio
