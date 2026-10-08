/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Render/SkinScratch.hpp>

namespace Assisi::Render
{

std::optional<uint32_t> SkinScratch::Reuse(uint32_t vertexCount, uint32_t owner)
{
    for (Range &range : _ranges)
    {
        if (range.owner == kFree && range.vertexCount == vertexCount)
        {
            range.owner = owner;
            range.claimed = true;
            return range.vertexBase;
        }
    }
    return std::nullopt;
}

void SkinScratch::Add(uint32_t vertexBase, uint32_t vertexCount, uint32_t owner)
{
    _rangeAt[vertexBase] = static_cast<uint32_t>(_ranges.size());
    _ranges.push_back(Range{.vertexBase = vertexBase, .vertexCount = vertexCount, .owner = owner, .claimed = true});
}

bool SkinScratch::Claim(uint32_t vertexBase, uint32_t owner)
{
    const std::unordered_map<uint32_t, uint32_t>::const_iterator found = _rangeAt.find(vertexBase);
    if (found == _rangeAt.end())
    {
        return false;
    }
    Range &range = _ranges[found->second];
    if (range.owner != owner || range.claimed)
    {
        return false;
    }
    range.claimed = true;
    return true;
}

void SkinScratch::EndFrame()
{
    for (Range &range : _ranges)
    {
        if (!range.claimed)
        {
            range.owner = kFree;
        }
        range.claimed = false;
    }
}

void SkinScratch::Clear()
{
    _ranges.clear();
    _rangeAt.clear();
}

} // namespace Assisi::Render
