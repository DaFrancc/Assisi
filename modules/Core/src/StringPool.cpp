/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/StringPool.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{

PooledString StringPool::Add(std::string_view text)
{
    if (text.size() > kMaxPoolBytes - _bytes.size())
    {
        ASSISI_ASSERT(false, "a string pool is full");
        Log::Error("StringPool: adding {} bytes would pass the {}-byte limit; the string is dropped", text.size(),
                   kMaxPoolBytes);
        return {};
    }
    const PooledString handle{.offset = static_cast<std::uint32_t>(_bytes.size()),
                              .length = static_cast<std::uint32_t>(text.size())};
    _bytes.append(text);
    return handle;
}

std::string_view StringPool::View(PooledString handle) const
{
    // Compared as sizes rather than as offset + length, which could wrap.
    if (handle.offset > _bytes.size() || handle.length > _bytes.size() - handle.offset)
    {
        return {};
    }
    return std::string_view{_bytes}.substr(handle.offset, handle.length);
}

bool StringPool::Assign(std::string_view bytes)
{
    if (bytes.size() > kMaxPoolBytes)
    {
        return false;
    }
    _bytes.assign(bytes);
    return true;
}

} // namespace Assisi::Core
