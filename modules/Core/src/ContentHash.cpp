/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/ContentHash.hpp>

#include <array>
#include <fstream>

namespace Assisi::Core
{

std::optional<std::uint64_t> HashTextFileNormalized(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return std::nullopt;

    // Read through the stream rather than an istreambuf_iterator pair. The
    // iterator drives the streambuf directly, with nothing between a failing
    // `underflow` and the caller: libstdc++ lets its exception escape the string
    // constructor, so a path naming a directory took the process down, and the
    // state check that stood here was dead either way — the iterator sets
    // neither eofbit nor failbit, so a short read hashed what it got and called
    // it a success. `read` runs the same failure through its sentry, which
    // records badbit and returns.
    std::string                 bytes;
    std::array<char, 64 * 1024> chunk{};
    while (file.read(chunk.data(), static_cast<std::streamsize>(chunk.size())) || file.gcount() > 0)
        bytes.append(chunk.data(), static_cast<std::size_t>(file.gcount()));

    // Only badbit says the read failed. Reaching the end sets eofbit and failbit
    // on the last short read, which is how this loop is meant to end.
    if (file.bad())
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
