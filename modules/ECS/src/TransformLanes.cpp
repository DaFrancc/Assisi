/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/ECS/Transform.hpp>

namespace Assisi::ECS
{

void SparseSetLanes<Transform>::Clear()
{
    _world.clear();
    _marks.clear();
    _current.clear();
    _ended.clear();
    _redrawAll = false;
}

void SparseSetLanes<Transform>::RecordPrevious(BlendMark &mark, Entity entity, const Transform &current)
{
    mark = BlendMark{.step = _step, .entry = static_cast<uint32_t>(_current.size())};
    BlendEntry &entry = _current.emplace_back();
    entry.prevRotation = current.rotation;
    entry.prevPosition = current.position;
    entry.prevScale = current.scale;
    entry.entity = entity;
}

void SparseSetLanes<Transform>::RecordEnd(BlendEntry &entry, const Transform &current)
{
    entry.endRotation = current.rotation;
    entry.endPosition = current.position;
    entry.endScale = current.scale;
    entry.hasEnd = true;
}

void SparseSetLanes<Transform>::Advance()
{
    for (const BlendEntry &entry : _current)
    {
        _ended.push_back(entry.entity);
    }
    _current.clear();
    if (_ended.size() > _marks.size())
    {
        _ended.clear();
        _redrawAll = true;
    }
    ++_step;
    if (_step == 0)
    {
        ++_step; // 0 means "no entry"; a mark from before the wrap is stale anyway
    }
}

void SparseSetLanes<Transform>::BeginFixedStep()
{
    Advance();
    _inFixedStep = true;
}

void SparseSetLanes<Transform>::EndFixedStep()
{
    _inFixedStep = false;
}

} // namespace Assisi::ECS
