/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAudioEmitter.cpp
/// @brief What an emitter does with requests when it is full, how it pauses and
/// stops, and what the AudioEmitters system does with events, a paused world, a
/// destroyed emitter and the default emitter.
///
/// The mixer is a recording stand-in, so each test reads what the emitter asked
/// of it rather than listening for it.

#include <Assisi/App/AudioSystems.hpp>
#include <Assisi/App/SystemRegistry.hpp>
#include <Assisi/App/World.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/SoundAsset.hpp>
#include <Assisi/Audio/SoundOutput.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/JobSystem.hpp>
#include <Assisi/Runtime/AudioEmitter.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Assisi;
using Runtime::AudioEmitter;
using Runtime::WhenFull;

namespace
{

/// Enough workers that a clip really loads off the main thread.
constexpr std::uint32_t kWorkers = 2;

constexpr std::size_t kClipSamples = 64;

/// Records every call, in order, so a test can read what an emitter asked for.
class RecordingOutput final : public Audio::SoundOutput
{
  public:
    [[nodiscard]] std::optional<Audio::BusId> FindBus(std::string_view name) const override
    {
        if (name == "SFX" || name == "UI")
        {
            return Audio::ToBusId(Audio::DefaultBus::Sfx);
        }
        return std::nullopt;
    }

    [[nodiscard]] std::expected<Audio::SoundHandle, Audio::AudioError> Claim(std::shared_ptr<const Audio::PcmClip>,
                                                                             Audio::BusId) override
    {
        const Audio::SoundHandle sound{.index = _next++, .generation = 0};
        calls.push_back("claim " + std::to_string(sound.index));
        return sound;
    }

    void Play(Audio::SoundHandle sound) override
    {
        played.push_back(sound.index);
        calls.push_back("play " + std::to_string(sound.index));
    }

    void SetVolume(Audio::SoundHandle sound, float volume) override { volumes[sound.index] = volume; }
    void SetLooping(Audio::SoundHandle sound, bool looping) override { loops[sound.index] = looping; }

    void Pause(Audio::SoundHandle sound) override
    {
        paused.insert(sound.index);
        calls.push_back("pause " + std::to_string(sound.index));
    }

    void Resume(Audio::SoundHandle sound) override { paused.erase(sound.index); }
    void Hold(Audio::SoundHandle sound) override { held.insert(sound.index); }
    void Stop(Audio::SoundHandle sound) override { stopped.push_back(sound.index); }

    [[nodiscard]] bool IsFinished(Audio::SoundHandle sound) const override
    {
        return finished.contains(sound.index) || std::ranges::find(stopped, sound.index) != stopped.end();
    }

    std::vector<std::string> calls;
    std::vector<std::uint32_t> played;
    std::vector<std::uint32_t> stopped;
    std::set<std::uint32_t> paused;
    std::set<std::uint32_t> held;
    std::set<std::uint32_t> finished;
    std::unordered_map<std::uint32_t, float> volumes;
    std::unordered_map<std::uint32_t, bool> loops;

  private:
    std::uint32_t _next = 0;
};

std::shared_ptr<const Audio::PcmClip> AnyClip()
{
    std::shared_ptr<Audio::PcmClip> clip = std::make_shared<Audio::PcmClip>();
    clip->samples.assign(kClipSamples, 0.0f);
    return clip;
}

/// An emitter that already plays @p count sounds, the first started first.
AudioEmitter FullEmitter(WhenFull whenFull, std::uint32_t count, RecordingOutput &output)
{
    AudioEmitter emitter;
    emitter.whenFull = whenFull;
    emitter.maxConcurrentSounds = count;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        App::RequestPlay(emitter, AnyClip(), output);
    }
    REQUIRE(emitter.playing.size() == count);
    return emitter;
}

std::vector<std::uint32_t> Indices(const AudioEmitter &emitter)
{
    std::vector<std::uint32_t> indices;
    for (const Audio::SoundHandle &sound : emitter.playing)
    {
        indices.push_back(sound.index);
    }
    return indices;
}

