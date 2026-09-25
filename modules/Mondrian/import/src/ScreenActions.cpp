/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include "MarkupValues.hpp"
#include "ScreenAttributes.hpp"
#include "ScreenWalk.hpp"

#include <Assisi/Core/EventCatalog.hpp>

#include <cstddef>
#include <optional>
#include <utility>

namespace Assisi::Mondrian::Import
{
namespace
{

/// One argument of a call, with every parameter in it replaced.
struct Argument
{
    std::string text;
    /// What a node name here is qualified with: where the text was written.
    std::string resolveIn;
};

/// A call as it reads once parameters are replaced.
struct ResolvedCall
{
    std::vector<Argument> arguments;
    std::string name;
};

/// @p attribute's value read as a call, or nullopt when it is a bare name, with
/// any refusal placed where the attribute was written.
std::expected<std::optional<MarkupCall>, MarkupError> ParseCall(const MarkupAttribute &attribute)
{
    std::expected<std::optional<MarkupCall>, std::string> call = SplitCall(attribute.value);
    if (!call)
    {
        return std::unexpected(At(attribute, std::move(call.error())));
    }
    return *call;
}

/// A bare name: an event this build declares. A verb's name written bare is
/// refused, so a verb has one spelling and it is a call.
std::expected<void, MarkupError> ApplyEvent(ScreenNode &node, const MarkupAttribute &attribute,
                                            const Core::EventCatalog &catalog)
{
    if (const std::optional<ScreenVerb> verb = LookUpVerb(attribute.value))
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' is a verb, and a verb is written as a call: " +
                                                 Signature(*verb) + "."));
    }
    if (catalog.Find(attribute.value) == nullptr)
    {
        return std::unexpected(At(attribute, "'" + attribute.value +
                                                 "' names no event this build declares. An event is a struct marked "
                                                 "AEVENT() in a reflected header, named here by its full C++ name; "
                                                 "a verb is a call, one of: " +
                                                 KnownVerbs() + "."));
    }
    node.action = ActionKind::Event;
    node.eventName = attribute.value;
    return {};
}

/// A call: a verb, checked against what it takes. A target is kept by name
/// until the walk is done, since the node it names may not have been read yet.
std::expected<void, MarkupError> ApplyVerb(Walk &walk, uint32_t index, const MarkupAttribute &attribute,
                                           const ResolvedCall &call)
{
    const std::optional<ScreenVerb> verb = LookUpVerb(call.name);
    if (!verb)
    {
        return std::unexpected(
            At(attribute, "'" + call.name + "' is not a verb this markup has. It has: " + KnownVerbs() + "."));
    }

    const std::size_t wanted =
        static_cast<std::size_t>(VerbTakesTarget(*verb)) + static_cast<std::size_t>(VerbTakesMoves(*verb));
    if (call.arguments.size() != wanted)
    {
        return std::unexpected(At(attribute, "'" + attribute.value + "' gives " + call.name + " " +
                                                 std::to_string(call.arguments.size()) + " arguments; it is written " +
                                                 Signature(*verb) + "."));
    }

    ScreenNode &node = walk.document.nodes[index];
    node.action = ActionKind::Verb;
    node.verb = *verb;

    std::size_t next = 0;
    if (VerbTakesTarget(*verb))
    {
        // Qualified here, where the scope it was written in is known: a target
        // inside a template means a part of the same instance, and one an
        // instance passed means a node where the instance sits.
        const Argument &target = call.arguments[next];
        walk.targets.push_back(PendingTarget{.name = QualifyIn(target.resolveIn, target.text),
                                             .node = index,
                                             .line = attribute.line,
                                             .column = attribute.column});
        ++next;
    }
    if (VerbTakesMoves(*verb))
    {
        const std::string_view written = call.arguments[next].text;
        const std::optional<int32_t> moves = ParseInt(written);
        if (!moves)
        {
            return std::unexpected(At(attribute, "'" + std::string{written} + "' is not a whole number of moves."));
        }
        // A move of none swallows the press and changes nothing, which nobody
        // writes on purpose.
        if (*moves == 0)
        {
            return std::unexpected(At(attribute, "'" + attribute.value + "' makes 0 moves, which moves nothing."));
        }
        node.moves = *moves;
    }
    return {};
}

} // namespace

/// A call written out has each argument's parameters replaced on their own, so
/// a target the template writes and one an instance passes each resolve where
/// they were written. A call that is wholly one parameter's value was written
/// where that value was.
std::expected<void, MarkupError> ApplyAction(Walk &walk, uint32_t index, const MarkupAttribute &written)
{
    const std::expected<Substituted, MarkupError> whole = Substitute(walk, written.value, written.line, written.column);
    if (!whole)
    {
        return std::unexpected(whole.error());
    }
    // Checked per instance, once the value is known.
    if (!whole->known)
    {
        return {};
    }
    MarkupAttribute attribute = written;
    attribute.value = whole->text;

    const std::expected<std::optional<MarkupCall>, MarkupError> call = ParseCall(attribute);
    if (!call)
    {
        return std::unexpected(call.error());
    }
    if (!call->has_value())
    {
        return ApplyEvent(walk.document.nodes[index], attribute, walk.catalog);
    }

    ResolvedCall resolved{.arguments = {}, .name = std::string{(*call)->name}};
    const std::expected<std::optional<MarkupCall>, MarkupError> raw = ParseCall(written);
    if (!raw)
    {
        return std::unexpected(raw.error());
    }
    if (raw->has_value() && (*raw)->arguments.size() == (*call)->arguments.size())
    {
        for (const std::string_view argument : (*raw)->arguments)
        {
            const std::expected<Substituted, MarkupError> one =
                Substitute(walk, argument, written.line, written.column);
            if (!one)
            {
                return std::unexpected(one.error());
            }
            resolved.arguments.push_back(
                Argument{.text = one->text,
                         .resolveIn = one->sole != nullptr ? one->sole->resolveIn : std::string{CurrentPrefix(walk)}});
        }
    }
    else
    {
        const std::string resolveIn =
            whole->sole != nullptr ? whole->sole->resolveIn : std::string{CurrentPrefix(walk)};
        for (const std::string_view argument : (*call)->arguments)
        {
            resolved.arguments.push_back(Argument{.text = std::string{argument}, .resolveIn = resolveIn});
        }
    }
    return ApplyVerb(walk, index, attribute, resolved);
}

std::expected<void, MarkupError> ResolveTargets(Walk &walk)
{
    for (const PendingTarget &pending : walk.targets)
    {
        const MarkupError where{.message = {}, .file = {}, .line = pending.line, .column = pending.column};
        const std::unordered_map<std::string, NamedNode>::const_iterator named = walk.names.find(pending.name);
        if (named == walk.names.end())
        {
            MarkupError error = where;
            error.message = "'" + pending.name + "' names no node on this screen.";
            return std::unexpected(std::move(error));
        }

        ScreenNode &node = walk.document.nodes[pending.node];
        if (!VerbActsOn(node.verb, walk.document.nodes[named->second.index].widget))
        {
            MarkupError error = where;
            error.message = "'" + pending.name + "' is not a slider, and " + Signature(node.verb) + " moves a slider.";
            return std::unexpected(std::move(error));
        }
        node.target = named->second.index;
    }
    return {};
}

} // namespace Assisi::Mondrian::Import
