/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/ContentHash.hpp>

#include <fstream>
#include <iterator>

namespace Assisi::Core
{

std::optional<std::uint64_t> HashTextFileNormalized(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt;

    std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof())
        return std::nullopt;

    // In place, keeping every byte except a CR that precedes an LF. A trailing
    // lone CR is kept: it is not a line ending either translation produces.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n')
            continue;
        bytes[kept++] = bytes[i];
    }
    bytes.resize(kept);

    return ContentHash64(std::as_bytes(std::span{bytes.data(), bytes.size()}));
}

} // namespace Assisi::Core
