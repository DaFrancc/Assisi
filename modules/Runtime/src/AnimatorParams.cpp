/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/AnimatorParams.hpp>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/Math/Interpolate.hpp>
#include <Assisi/Sigil/Bytecode.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>

namespace Assisi::Runtime
{
namespace
{

bool IsTrigger(const AnimatorGraph &graph, uint32_t slot)
{
    return std::ranges::find(graph.triggerSlots, slot) != graph.triggerSlots.end();
}

/// How a file declares a param of @p type, for a message.
std::string_view TypeName(Sigil::SlotType type, bool trigger)
{
    switch (type)
    {
    case Sigil::SlotType::Float:
        return "float";
    case Sigil::SlotType::Int:
        return "int";
    case Sigil::SlotType::Bool:
        return trigger ? "trigger" : "bool";
    case Sigil::SlotType::None:
    case Sigil::SlotType::Count_:
        break;
    }
    return "clip";
}

/// "a" or "an", for @p typeName in a message.
std::string_view Article(std::string_view typeName)
{
    return typeName == "int" ? "an" : "a";
}

/// Whether @p a and @p b are the same file, compared by owner rather than
/// address.
bool SameFile(const std::weak_ptr<const AnimatorGraph> &a, const std::shared_ptr<const AnimatorGraph> &b)
{
    return !a.owner_before(b) && !b.owner_before(a);
}

template <AnimatorValue T> Sigil::SlotType SlotTypeOf()
{
    if constexpr (std::same_as<T, float>)
    {
        return Sigil::SlotType::Float;
    }
    else if constexpr (std::same_as<T, int32_t>)
    {
        return Sigil::SlotType::Int;
    }
    else
    {
        return Sigil::SlotType::Bool;
    }
}

template <AnimatorValue T> Sigil::Word ToWord(T value)
{
    if constexpr (std::same_as<T, float>)
    {
        return Sigil::FromFloat(value);
    }
    else if constexpr (std::same_as<T, int32_t>)
    {
        return Sigil::FromInt(value);
    }
    else
    {
        return Sigil::FromBool(value);
    }
}

template <AnimatorValue T> T FromWord(Sigil::Word word)
{
    if constexpr (std::same_as<T, float>)
    {
        return Sigil::ToFloat(word);
    }
    else if constexpr (std::same_as<T, int32_t>)
    {
        return Sigil::ToInt(word);
    }
    else
    {
        return Sigil::ToBool(word);
    }
}

} // namespace

AnimatorParamSlot::AnimatorParamSlot(std::string name, Sigil::SlotType type, bool trigger)
    : _name(std::move(name)), _type(type), _trigger(trigger)
{
}

std::optional<uint32_t> AnimatorParamSlot::Find(ECS::Entity entity, const AnimatorRun &run)
{
    const std::optional<uint32_t> slot = FindQuietly(run);
    if (!slot.has_value() && run.graph != nullptr)
    {
        Report(entity, run.graph);
    }
    return slot;
}

std::optional<uint32_t> AnimatorParamSlot::FindQuietly(const AnimatorRun &run)
{
    if (run.graph == nullptr)
    {
        return std::nullopt;
    }
    Resolve(run.graph);
    if (!_slot.has_value() || *_slot >= run.block.size())
    {
        return std::nullopt;
    }
    return _slot;
}

void AnimatorParamSlot::Resolve(const std::shared_ptr<const AnimatorGraph> &graph)
{
    if (_lookups > 0 && SameFile(_graph, graph))
    {
        return;
    }
    ++_lookups;
    _graph = graph;
    const std::expected<uint32_t, Sigil::ParamMiss> found = Sigil::FindParam(graph->layout, _name, _type);
    if (!found.has_value())
    {
        _slot.reset();
        _miss = found.error();
        return;
    }
    // A trigger and a bool are both bools in the block; each is set its own way.
    if (IsTrigger(*graph, *found) != _trigger)
    {
        _slot.reset();
        _miss = Sigil::ParamMiss::OtherType;
        return;
    }
    _slot = *found;
}

void AnimatorParamSlot::Report(ECS::Entity entity, const std::shared_ptr<const AnimatorGraph> &graph)
{
    std::erase_if(_reported, [](const std::weak_ptr<const AnimatorGraph> &file) { return file.expired(); });
    if (std::ranges::any_of(_reported,
                            [&graph](const std::weak_ptr<const AnimatorGraph> &file) { return SameFile(file, graph); }))
    {
        return;
    }
    _reported.push_back(graph);

    const std::string_view wanted = TypeName(_type, _trigger);
    switch (_miss)
    {
    case Sigil::ParamMiss::Missing:
        Core::Log::Warn("Animator: entity (index {}, gen {}) has no {} param \"{}\" in its file, so setting it does "
                        "nothing.",
                        entity.index, entity.generation, wanted, _name);
        return;
    case Sigil::ParamMiss::NotParam:
        Core::Log::Warn("Animator: \"{}\" in the file of entity (index {}, gen {}) is not a param, so code can't set "
                        "it.",
                        _name, entity.index, entity.generation);
        return;
    case Sigil::ParamMiss::OtherType:
    case Sigil::ParamMiss::Count_:
        break;
    }
    const std::optional<uint32_t> slot = Sigil::FindSlot(graph->layout, _name);
    const std::string_view declared =
        slot.has_value() ? TypeName(graph->layout.slots[*slot].type, IsTrigger(*graph, *slot)) : "param";
    Core::Log::Warn("Animator: param \"{}\" in the file of entity (index {}, gen {}) is {} {}, not {} {}, so setting "
                    "it does nothing.",
                    _name, entity.index, entity.generation, Article(declared), declared, Article(wanted), wanted);
}

template <AnimatorValue T>
AnimatorParam<T>::AnimatorParam(std::string name) : _slot(std::move(name), SlotTypeOf<T>(), /*trigger=*/ false)
{
}

template <AnimatorValue T> bool AnimatorParam<T>::Set(ECS::Entity entity, AnimatorRun &run, T value)
{
    const std::optional<uint32_t> slot = _slot.Find(entity, run);
    if (!slot.has_value())
    {
        return false;
    }
    run.block[*slot] = ToWord(value);
    return true;
}

template <AnimatorValue T>
bool AnimatorParam<T>::SetDamped(ECS::Entity entity, AnimatorRun &run, float target, float seconds, float dt)
requires std::same_as<T, float>
{
    const std::optional<uint32_t> slot = _slot.Find(entity, run);
    if (!slot.has_value())
    {
        return false;
    }
    const float current = Sigil::ToFloat(run.block[*slot]);
    run.block[*slot] = Sigil::FromFloat(Math::Damp(current, target, seconds, dt));
    return true;
}

template <AnimatorValue T> std::optional<T> AnimatorParam<T>::Get(const AnimatorRun &run)
{
    const std::optional<uint32_t> slot = _slot.FindQuietly(run);
    if (!slot.has_value())
    {
        return std::nullopt;
    }
    return FromWord<T>(run.block[*slot]);
}

template class AnimatorParam<float>;
template class AnimatorParam<int32_t>;
template class AnimatorParam<bool>;

AnimatorTrigger::AnimatorTrigger(std::string name) : _slot(std::move(name), Sigil::SlotType::Bool, /*trigger=*/ true)
{
}

bool AnimatorTrigger::Fire(ECS::Entity entity, AnimatorRun &run)
{
    const std::optional<uint32_t> slot = _slot.Find(entity, run);
    if (!slot.has_value())
    {
        return false;
    }
    run.block[*slot] = Sigil::FromBool(true);
    return true;
}

bool AnimatorTrigger::FireOnChange(ECS::Entity entity, AnimatorRun &run, uint32_t count)
{
    const std::optional<uint32_t> slot = _slot.Find(entity, run);
    if (!slot.has_value())
    {
        return false;
    }
    if (run.counts.size() < run.block.size())
    {
        run.counts.resize(run.block.size());
    }
    std::optional<uint32_t> &seen = run.counts[*slot];
    const bool changed = seen.has_value() && *seen != count;
    seen = count;
    if (changed)
    {
        run.block[*slot] = Sigil::FromBool(true);
    }
    return changed;
}

} // namespace Assisi::Runtime
