/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Checker.hpp"

#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <format>
#include <utility>

namespace Assisi::Sigil::Compile::Detail
{

namespace
{

using StateList = std::span<const Block>;

/// One state leading to another: through a transition, or a clause naming it.
struct Edge
{
    uint32_t from = 0;
    uint32_t to = 0;
};

/// Where a clause is written: in a block of a kind, or in a transition.
struct Placement
{
    std::string_view kind;
    bool transition = false;
};

std::optional<uint32_t> FindKind(const Vocabulary &vocabulary, std::string_view name)
{
    for (std::size_t i = 0; i < vocabulary.blocks.size(); ++i)
    {
        if (vocabulary.blocks[i].name == name)
        {
            return static_cast<uint32_t>(i);
        }
    }
    return std::nullopt;
}

std::vector<std::string_view> StateNames(StateList states)
{
    std::vector<std::string_view> names;
    for (const Block &state : states)
    {
        names.push_back(state.name);
    }
    return names;
}

std::optional<uint32_t> FindState(StateList states, std::string_view name)
{
    for (std::size_t i = 0; i < states.size(); ++i)
    {
        if (states[i].name == name)
        {
            return static_cast<uint32_t>(i);
        }
    }
    return std::nullopt;
}

/// The state @p name names among @p states, or nothing, reported against @p owner.
std::optional<uint32_t> ResolveState(Checker &checker, const Syntax::Named &name, StateList states,
                                     std::string_view owner)
{
    const std::optional<uint32_t> found = FindState(states, name.name);
    if (!found.has_value())
    {
        Fail(checker, name.where,
             std::format("unknown state \"{}\" in {}{}", name.name, owner, DidYouMean(StateNames(states), name.name)));
    }
    return found;
}

bool PlacedHere(const ClauseSpec &spec, Placement placement)
{
    if (placement.transition)
    {
        return spec.onTransition;
    }
    return std::ranges::find(spec.blocks, placement.kind) != spec.blocks.end();
}

std::string PlacementName(Placement placement)
{
    return placement.transition ? std::string{"a transition"} : std::format("a {}", placement.kind);
}

std::optional<Argument> CheckArgument(Checker &checker, const Syntax::Expr &syntax, const ArgumentSpec &spec,
                                      StateList states)
{
    if (spec.kind == ArgumentKind::State)
    {
        if (syntax.kind != Syntax::ExprKind::Name)
        {
            Fail(checker, syntax.where, "expected the name of a state here",
                 "write a state's name on its own, without quotes");
            return std::nullopt;
        }
        const std::optional<uint32_t> state = ResolveState(
            checker, Syntax::Named{.name = syntax.text, .where = syntax.where}, states, "this block");
        if (!state.has_value())
        {
            return std::nullopt;
        }
        return Argument{.value = {}, .state = static_cast<int32_t>(*state)};
    }
    Expr value = CheckExpression(checker, syntax, ExprMode::Formula);
    if (value.type.kind == TypeKind::Error)
    {
        return std::nullopt;
    }
    if (ReadsWhenOnly(checker, value))
    {
        Fail(checker, syntax.where, "this reads a trigger, which can only decide transitions",
             "a trigger, or a let or function that reads one, only belongs in a transition's \"when\"");
        return std::nullopt;
    }
    if (spec.type == TypeNames::kNumeric)
    {
        if (value.type.kind != TypeKind::Int && value.type.kind != TypeKind::Float)
        {
            Fail(checker, syntax.where, std::format("expected a number, got {}", TypeName(checker, value.type)));
            return std::nullopt;
        }
        return Argument{.value = std::move(value), .state = -1};
    }
    const std::optional<Type> wanted = ResolveType(checker, Syntax::Named{.name = spec.type, .where = syntax.where});
    value = Coerce(checker, std::move(value), wanted.value_or(Type{}));
    if (value.type.kind == TypeKind::Error)
    {
        return std::nullopt;
    }
    return Argument{.value = std::move(value), .state = -1};
}

std::optional<Clause> CheckClause(Checker &checker, const Syntax::Clause &syntax, Placement placement,
                                  StateList states)
{
    const std::vector<ClauseSpec> &specs = checker.vocabulary.clauses;
    const std::vector<ClauseSpec>::const_iterator spec = std::ranges::find_if(
        specs, [&syntax](const ClauseSpec &candidate) { return candidate.word == syntax.word.name; });
    if (spec == specs.end())
    {
        std::vector<std::string_view> words;
        for (const ClauseSpec &candidate : specs)
        {
            words.push_back(candidate.word);
        }
        Fail(checker, syntax.word.where,
             std::format("unknown clause \"{}\"{}", syntax.word.name, DidYouMean(words, syntax.word.name)));
        return std::nullopt;
    }
    if (!PlacedHere(*spec, placement))
    {
        Fail(checker, syntax.word.where,
             std::format("\"{}\" can't go in {}", syntax.word.name, PlacementName(placement)));
        return std::nullopt;
    }
    if (syntax.arguments.size() != spec->arguments.size())
    {
        Fail(checker, syntax.word.where,
             std::format("\"{}\" takes {} values, got {}", syntax.word.name, spec->arguments.size(),
                         syntax.arguments.size()));
        return std::nullopt;
    }
    Clause clause{.arguments = {}, .where = syntax.word.where,
                  .spec = static_cast<uint32_t>(spec - specs.begin())};
    for (std::size_t i = 0; i < syntax.arguments.size(); ++i)
    {
        std::optional<Argument> argument = CheckArgument(checker, syntax.arguments[i], spec->arguments[i], states);
        if (!argument.has_value())
        {
            return std::nullopt;
        }
        clause.arguments.push_back(std::move(*argument));
    }
    return clause;
}

/// Each clause is written no more often than its spec allows, and a clause
/// that must be written once is, in what @p owner names.
void CheckCardinality(Checker &checker, std::span<const Clause> clauses, Placement placement,
                      const Syntax::Named &owner)
{
    const std::vector<ClauseSpec> &specs = checker.vocabulary.clauses;
    std::vector<uint32_t> counts(specs.size(), 0);
    for (const Clause &clause : clauses)
    {
        const ClauseSpec &spec = specs[clause.spec];
        ++counts[clause.spec];
        if (counts[clause.spec] == 2 && spec.cardinality != Cardinality::Any)
        {
            Fail(checker, clause.where,
                 std::format("\"{}\" can only be written once in {}", spec.word, PlacementName(placement)));
        }
    }
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        if (specs[i].cardinality == Cardinality::ExactlyOnce && counts[i] == 0 && PlacedHere(specs[i], placement))
        {
            Fail(checker, owner.where, std::format("{} \"{}\" needs a \"{}\" clause", placement.kind, owner.name,
                                                   specs[i].word));
        }
    }
}

/// Checks the clauses of a block, whose State arguments name its siblings in
/// @p states, adding the edges they make from state @p self.
std::vector<Clause> CheckBlockClauses(Checker &checker, const Syntax::Block &syntax, StateList states, uint32_t self,
                                      std::vector<Edge> &edges)
{
    const Placement placement{.kind = syntax.kind.name, .transition = false};
    std::vector<Clause> clauses;
    for (const Syntax::Clause &clauseSyntax : syntax.clauses)
    {
        std::optional<Clause> clause = CheckClause(checker, clauseSyntax, placement, states);
        if (!clause.has_value())
        {
            continue;
        }
        for (const Argument &argument : clause->arguments)
        {
            if (argument.state >= 0)
            {
                edges.push_back(Edge{.from = self, .to = static_cast<uint32_t>(argument.state)});
            }
        }
        clauses.push_back(std::move(*clause));
    }
    CheckCardinality(checker, clauses, placement, syntax.name);
    return clauses;
}

/// The states a transition leaves: its first source, or every state but its
/// target for `any`, then each step added or removed in turn.
std::optional<std::vector<bool>> SourceSet(Checker &checker, const Syntax::Transition &syntax, StateList states,
                                           uint32_t target, std::string_view owner)
{
    std::vector<bool> set(states.size(), false);
    if (syntax.fromAny)
    {
        std::ranges::fill(set, true);
        set[target] = false;
    }
    else
    {
        const std::optional<uint32_t> first = ResolveState(checker, syntax.first, states, owner);
        if (!first.has_value())
        {
            return std::nullopt;
        }
        set[*first] = true;
    }
    for (const Syntax::SourceStep &step : syntax.steps)
    {
        const std::optional<uint32_t> state = ResolveState(checker, step.state, states, owner);
        if (!state.has_value())
        {
            return std::nullopt;
        }
        if (step.remove != set[*state])
        {
            Fail(checker, step.state.where,
                 step.remove ? std::format("\"{}\" isn't in the set it's removed from", step.state.name)
                             : std::format("\"{}\" is already in the set", step.state.name));
            return std::nullopt;
        }
        set[*state] = !step.remove;
    }
    return set;
}

/// Checks a transition's condition and clauses into @p transition.
void CheckTransitionBody(Checker &checker, const Syntax::Transition &syntax, StateList states,
                         Transition &transition)
{
    transition.condition = CheckExpression(checker, syntax.condition, ExprMode::Formula);
    const TypeKind conditionKind = transition.condition.type.kind;
    if (conditionKind != TypeKind::Bool && conditionKind != TypeKind::Trigger && conditionKind != TypeKind::Error)
    {
        Fail(checker, syntax.condition.where,
             std::format("a transition's condition must be a bool, got {}", TypeName(checker, transition.condition.type)),
             "compare it to something, like speed > 1.0");
    }
    CollectTriggers(checker, transition.condition, transition.triggersRead);
    const Placement placement{.kind = {}, .transition = true};
    for (const Syntax::Clause &clauseSyntax : syntax.clauses)
    {
        std::optional<Clause> clause = CheckClause(checker, clauseSyntax, placement, states);
        if (clause.has_value())
        {
            transition.clauses.push_back(std::move(*clause));
        }
    }
    CheckCardinality(checker, transition.clauses, placement, syntax.target);
}

std::optional<Transition> CheckTransition(Checker &checker, const Syntax::Transition &syntax, StateList states,
                                          std::string_view owner)
{
    Transition transition;
    transition.where = syntax.first.where;
    // The condition and clauses are checked whatever is wrong with the states,
    // so their own mistakes are reported in the same compile.
    CheckTransitionBody(checker, syntax, states, transition);
    const std::optional<uint32_t> target = ResolveState(checker, syntax.target, states, owner);
    if (!target.has_value())
    {
        return std::nullopt;
    }
    const std::optional<std::vector<bool>> set = SourceSet(checker, syntax, states, *target, owner);
    if (!set.has_value())
    {
        return std::nullopt;
    }
    transition.target = *target;
    for (std::size_t i = 0; i < set->size(); ++i)
    {
        if ((*set)[i])
        {
            transition.sources.push_back(static_cast<uint32_t>(i));
        }
    }
    if (transition.sources.empty())
    {
        Fail(checker, syntax.first.where, "this transition leaves from no state",
             "removing states left none for it to leave from");
        return std::nullopt;
    }
    const bool restarts = std::ranges::find(transition.sources, *target) != transition.sources.end();
    if (restarts && transition.sources.size() > 1)
    {
        Fail(checker, syntax.first.where,
             std::format("this transition goes to \"{}\", so it can't also leave from \"{}\"", syntax.target.name,
                         syntax.target.name),
             std::format("to restart \"{}\" while it's playing, write \"{} -> {}\" as a transition of its own",
                         syntax.target.name, syntax.target.name, syntax.target.name));
        return std::nullopt;
    }
    return transition;
}

/// Every state is reached from the first, through transitions and clauses
/// that name states. @p kind is the block's kind, for the message.
void CheckReachable(Checker &checker, const Block &block, std::span<const Edge> edges, std::string_view kind)
{
    if (block.children.empty())
    {
        return;
    }
    std::vector<bool> reached(block.children.size(), false);
    std::deque<uint32_t> frontier{0};
    reached[0] = true;
    while (!frontier.empty())
    {
        const uint32_t from = frontier.front();
        frontier.pop_front();
        for (const Edge &edge : edges)
        {
            if (edge.from == from && !reached[edge.to])
            {
                reached[edge.to] = true;
                frontier.push_back(edge.to);
            }
        }
    }
    for (std::size_t i = 0; i < reached.size(); ++i)
    {
        if (!reached[i])
        {
            Fail(checker, block.children[i].where,
                 std::format("\"{}\" is unreachable from the \"{}\" {}", block.children[i].name, block.name, kind),
                 std::format("no transition or clause leads to it from \"{}\", where the {} starts",
                             block.children.front().name, kind));
        }
    }
}

bool Allowed(const BlockKind *parent, const BlockKind &child)
{
    if (parent == nullptr)
    {
        return child.topLevel;
    }
    return std::ranges::find(child.parents, parent->name) != child.parents.end();
}

Block CheckBlock(Checker &checker, const Syntax::Block &syntax, const BlockKind *kind, uint32_t kindIndex);

/// Checks @p syntax's child blocks into @p block, keeping which syntax each
/// came from in @p childSyntax.
void CheckChildren(Checker &checker, const Syntax::Block &syntax, const BlockKind *kind, Block &block,
                   std::vector<const Syntax::Block *> &childSyntax)
{
    for (const Syntax::Block &child : syntax.blocks)
    {
        const std::optional<uint32_t> childKind = FindKind(checker.vocabulary, child.kind.name);
        if (!childKind.has_value())
        {
            std::vector<std::string_view> names;
            for (const BlockKind &candidate : checker.vocabulary.blocks)
            {
                names.push_back(candidate.name);
            }
            Fail(checker, child.kind.where,
                 std::format("unknown block kind \"{}\"{}", child.kind.name, DidYouMean(names, child.kind.name)));
            continue;
        }
        const BlockKind &childSpec = checker.vocabulary.blocks[*childKind];
        if (checker.program.library)
        {
            Fail(checker, child.kind.where, "a library can only hold imports, enums and consts, not blocks",
                 "remove \"sigiltype library;\" to make this an ordinary file");
            continue;
        }
        if (!Allowed(kind, childSpec))
        {
            Fail(checker, child.kind.where,
                 kind == nullptr ? std::format("a {} can't go at the top level of the file", childSpec.name)
                                 : std::format("a {} can't go in a {}", childSpec.name, kind->name));
            continue;
        }
        if (IsReserved(checker, child.name))
        {
            continue;
        }
        const std::optional<uint32_t> existing = FindState(block.children, child.name.name);
        if (existing.has_value())
        {
            Fail(checker, child.name.where,
                 std::format("there is already a {} \"{}\" here, on line {}", childSpec.name, child.name.name,
                             block.children[*existing].where.line));
            continue;
        }
        block.children.push_back(CheckBlock(checker, child, &childSpec, *childKind));
        childSyntax.push_back(&child);
    }
}

Block CheckBlock(Checker &checker, const Syntax::Block &syntax, const BlockKind *kind, uint32_t kindIndex)
{
    Block block;
    block.name = syntax.name.name;
    block.where = syntax.name.where;
    block.kind = kindIndex;
    std::vector<const Syntax::Block *> childSyntax;
    CheckChildren(checker, syntax, kind, block, childSyntax);

    // A child's clauses name its siblings, so they are checked once every
    // child is known.
    std::vector<Edge> edges;
    for (std::size_t i = 0; i < block.children.size(); ++i)
    {
        block.children[i].clauses =
            CheckBlockClauses(checker, *childSyntax[i], block.children, static_cast<uint32_t>(i), edges);
    }
    if (kind == nullptr)
    {
        for (const Syntax::Clause &clause : syntax.clauses)
        {
            Fail(checker, clause.word.where, "clauses go inside a block, not at the top level of the file");
        }
    }

    const std::string owner = kind == nullptr ? std::string{"the file"}
                                              : std::format("{} \"{}\"", kind->name, block.name);
    if (!syntax.transitions.empty() && (kind == nullptr || !kind->holdsStates))
    {
        Fail(checker, syntax.transitions.front().first.where,
             std::format("transitions can't go in {}, which holds no states", owner));
        return block;
    }
    for (const Syntax::Transition &transitionSyntax : syntax.transitions)
    {
        std::optional<Transition> transition = CheckTransition(checker, transitionSyntax, block.children, owner);
        if (!transition.has_value())
        {
            continue;
        }
        for (const uint32_t source : transition->sources)
        {
            edges.push_back(Edge{.from = source, .to = transition->target});
        }
        block.transitions.push_back(std::move(*transition));
    }
    if (kind != nullptr && kind->holdsStates)
    {
        CheckReachable(checker, block, edges, kind->name);
    }
    return block;
}

} // namespace

void CheckBlocks(Checker &checker, const Syntax::File &file)
{
    checker.program.root = CheckBlock(checker, file.root, nullptr, 0);
}

} // namespace Assisi::Sigil::Compile::Detail
