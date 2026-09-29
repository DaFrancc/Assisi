/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Fixtures.hpp
/// @brief Reads the encoded sound files in tests/fixtures, for the formats a test
/// cannot build in memory the way WavBuilder.hpp builds a WAV.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <vector>

namespace Assisi::Audio::Testing
{

/// @brief The bytes of fixtures/@p name, or empty if it cannot be read.
inline std::vector<std::byte> ReadFixture(std::string_view name)
{
    const std::filesystem::path path = std::filesystem::path{ASSISI_AUDIO_FIXTURE_DIR} / name;
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return {};
    }
    const std::vector<char> chars{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    const std::byte *first = reinterpret_cast<const std::byte *>(chars.data());
    return std::vector<std::byte>{first, first + chars.size()};
}

} // namespace Assisi::Audio::Testing