/// Cooked sound blobs by id, held in memory: a package without the files.
class MemoryPackage final : public Core::AssetProvider
{
  public:
    void Add(Core::AssetId id, std::vector<std::byte> bytes) { _files[id] = std::move(bytes); }

    [[nodiscard]] std::expected<std::vector<std::byte>, Core::AssetError> Open(Core::AssetId id) const override
    {
        const std::unordered_map<Core::AssetId, std::vector<std::byte>>::const_iterator found = _files.find(id);
        if (found == _files.end())
        {
            return std::unexpected(Core::AssetErrorCode::UnknownAssetId);
        }
        return found->second;
    }

    [[nodiscard]] std::expected<Core::AssetId, Core::AssetError> Resolve(std::string_view) const override
    {
        return std::unexpected(Core::AssetErrorCode::UnknownAssetId);
    }

  private:
    std::unordered_map<Core::AssetId, std::vector<std::byte>> _files;
};

std::vector<std::byte> ReadFixture(std::string_view name)
{
    std::ifstream file(std::filesystem::path{ASSISI_AUDIO_FIXTURE_DIR} / name, std::ios::binary);
    const std::vector<char> chars{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const std::byte *first = reinterpret_cast<const std::byte *>(chars.data());
    return std::vector<std::byte>{first, first + chars.size()};
}

/// A world, a recording mixer and a sound already loaded in the asset store:
/// what the AudioEmitters system needs to run a frame.
struct Stage
{
    Core::JobSystem jobs{kWorkers};
    MemoryPackage package;
    Core::AssetStore store;
    Core::EventQueue events;
    RecordingOutput output;
    App::WorldManager worlds;
    App::World &world = worlds.Create("Audio");
    const Core::AssetId clip = Core::DerivedAssetId("sounds/tone.ogg");

    Stage()
    {
        const Core::AssetKind *kind = Core::AssetKindRegistry::Instance().Find(Audio::kSoundKind);
        REQUIRE(kind != nullptr);
        std::expected<std::vector<std::byte>, Core::AssetError> blob =
            Core::CookAssetBytes(*kind, ReadFixture("tone.ogg"));
        REQUIRE(blob.has_value());
        package.Add(clip, std::move(*blob));
        store.Initialize(jobs, package);
        (void)store.Resolve<Audio::PcmClip>(clip);
        jobs.HelpUntil([this] { return !store.HasPendingLoads(); }, true);
        REQUIRE(store.Resolve<Audio::PcmClip>(clip) != nullptr);
    }

    ECS::Entity Emitter()
    {
        const ECS::Entity entity = world.scene.Create();
        AudioEmitter emitter;
        emitter.clip = clip;
        (void)world.scene.Add<AudioEmitter>(entity, emitter);
        return entity;
    }

    /// One frame: the mixer's update first, as the application runs it, then the system.
    void RunFrame()
    {
        output.held.clear();
        App::SystemContext ctx{.world = world,
                               .dt = 0.f,
                               .simTick = 0,
                               .input = nullptr,
                               .actions = nullptr,
                               .events = events,
                               .isActiveWorld = true,
                               .worldManager = &worlds,
                               .mixer = &output,
                               .assets = &store};
        App::AudioEmitterSystem(ctx);
        events.Flush();
    }
};

} // namespace

TEST_CASE("A full emitter set to Drop starts nothing and queues nothing")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::Drop, 1, output);
    App::RequestPlay(emitter, AnyClip(), output);
    CHECK(output.played.size() == 1);
    CHECK(output.stopped.empty());
    CHECK(emitter.queued == 0);
}

TEST_CASE("A full emitter set to Queue waits up to its limit, and starts one per freed slot")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::Queue, 1, output);
    emitter.maxQueuedRequests = 2;
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        App::RequestPlay(emitter, AnyClip(), output);
    }
    CHECK(emitter.queued == 2);
    CHECK(output.played.size() == 1);

    output.finished.insert(emitter.playing.front().index);
    App::AdvanceEmitter(emitter, AnyClip(), output);
    CHECK(output.played.size() == 2);
    CHECK(emitter.queued == 1);
    CHECK(emitter.playing.size() == 1);
}

