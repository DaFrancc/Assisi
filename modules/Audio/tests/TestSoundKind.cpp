/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestSoundKind.cpp
/// @brief Sounds are an asset kind: new sound files are given it, the cook
/// refuses a file that would not play and ships the rest as authored, and a
/// cooked sound loads by id in the background into a clip the mixer plays.

#include "Fixtures.hpp"
#include "WavBuilder.hpp"

#include <Assisi/Audio/AudioError.hpp>
#include <Assisi/Audio/AudioFormat.hpp>
#include <Assisi/Audio/BusConfig.hpp>
#include <Assisi/Audio/BusLayout.hpp>
#include <Assisi/Audio/Clip.hpp>
#include <Assisi/Audio/Mixer.hpp>
#include <Assisi/Audio/SoundAsset.hpp>
#include <Assisi/Core/AssetId.hpp>
#include <Assisi/Core/AssetKind.hpp>
#include <Assisi/Core/AssetProvider.hpp>
#include <Assisi/Core/AssetStore.hpp>
#include <Assisi/Core/JobSystem.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

using namespace Assisi;

namespace
{

/// @brief Enough workers that a load really runs off the main thread.
constexpr std::uint32_t kWorkers = 2;

/// @brief Frames in the WAV built for these tests: short, and not a multiple of
/// any decoder's chunk size.
constexpr std::size_t kWavFrames = 1000;

/// @brief A sample value in the built WAV, loud enough to survive conversion.
constexpr std::int16_t kWavLevel = 1000;

const Core::AssetId kSoundId = Core::DerivedAssetId("sounds/tone.ogg");

/// @brief Cooked blobs by id, held in memory: a package without the file.
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

const Core::AssetKind &SoundKind()
{
    const Core::AssetKind *kind = Core::AssetKindRegistry::Instance().Find(Audio::kSoundKind);
    REQUIRE(kind != nullptr);
    return *kind;
}

const Core::AssetCookStep &SoundCookStep()
{
    const Core::AssetCookStep *step = Core::AssetKindRegistry::Instance().CookStepFor(Audio::kSoundKind);
    REQUIRE(step != nullptr);
    return *step;
}

std::vector<std::byte> BuiltWav()
{
    const std::vector<std::int16_t> samples(kWavFrames, kWavLevel);
    return Audio::Testing::BuildWav(samples, Audio::kSampleRate, 1);
}

} // namespace

TEST_CASE("Sound is a registered kind that loads a PcmClip")
{
    const Core::AssetKind &kind = SoundKind();
    CHECK(kind.name == Audio::kSoundKindName);
    CHECK(kind.valueType == std::type_index{typeid(Audio::PcmClip)});
}

TEST_CASE("A new .wav, .flac or .ogg file is given the sound kind")
{
    const Core::AssetKindRegistry &registry = Core::AssetKindRegistry::Instance();
    for (const std::string_view extension : {".wav", ".flac", ".ogg"})
    {
        CAPTURE(extension);
        const Core::AssetKind *kind = registry.KindForNewFile(extension);
        REQUIRE(kind != nullptr);
        CHECK(kind->id == Audio::kSoundKind);
    }
}

TEST_CASE("The sound cook step ships a sound that decodes exactly as authored")
{
    const Core::AssetCookStep &step = SoundCookStep();
    const std::vector<std::vector<std::byte>> sources{BuiltWav(), Audio::Testing::ReadFixture("tone.flac"),
                                                      Audio::Testing::ReadFixture("tone.ogg")};
    for (const std::vector<std::byte> &source : sources)
    {
        REQUIRE_FALSE(source.empty());
        const std::expected<std::vector<std::byte>, Core::AssetError> cooked = step.cook(source);
        REQUIRE(cooked.has_value());
        CHECK(*cooked == source);
    }
}

TEST_CASE("The sound cook step refuses a file that would not play")
{
    const Core::AssetCookStep &step = SoundCookStep();

    const std::vector<std::byte> mp3 = Audio::Testing::ReadFixture("tone.mp3");
    REQUIRE_FALSE(mp3.empty());
    const std::expected<std::vector<std::byte>, Core::AssetError> fromMp3 = step.cook(mp3);
    REQUIRE_FALSE(fromMp3.has_value());
    // A valid file this build cannot read, not a broken one.
    CHECK(fromMp3.error() == Core::AssetErrorCode::UnsupportedEncoding);
    CHECK_FALSE(fromMp3.error().detail.empty());

    const std::string_view text = "not a sound at all";
    const std::byte *first = reinterpret_cast<const std::byte *>(text.data());
    const std::vector<std::byte> garbage{first, first + text.size()};
    CHECK_FALSE(step.cook(garbage).has_value());
}

TEST_CASE("A decoder's error becomes the asset error that says what went wrong")
{
    CHECK(Audio::ToAssetError(Audio::AudioError::UnsupportedEncoding) == Core::AssetErrorCode::UnsupportedEncoding);
    const Core::AssetError broken = Audio::ToAssetError(Audio::AudioError::DecodeFailed);
    CHECK(broken == Core::AssetErrorCode::CorruptAsset);
    CHECK(broken.detail == Audio::ToString(Audio::AudioError::DecodeFailed));
}

TEST_CASE("A cooked sound loads by id in the background and plays on the mixer")
{
    const std::vector<std::byte> source = Audio::Testing::ReadFixture("tone.ogg");
    REQUIRE_FALSE(source.empty());
    const std::expected<Audio::PcmClip, Audio::AudioError> decoded = Audio::DecodeClip(source);
    REQUIRE(decoded.has_value());

    std::expected<std::vector<std::byte>, Core::AssetError> blob = Core::CookAssetBytes(SoundKind(), source);
    REQUIRE(blob.has_value());
    MemoryPackage package;
    package.Add(kSoundId, std::move(*blob));

    Core::JobSystem jobs(kWorkers);
    Core::AssetStore store;
    store.Initialize(jobs, package);

    CHECK(store.Resolve<Audio::PcmClip>(kSoundId) == nullptr);
    jobs.HelpUntil([&store] { return !store.HasPendingLoads(); }, true);

    const std::shared_ptr<const Audio::PcmClip> clip = store.Resolve<Audio::PcmClip>(kSoundId);
    REQUIRE(clip != nullptr);
    CHECK(clip->Frames() == decoded->Frames());

    std::expected<std::unique_ptr<Audio::Mixer>, Audio::AudioError> mixer =
        Audio::Mixer::Create(Audio::BusLayout::Defaults());
    REQUIRE(mixer.has_value());
    CHECK((*mixer)->Attach(clip, Audio::ToBusId(Audio::DefaultBus::Sfx)).has_value());
}
