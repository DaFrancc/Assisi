/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Audio/BusConfig.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/Mixer.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

using namespace Assisi::Audio;

namespace
{

/// @brief Float arithmetic through gains and a sum stays well inside this.
constexpr float kLevelTolerance = 1e-4f;

/// @brief A clip long enough that no test here reaches its end by accident.
constexpr std::uint64_t kLongClipFrames = kSampleRate * 2;

std::shared_ptr<const PcmClip> ConstantClip(float level, std::uint64_t frames)
{
    std::shared_ptr<PcmClip> clip = std::make_shared<PcmClip>();
    clip->samples.assign(static_cast<std::size_t>(frames) * kChannelCount, level);
    return clip;
}

std::unique_ptr<Mixer> MakeMixer(const BusLayout &layout)
{
    std::expected<std::unique_ptr<Mixer>, AudioError> mixer = Mixer::Create(layout);
    REQUIRE(mixer.has_value());
    return std::move(*mixer);
}

/// @brief @p frames of the mixer's output, rendered in one call.
std::vector<float> Render(Mixer &mixer, std::uint64_t frames)
{
    std::vector<float> out(static_cast<std::size_t>(frames) * kChannelCount, -1.0f);
    mixer.Render(out);
    return out;
}

/// @brief Render past every volume ramp, so levels read afterwards are settled.
void Settle(Mixer &mixer)
{
    (void)Render(mixer, kVolumeRampFrames * 2);
}

} // namespace

TEST_CASE("Sounds on different buses are scaled by every bus above them and summed")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    mixer->SetBusVolume(ToBusId(DefaultBus::Sfx), 0.5f);
    mixer->SetBusVolume(ToBusId(DefaultBus::Music), 1.0f);
    mixer->SetBusVolume(ToBusId(DefaultBus::Master), 0.5f);

    REQUIRE(mixer->Attach(ConstantClip(0.25f, kLongClipFrames), ToBusId(DefaultBus::Sfx)).has_value());
    REQUIRE(mixer->Attach(ConstantClip(0.25f, kLongClipFrames), ToBusId(DefaultBus::Music)).has_value());
    Settle(*mixer);

    const float expected = 0.25f * 0.5f * 0.5f + 0.25f * 1.0f * 0.5f;
    for (const float sample : Render(*mixer, kVolumeRampFrames))
    {
        CHECK(std::fabs(sample - expected) <= kLevelTolerance);
    }
}

TEST_CASE("A bus the game declares is scaled by its own volume and its parent's")
{
    BusConfig config;
    config.buses.push_back(BusDeclaration{.name = Assisi::Core::InternedString{"Footsteps"},
                                          .parent = Assisi::Core::InternedString{"SFX"},
                                          .volume = 0.5f});
    const std::expected<BusLayout, AudioError> layout = BusLayout::FromConfig(config);
    REQUIRE(layout.has_value());
    const std::optional<BusId> footsteps = layout->FindBus("Footsteps");
    REQUIRE(footsteps.has_value());

    std::unique_ptr<Mixer> mixer = MakeMixer(*layout);
    CHECK(mixer->BusVolume(*footsteps) == doctest::Approx(0.5f));
    mixer->SetBusVolume(ToBusId(DefaultBus::Sfx), 0.5f);
    REQUIRE(mixer->Attach(ConstantClip(1.0f, kLongClipFrames), *footsteps).has_value());
    Settle(*mixer);

    for (const float sample : Render(*mixer, kVolumeRampFrames))
    {
        CHECK(std::fabs(sample - 0.25f) <= kLevelTolerance);
    }
}

TEST_CASE("A bus volume change ramps to its new level instead of jumping")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    REQUIRE(mixer->Attach(ConstantClip(1.0f, kLongClipFrames), ToBusId(DefaultBus::Master)).has_value());
    Settle(*mixer);

    mixer->SetBusVolume(ToBusId(DefaultBus::Master), 0.0f);
    const std::vector<float> ramp = Render(*mixer, kVolumeRampFrames * 2);

    // A linear ramp moves one step of 1/kVolumeRampFrames per frame; a jump is the whole way at once.
    const float largestStep = 1.0f / static_cast<float>(kVolumeRampFrames) + kLevelTolerance;
    // The first frame is a step from the settled level, which rendered as 1.0 before the change.
    CHECK(std::fabs(ramp.front() - 1.0f) <= largestStep);
    for (std::size_t i = kChannelCount; i < ramp.size(); ++i)
    {
        INFO("sample " << i << ": " << ramp[i - kChannelCount] << " -> " << ramp[i]);
        CHECK(std::fabs(ramp[i] - ramp[i - kChannelCount]) <= largestStep);
    }
    CHECK(std::fabs(ramp.back()) <= kLevelTolerance);
}