TEST_CASE("A full emitter set to ReplaceOldest stops the sound started first")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::ReplaceOldest, 2, output);
    const std::vector<std::uint32_t> before = Indices(emitter);

    App::RequestPlay(emitter, AnyClip(), output);
    REQUIRE(output.stopped.size() == 1);
    CHECK(output.stopped.front() == before.front());
    CHECK(Indices(emitter) == std::vector<std::uint32_t>{before.back(), output.played.back()});
}

TEST_CASE("A full emitter set to ReplaceNewest stops the sound started last")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::ReplaceNewest, 2, output);
    const std::vector<std::uint32_t> before = Indices(emitter);

    App::RequestPlay(emitter, AnyClip(), output);
    REQUIRE(output.stopped.size() == 1);
    CHECK(output.stopped.front() == before.back());
    CHECK(Indices(emitter) == std::vector<std::uint32_t>{before.front(), output.played.back()});
}

TEST_CASE("Replacing on an emitter of one sound restarts it")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::ReplaceOldest, 1, output);
    const std::uint32_t first = emitter.playing.front().index;

    App::RequestPlay(emitter, AnyClip(), output);
    CHECK(output.stopped == std::vector<std::uint32_t>{first});
    CHECK(emitter.playing.size() == 1);
    CHECK(emitter.playing.front().index != first);
}

TEST_CASE("A sound starts with the emitter's volume and looping, and paused if the emitter is")
{
    RecordingOutput output;
    AudioEmitter emitter;
    emitter.volume = 0.25f;
    emitter.looping = true;
    emitter.pausedByRequest = true;
    App::RequestPlay(emitter, AnyClip(), output);

    REQUIRE(emitter.playing.size() == 1);
    const std::uint32_t sound = emitter.playing.front().index;
    CHECK(output.volumes.at(sound) == doctest::Approx(0.25f));
    CHECK(output.loops.at(sound));
    // Paused before it is played, so it never makes a sound while its emitter is paused.
    CHECK(output.calls == std::vector<std::string>{"claim 0", "pause 0", "play 0"});
}

TEST_CASE("A request that arrives before the clip has loaded waits like a queued one")
{
    RecordingOutput output;
    AudioEmitter emitter;
    emitter.whenFull = WhenFull::Drop;
    App::RequestPlay(emitter, nullptr, output);
    CHECK(output.played.empty());
    CHECK(emitter.queued == 1);

    App::AdvanceEmitter(emitter, AnyClip(), output);
    CHECK(output.played.size() == 1);
    CHECK(emitter.queued == 0);
}

TEST_CASE("Stopping an emitter stops every sound and forgets its queue")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::Queue, 2, output);
    App::RequestPlay(emitter, AnyClip(), output);
    REQUIRE(emitter.queued == 1);
    const std::vector<std::uint32_t> before = Indices(emitter);

    App::StopAll(emitter, output);
    CHECK(output.stopped == before);
    CHECK(emitter.playing.empty());
    CHECK(emitter.queued == 0);

    App::AdvanceEmitter(emitter, AnyClip(), output);
    CHECK(output.played.size() == 2);
}

TEST_CASE("An emitter resumes only once neither its own pause nor its world's holds it")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::Drop, 1, output);
    const std::uint32_t sound = emitter.playing.front().index;

    emitter.pausedByRequest = true;
    emitter.pausedByWorld = true;
    App::ApplyPause(emitter, output);
    CHECK(output.paused.contains(sound));

    emitter.pausedByRequest = false;
    App::ApplyPause(emitter, output);
    CHECK(output.paused.contains(sound));

    emitter.pausedByWorld = false;
    App::ApplyPause(emitter, output);
    CHECK_FALSE(output.paused.contains(sound));
}

