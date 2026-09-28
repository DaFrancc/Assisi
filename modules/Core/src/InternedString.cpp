/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/InternedString.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>
#include <Assisi/Core/StringHash.hpp>

namespace Assisi::Core
{
namespace
{

/// Entries per chunk. A chunk is allocated whole when the one before it fills,
/// so this trades a little unused space for few allocations.
constexpr std::uint32_t kChunkEntries = 4096;

/// Chunks the table can ever have; together with kChunkEntries this is the
/// table's capacity.
constexpr std::uint32_t kMaxChunks = kMaxInternedStrings / kChunkEntries;
static_assert(kMaxChunks * kChunkEntries == kMaxInternedStrings,
              "the intern table's capacity must be a whole number of chunks");

/// Bytes per block of text storage. A text longer than this gets a block of its
/// own size, so there is no length limit.
constexpr std::size_t kTextBlockBytes = std::size_t{64} * 1024;

using Chunk = std::array<std::string_view, kChunkEntries>;

/// The table behind every InternedString.
///
/// Readers go index → chunk → entry with no lock. That is safe because nothing a
/// reader can reach ever moves or changes: chunk pointers live in a fixed array,
/// chunks and text blocks are never freed, and an entry is written before the
/// release store of `count` that makes its index valid.
class InternTable
{
public:
    InternTable()
    {
        // Index 0 is the empty name, so a default-constructed InternedString needs
        // no table access and means the same thing in every run.
        _chunks[0].store(new Chunk{}, std::memory_order_relaxed);
        _indices.emplace(std::string_view{}, 0u);
        _count.store(1, std::memory_order_release);
    }

    InternTable(const InternTable &)            = delete;
    InternTable &operator=(const InternTable &) = delete;

    ~InternTable()
    {
        for (std::atomic<Chunk *> &chunk : _chunks)
        {
            delete chunk.load(std::memory_order_relaxed);
        }
    }

    std::uint32_t Intern(std::string_view text)
    {
        const std::lock_guard<std::mutex> lock(_mutex);

        // Looked up under the same lock that appends, so two threads interning the
        // same text for the first time agree on one index.
        const std::unordered_map<std::string_view, std::uint32_t, TransparentStringHash,
                                 std::equal_to<>>::const_iterator found = _indices.find(text);
        if (found != _indices.end())
        {
            return found->second;
        }

        const std::uint32_t index = _count.load(std::memory_order_relaxed);
        if (index >= kMaxInternedStrings)
        {
            ASSISI_ASSERT(false, "the intern table is full");
            Log::Error("InternedString: the table is full at {} names; '{}' becomes the empty name",
                       kMaxInternedStrings, text);
            return 0;
        }

        const std::uint32_t chunkIndex = index / kChunkEntries;
        Chunk *chunk                   = _chunks[chunkIndex].load(std::memory_order_relaxed);
        if (chunk == nullptr)
        {
            chunk = new Chunk{};
            _chunks[chunkIndex].store(chunk, std::memory_order_release);
        }

        const std::string_view stored = Store(text);
        (*chunk)[index % kChunkEntries] = stored;
        _indices.emplace(stored, index);
        _count.store(index + 1, std::memory_order_release);
        return index;
    }

    std::string_view View(std::uint32_t index) const
    {
        if (index >= _count.load(std::memory_order_acquire))
        {
            return {};
        }
        const Chunk *chunk = _chunks[index / kChunkEntries].load(std::memory_order_acquire);
        return (*chunk)[index % kChunkEntries];
    }

    std::uint32_t Count() const { return _count.load(std::memory_order_acquire); }

private:
    /// Copies @p text into block storage that never moves, and returns the copy.
    std::string_view Store(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        if (_blocks.empty() || _blockUsed + text.size() > _blockSize)
        {
            _blockSize = std::max(kTextBlockBytes, text.size());
            _blocks.push_back(std::make_unique<char[]>(_blockSize));
            _blockUsed = 0;
        }
        char *destination = _blocks.back().get() + _blockUsed;
        std::memcpy(destination, text.data(), text.size());
        _blockUsed += text.size();
        return {destination, text.size()};
    }

    std::array<std::atomic<Chunk *>, kMaxChunks> _chunks{};
    std::unordered_map<std::string_view, std::uint32_t, TransparentStringHash, std::equal_to<>> _indices;
    std::vector<std::unique_ptr<char[]>> _blocks;
    std::mutex _mutex;
    std::size_t _blockSize = 0;
    std::size_t _blockUsed = 0;
    std::atomic<std::uint32_t> _count{0};
};

InternTable &Table()
{
    static InternTable table;
    return table;
}

} // namespace

InternedString::InternedString(std::string_view text) : _index(text.empty() ? 0u : Table().Intern(text)) {}

std::string_view InternedString::View() const
{
    return _index == 0 ? std::string_view{} : Table().View(_index);
}

std::uint32_t InternedStringCount()
{
    return Table().Count();
}

} // namespace Assisi::Core