TEST_CASE("Bus volumes are clamped to the range a slider offers")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    mixer->SetBusVolume(ToBusId(DefaultBus::Music), 3.0f);
    CHECK(mixer->BusVolume(ToBusId(DefaultBus::Music)) == doctest::Approx(kMaxVolume));
    mixer->SetBusVolume(ToBusId(DefaultBus::Music), -1.0f);
    CHECK(mixer->BusVolume(ToBusId(DefaultBus::Music)) == doctest::Approx(kMinVolume));
}

TEST_CASE("A sound finishes when its clip runs out")
{
    constexpr std::uint64_t kClipFrames = 100;
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Attach(ConstantClip(0.5f, kClipFrames), ToBusId(DefaultBus::Sfx));
    REQUIRE(sound.has_value());

    (void)Render(*mixer, kClipFrames / 2);
    mixer->Update();
    CHECK_FALSE(mixer->IsFinished(*sound));

    (void)Render(*mixer, kClipFrames);
    mixer->Update();
    CHECK(mixer->IsFinished(*sound));
}

TEST_CASE("A stopped sound fades to silence and then finishes")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Attach(ConstantClip(1.0f, kLongClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(sound.has_value());
    Settle(*mixer);

    mixer->Stop(*sound);
    const std::vector<float> fade = Render(*mixer, kSoundFadeFrames * 2);
    for (std::size_t i = kChannelCount; i < fade.size(); ++i)
    {
        CHECK(fade[i] <= fade[i - kChannelCount] + kLevelTolerance);
    }
    CHECK(fade.front() > 0.5f);
    CHECK(std::fabs(fade.back()) <= kLevelTolerance);

    mixer->Update();
    CHECK(mixer->IsFinished(*sound));
}

TEST_CASE("A full mixer refuses a sound until a slot is freed, and a reused slot's old handle stays finished")
{
    constexpr std::uint64_t kClipFrames = 16;
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());

    std::vector<SoundHandle> sounds;
    for (std::size_t i = 0; i < kMaxSounds; ++i)
    {
        const std::expected<SoundHandle, AudioError> sound =
            mixer->Attach(ConstantClip(0.0f, kClipFrames), ToBusId(DefaultBus::Sfx));
        REQUIRE(sound.has_value());
        sounds.push_back(*sound);
    }
    const std::expected<SoundHandle, AudioError> overflow =
        mixer->Attach(ConstantClip(0.0f, kClipFrames), ToBusId(DefaultBus::Sfx));
    REQUIRE_FALSE(overflow.has_value());
    CHECK(overflow.error() == AudioError::TooManySounds);

    (void)Render(*mixer, kClipFrames * 2);
    mixer->Update();

    const std::expected<SoundHandle, AudioError> reused =
        mixer->Attach(ConstantClip(0.5f, kLongClipFrames), ToBusId(DefaultBus::Sfx));
    REQUIRE(reused.has_value());
    CHECK_FALSE(mixer->IsFinished(*reused));
    for (const SoundHandle &old : sounds)
    {
        CHECK(mixer->IsFinished(old));
    }
}

