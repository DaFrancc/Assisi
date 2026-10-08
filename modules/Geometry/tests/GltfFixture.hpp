/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file GltfFixture.hpp
/// @brief Builds a small `.gltf` and its `.bin` in a fresh asset root, for the
///        tests that import one.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <Assisi/Core/AssetSystem.hpp>

namespace GltfFixture
{

/// glTF accessor component types.
inline constexpr uint32_t kFloat = 5126;
inline constexpr uint32_t kUnsignedByte = 5121;
inline constexpr uint32_t kUnsignedShort = 5123;

/// glTF requires every accessor to start on a multiple of its component size;
/// four covers every type these fixtures use.
inline constexpr std::size_t kViewAlignment = 4;

inline uint32_t ComponentsOf(std::string_view type)
{
    if (type == "MAT4")
    {
        return 16;
    }
    if (type == "VEC4")
    {
        return 4;
    }
    if (type == "VEC3")
    {
        return 3;
    }
    return 1;
}

/// The binary buffer of a fixture glTF and the accessors over it, built in the
/// order a test adds them so the test can name each accessor by the index Add
/// returns.
class GltfBuffer
{
public:
    template <typename T>
    uint32_t Add(const std::vector<T> &values, uint32_t componentType, std::string_view type,
                 std::string_view extraKeys = {})
    {
        while (_bytes.size() % kViewAlignment != 0)
        {
            _bytes.push_back(std::byte{0});
        }
        const std::size_t offset = _bytes.size();
        const std::size_t length = values.size() * sizeof(T);
        _bytes.resize(offset + length);
        std::memcpy(_bytes.data() + offset, values.data(), length);

        const uint32_t index = _accessorCount++;
        const char *separator = index == 0 ? "" : ",";
        _views += std::format(R"({}{{"buffer":0,"byteOffset":{},"byteLength":{}}})", separator, offset, length);
        _accessors += std::format(R"({}{{"bufferView":{},"componentType":{},"count":{},"type":"{}"{}}})", separator,
                                  index, componentType, values.size() / ComponentsOf(type), type, extraKeys);
        return index;
    }

    /// Writes `<stem>.gltf` and `<stem>.bin` into a fresh asset root named
    /// @p rootName under the temp directory, and makes it the asset root.
    /// @p sceneKeys holds the "scenes", "nodes" and whatever else the file has.
    std::filesystem::path Write(std::string_view rootName, std::string_view stem, std::string_view sceneKeys) const
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / rootName;
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        WriteInto(root, stem, sceneKeys);
        REQUIRE(Assisi::Core::AssetSystem::SetRoot(root).has_value());
        return root;
    }

    /// Writes `<stem>.gltf` and `<stem>.bin` into @p root, leaving it as it is.
    void WriteInto(const std::filesystem::path &root, std::string_view stem, std::string_view sceneKeys) const
    {
        const std::string gltf =
            std::format(R"({{"asset":{{"version":"2.0"}},"scene":0,{},"buffers":[{{"uri":"{}.bin","byteLength":{}}}],)"
                        R"("bufferViews":[{}],"accessors":[{}]}})",
                        sceneKeys, stem, _bytes.size(), _views, _accessors);
        {
            std::ofstream file(root / std::format("{}.gltf", stem), std::ios::binary);
            file.write(gltf.data(), static_cast<std::streamsize>(gltf.size()));
        }
        {
            std::ofstream file(root / std::format("{}.bin", stem), std::ios::binary);
            file.write(reinterpret_cast<const char *>(_bytes.data()), static_cast<std::streamsize>(_bytes.size()));
        }
    }

private:
    std::vector<std::byte> _bytes;
    std::string _views;
    std::string _accessors;
    uint32_t _accessorCount = 0;
};

} // namespace GltfFixture