TEST_CASE("Each frame an emitter holds what still plays and forgets what finished")
{
    RecordingOutput output;
    AudioEmitter emitter = FullEmitter(WhenFull::Drop, 2, output);
    const std::vector<std::uint32_t> before = Indices(emitter);
    output.finished.insert(before.front());

    App::AdvanceEmitter(emitter, AnyClip(), output);
    CHECK(Indices(emitter) == std::vector<std::uint32_t>{before.back()});
    CHECK(output.held == std::set<std::uint32_t>{before.back()});
}

TEST_CASE("An emitter on a bus the mixer does not have plays nothing")
{
    RecordingOutput output;
    AudioEmitter emitter;
    emitter.bus = Core::InternedString{"Nowhere"};
    App::RequestPlay(emitter, AnyClip(), output);
    CHECK(output.played.empty());
    CHECK(emitter.warned);
}

TEST_CASE("PlaySound plays its target's emitter, and one naming no entity plays nothing")
{
    Stage stage;
    const ECS::Entity emitter = stage.Emitter();

    stage.events.Push(App::PlaySound{});
    stage.RunFrame();
    CHECK(stage.output.played.empty());

    stage.events.Push(App::PlaySound{.target = emitter});
    stage.RunFrame();
    CHECK(stage.output.played.size() == 1);
}

TEST_CASE("A paused world pauses its emitters, except the default one and those that ignore it")
{
    Stage stage;
    const ECS::Entity plain = stage.Emitter();
    const ECS::Entity ignoring = stage.Emitter();
    stage.world.scene.Get<AudioEmitter>(ignoring)->ignoresWorldPause = true;
    const ECS::Entity speaker = stage.Emitter();
    (void)stage.world.scene.Add<Runtime::DefaultEmitter>(speaker);
    for (const ECS::Entity entity : {plain, ignoring, speaker})
    {
        stage.events.Push(App::PlaySound{.target = entity});
    }
    stage.RunFrame();
    REQUIRE(stage.output.played.size() == 3);

    stage.world.paused = true;
    stage.RunFrame();
    const Audio::SoundHandle plainSound = stage.world.scene.Get<AudioEmitter>(plain)->playing.front();
    CHECK(stage.output.paused == std::set<std::uint32_t>{plainSound.index});

    stage.world.paused = false;
    stage.RunFrame();
    CHECK(stage.output.paused.empty());
}

TEST_CASE("A destroyed emitter's sounds are no longer held, and its queue never plays")
{
    Stage stage;
    const ECS::Entity entity = stage.Emitter();
    AudioEmitter *emitter = stage.world.scene.Get<AudioEmitter>(entity);
    emitter->whenFull = WhenFull::Queue;
    emitter->maxQueuedRequests = 2;
    for (std::uint32_t i = 0; i < 3; ++i)
    {
        stage.events.Push(App::PlaySound{.target = entity});
    }
    stage.RunFrame();
    REQUIRE(stage.output.played.size() == 1);
    REQUIRE(stage.output.held.size() == 1);

    stage.world.scene.Destroy(entity);
    stage.world.scene.FlushDestroyed();
    stage.output.finished.insert(stage.output.played.front());
    stage.RunFrame();
    CHECK(stage.output.held.empty());
    CHECK(stage.output.played.size() == 1);
}

TEST_CASE("The default emitter is found only when exactly one entity carries it with an emitter")
{
    Stage stage;
    CHECK_FALSE(App::FindDefaultEmitter(stage.world).has_value());

    const ECS::Entity bare = stage.world.scene.Create();
    (void)stage.world.scene.Add<Runtime::DefaultEmitter>(bare);
    CHECK_FALSE(App::FindDefaultEmitter(stage.world).has_value());

    (void)stage.world.scene.Add<AudioEmitter>(bare);
    const std::optional<ECS::Entity> found = App::FindDefaultEmitter(stage.world);
    REQUIRE(found.has_value());
    CHECK(*found == bare);

    const ECS::Entity second = stage.Emitter();
    (void)stage.world.scene.Add<Runtime::DefaultEmitter>(second);
    CHECK_FALSE(App::FindDefaultEmitter(stage.world).has_value());
}