TEST_CASE("A claimed sound is silent until played, then starts at the volume it was given")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Claim(ConstantClip(1.0f, kLongClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(sound.has_value());
    mixer->SetVolume(*sound, 0.5f);

    for (const float sample : Render(*mixer, kVolumeRampFrames))
    {
        CHECK(sample == 0.0f);
    }

    mixer->Play(*sound);
    // The very first buffer: a volume set before playing is where the sound starts, not a ramp from full.
    for (const float sample : Render(*mixer, kVolumeRampFrames))
    {
        CHECK(std::fabs(sample - 0.5f) <= kLevelTolerance);
    }
}

TEST_CASE("A playing sound's volume ramps to a new level instead of jumping")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Attach(ConstantClip(1.0f, kLongClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(sound.has_value());
    Settle(*mixer);

    mixer->SetVolume(*sound, 0.25f);
    const std::vector<float> ramp = Render(*mixer, kVolumeRampFrames * 2);
    const float largestStep = 1.0f / static_cast<float>(kVolumeRampFrames) + kLevelTolerance;
    CHECK(ramp.front() > 0.25f + largestStep);
    for (std::size_t i = kChannelCount; i < ramp.size(); ++i)
    {
        CHECK(std::fabs(ramp[i] - ramp[i - kChannelCount]) <= largestStep);
    }
    CHECK(std::fabs(ramp.back() - 0.25f) <= kLevelTolerance);
}

TEST_CASE("A looping sound plays on past its clip's end, and a one-shot does not")
{
    constexpr std::uint64_t kClipFrames = 100;
    constexpr std::uint64_t kRenderedFrames = kClipFrames * 5 / 2;
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());

    const std::expected<SoundHandle, AudioError> loop =
        mixer->Claim(ConstantClip(0.5f, kClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(loop.has_value());
    mixer->SetLooping(*loop, true);
    mixer->Play(*loop);
    for (const float sample : Render(*mixer, kRenderedFrames))
    {
        CHECK(std::fabs(sample - 0.5f) <= kLevelTolerance);
    }
    mixer->Hold(*loop);
    mixer->Update();
    CHECK_FALSE(mixer->IsFinished(*loop));

    mixer->Stop(*loop);
    (void)Render(*mixer, kSoundFadeFrames * 2);
    mixer->Update();
    REQUIRE(mixer->IsFinished(*loop));

    const std::expected<SoundHandle, AudioError> once =
        mixer->Attach(ConstantClip(0.5f, kClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(once.has_value());
    const std::vector<float> rendered = Render(*mixer, kRenderedFrames);
    CHECK(std::fabs(rendered.back()) <= kLevelTolerance);
    mixer->Hold(*once);
    mixer->Update();
    CHECK(mixer->IsFinished(*once));
}

TEST_CASE("A sound nobody holds between two updates fades out, and a held one plays on")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> held =
        mixer->Attach(ConstantClip(0.5f, kLongClipFrames), ToBusId(DefaultBus::Master));
    const std::expected<SoundHandle, AudioError> dropped =
        mixer->Attach(ConstantClip(0.5f, kLongClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(held.has_value());
    REQUIRE(dropped.has_value());

    // Starting a sound holds it until the next update; after that it must be held again.
    mixer->Update();
    mixer->Hold(*held);
    mixer->Update();
    mixer->Hold(*held);
    const std::vector<float> fade = Render(*mixer, kSoundFadeFrames * 2);
    CHECK(std::fabs(fade.back() - 0.5f) <= kLevelTolerance);

    mixer->Update();
    CHECK_FALSE(mixer->IsFinished(*held));
    CHECK(mixer->IsFinished(*dropped));
}

TEST_CASE("A claimed sound that is never played or held gives its slot back")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Claim(ConstantClip(0.5f, kLongClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(sound.has_value());

    mixer->Update();
    CHECK_FALSE(mixer->IsFinished(*sound));
    mixer->Update();
    CHECK(mixer->IsFinished(*sound));
}

TEST_CASE("A paused sound falls silent where it is and resumes from there")
{
    constexpr std::uint64_t kClipFrames = kSoundFadeFrames * 8;
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Attach(ConstantClip(1.0f, kClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(sound.has_value());
    (void)Render(*mixer, kSoundFadeFrames);

    mixer->Pause(*sound);
    const std::vector<float> fade = Render(*mixer, kSoundFadeFrames * 2);
    CHECK(fade.front() > 0.5f);
    CHECK(std::fabs(fade.back()) <= kLevelTolerance);

    // Far longer than what is left of the clip: a sound that kept going while paused would be over.
    for (const float sample : Render(*mixer, kClipFrames * 2))
    {
        CHECK(sample == 0.0f);
    }
    mixer->Hold(*sound);
    mixer->Update();
    CHECK_FALSE(mixer->IsFinished(*sound));

    mixer->Resume(*sound);
    const std::vector<float> resumed = Render(*mixer, kSoundFadeFrames * 2);
    CHECK(std::fabs(resumed.back() - 1.0f) <= kLevelTolerance);
}

TEST_CASE("A sound paused before it is played starts silent")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    const std::expected<SoundHandle, AudioError> sound =
        mixer->Claim(ConstantClip(1.0f, kLongClipFrames), ToBusId(DefaultBus::Master));
    REQUIRE(sound.has_value());
    mixer->Pause(*sound);
    mixer->Play(*sound);

    for (const float sample : Render(*mixer, kVolumeRampFrames))
    {
        CHECK(sample == 0.0f);
    }
    mixer->Resume(*sound);
    const std::vector<float> resumed = Render(*mixer, kSoundFadeFrames * 2);
    CHECK(std::fabs(resumed.back() - 1.0f) <= kLevelTolerance);
}

TEST_CASE("A mixer with nothing attached renders silence")
{
    std::unique_ptr<Mixer> mixer = MakeMixer(BusLayout::Defaults());
    for (const float sample : Render(*mixer, kVolumeRampFrames))
    {
        CHECK(sample == 0.0f);
    }
}
