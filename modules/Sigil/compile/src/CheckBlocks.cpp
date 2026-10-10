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

/// Where a clause is written: in a block of a kind, in a transition, or at the
/// top of the file.
struct Placement
{
    std::string_view kind;
    bool transition = false;
    bool file = false;
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

/// The span of state @p state's name.
Span StateSpan(const Block &state)
{
    return Span{.where = state.where, .length = static_cast<uint32_t>(std::max<std::size_t>(state.name.size(), 1))};
}

/// The state @p name names among @p states, or nothing, reported against @p owner.
std::optional<uint32_t> ResolveState(Checker &checker, const Syntax::Named &name, StateList states,
                                     std::string_view owner)
{
    const std::optional<uint32_t> found = FindState(states, name.name);
    if (!found.has_value())
    {
        const std::optional<std::string_view> closest = ClosestName(StateNames(states), name.name);
        Fail(checker, name.Extent(), std::format("unknown state \"{}\" in {}", name.name, owner),
             closest.has_value() ? std::format("did you mean \"{}\"?", *closest) : std::string{"no such state here"});
    }
    return found;
}

bool PlacedHere(const ClauseSpec &spec, Placement placement)
{
    if (placement.file)
    {
        return spec.fileLevel;
    }
    if (placement.transition)
    {
        return spec.onTransition;
    }
    return std::ranges::find(spec.blocks, placement.kind) != spec.blocks.end();
}

std::string PlacementName(Placement placement)
{
    if (placement.file)
    {
        return "the file";
    }
    return placement.transition ? std::string{"a transition"} : std::format("a {}", placement.kind);
}

/// Where @p spec may be written, for the help when it's written elsewhere.
std::string AllowedPlaces(const ClauseSpec &spec)
{
    if (spec.fileLevel)
    {
        return "the top of the file, outside any block";
    }
    std::vector<std::string> places;
    for (const std::string &block : spec.blocks)
    {
        places.push_back(std::format("a {}", block));
    }
    if (spec.onTransition)
    {
        places.emplace_back("a transition's braces");
    }
    std::string text;
    for (std::size_t i = 0; i < places.size(); ++i)
    {
        text += i == 0 ? "" : (i + 1 == places.size() ? " or " : ", ");
        text += places[i];
    }
    return text.empty() ? std::string{"nowhere"} : text;
}

std::optional<Argument> CheckArgument(Checker &checker, const Syntax::Expr &syntax, const ArgumentSpec &spec,
                                      const Syntax::Named &word, StateList states)
{
    if (spec.kind == ArgumentKind::State)
    {
        if (syntax.kind != Syntax::ExprKind::Name)
        {
            Diagnostic &error = Fail(checker, syntax.extent, "expected the name of a state", "not a state's name");
            error.help = std::format("\"{}\" names a state: write its name on its own, without quotes", word.name);
            return std::nullopt;
        }
        const std::optional<uint32_t> state = ResolveState(
            checker, Syntax::Named{.name = syntax.text, .where = syntax.extent.where}, states, "this block");
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
        Diagnostic &error =
            Fail(checker, value.span, "a trigger can only decide transitions", "this reads a trigger");
        Relate(error, word.Extent(), std::format("used as a value of \"{}\"", word.name));
        error.help = "a trigger, or a let or function that reads one, only belongs in a transition's \"when\"";
        return std::nullopt;
    }
    if (spec.type == TypeNames::kNumeric)
    {
        if (value.type.kind != TypeKind::Int && value.type.kind != TypeKind::Float)
        {
            Diagnostic &error = Fail(checker, value.span, "mismatched types",
                                     std::format("expected a number, found {}", TypeName(checker, value.type)));
            Relate(error, word.Extent(), std::format("\"{}\" takes a number", word.name));
            return std::nullopt;
        }
        return Argument{.value = std::move(value), .state = -1};
    }
    const std::optional<Type> wanted =
        ResolveType(checker, Syntax::Named{.name = spec.type, .where = syntax.extent.where});
    const std::string_view article = spec.type.starts_with('i') || spec.type.starts_with('a') ? "an" : "a";
    const Reason because{.span = word.Extent(),
                         .label = std::format("\"{}\" takes {} {}", word.name, article, spec.type)};
    value = Coerce(checker, std::move(value), wanted.value_or(Type{}), because);
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
        const std::optional<std::string_view> closest = ClosestName(words, syntax.word.name);
        Fail(checker, syntax.word.Extent(), std::format("unknown clause \"{}\"", syntax.word.name),
             closest.has_value() ? std::format("did you mean \"{}\"?", *closest)
                                 : std::format("the {} vocabulary has no such clause", checker.vocabulary.name));
        return std::nullopt;
    }
    if (!PlacedHere(*spec, placement))
    {
        Diagnostic &error = Fail(checker, syntax.word.Extent(),
                                 std::format("\"{}\" can't go in {}", syntax.word.name, PlacementName(placement)),
                                 "not allowed here");
        error.help = std::format("\"{}\" goes in {}", syntax.word.name, AllowedPlaces(*spec));
        return std::nullopt;
    }
    if (syntax.arguments.size() != spec->arguments.size())
    {
        Fail(checker, syntax.word.Extent(),
             std::format("\"{}\" takes {} values, got {}", syntax.word.name, spec->arguments.size(),
                         syntax.arguments.size()),
             std::format("expected {} values", spec->arguments.size()));
        return std::nullopt;
    }
    Clause clause{.arguments = {}, .where = syntax.word.where, .spec = static_cast<uint32_t>(spec - specs.begin())};
    for (std::size_t i = 0; i < syntax.arguments.size(); ++i)
    {
        std::optional<Argument> argument =
            CheckArgument(checker, syntax.arguments[i], spec->arguments[i], syntax.word, states);
        if (!argument.has_value())
        {
            return std::nullopt;
        }
        clause.arguments.push_back(std::move(*argument));
    }
    return clause;
}

/// Each clause is written no more often than its spec allows, and a clause
/// that must be written once is, in the block or transition @p owner names.
void CheckCardinality(Checker &checker, std::span<const Clause> clauses, Placement placement,
                      const Syntax::Named &owner)
{
    const std::vector<ClauseSpec> &specs = checker.vocabulary.clauses;
    std::vector<const Clause *> first(specs.size(), nullptr);
    for (const Clause &clause : clauses)
    {
        const ClauseSpec &spec = specs[clause.spec];
        const Span span{.where = clause.where, .length = static_cast<uint32_t>(spec.word.size())};
        if (first[clause.spec] == nullptr)
        {
            first[clause.spec] = &clause;
            continue;
        }
        if (spec.cardinality != Cardinality::Any)
        {
            Diagnostic &error =
                Fail(checker, span, std::format("\"{}\" is written twice in {}", spec.word, PlacementName(placement)),
                     "written again here");
            Relate(error, Span{.where = first[clause.spec]->where, .length = span.length}, "first written here");
            error.help = std::format("{} takes one \"{}\"", PlacementName(placement), spec.word);
        }
    }
    for (std::size_t i = 0; i < specs.size(); ++i)
    {
        if (specs[i].cardinality != Cardinality::ExactlyOnce || first[i] != nullptr || !PlacedHere(specs[i], placement))
        {
            continue;
        }
        if (placement.file)
        {
            Diagnostic &error = Fail(checker, owner.Extent(),
                                     std::format("the file has no \"{}\" clause", specs[i].word),
                                     std::format("needs a \"{}\" clause", specs[i].word));
            error.help = std::format("write one \"{}\" at the top of the file, before any block", specs[i].word);
            continue;
        }
        Diagnostic &error = Fail(checker, owner.Extent(),
                                 std::format("{} \"{}\" has no \"{}\" clause", placement.kind, owner.name,
                                             specs[i].word),
                                 std::format("needs a \"{}\" clause", specs[i].word));
        error.help = std::format("every {} needs exactly one \"{}\"", placement.kind, specs[i].word);
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
            Diagnostic &error =
                Fail(checker, step.state.Extent(),
                     step.remove ? std::format("\"{}\" isn't in the set it's removed from", step.state.name)
                                 : std::format("\"{}\" is already in the set", step.state.name),
                     step.remove ? "removed here" : "added again here");
            Relate(error, syntax.first.Extent(), "the set starts here");
            return std::nullopt;
        }
        set[*state] = !step.remove;
    }
    return set;
}

/// Checks a transition's condition and clauses into @p transition.
void CheckTransitionBody(Checker &checker, const Syntax::Transition &syntax, StateList states, Transition &transition)
{
    transition.condition = CheckExpression(checker, syntax.condition, ExprMode::Formula);
    const TypeKind conditionKind = transition.condition.type.kind;
    if (conditionKind != TypeKind::Bool && conditionKind != TypeKind::Trigger && conditionKind != TypeKind::Error)
    {
        Diagnostic &error = Fail(checker, transition.condition.span, "a condition must be a bool",
                                 std::format("this is {}", TypeName(checker, transition.condition.type)));
        error.help = "compare it to something, like speed > 1.0";
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

/// The source part of a transition, `first + a - b`, for pointing at.
Span SourcesSpan(const Syntax::Transition &syntax)
{
    const Syntax::Named &last = syntax.steps.empty() ? syntax.first : syntax.steps.back().state;
    const bool oneLine = last.where.line == syntax.first.where.line;
    const uint32_t end = last.where.column + static_cast<uint32_t>(last.name.size());
    return Span{.where = syntax.first.where,
                .length = oneLine ? end - syntax.first.where.column : syntax.first.Extent().length};
}

/// The source part of @p syntax written without the steps that add its
/// target, or nothing when that leaves no first state to start from.
std::optional<std::string> SourcesWithoutTarget(const Syntax::Transition &syntax)
{
    const std::string &target = syntax.target.name;
    std::vector<const Syntax::SourceStep *> steps;
    for (const Syntax::SourceStep &step : syntax.steps)
    {
        if (step.remove || step.state.name != target)
        {
            steps.push_back(&step);
        }
    }
    std::string first = syntax.first.name;
    if (!syntax.fromAny && first == target)
    {
        const std::vector<const Syntax::SourceStep *>::iterator added =
            std::ranges::find_if(steps, [](const Syntax::SourceStep *step) { return !step->remove; });
        if (added == steps.end())
        {
            return std::nullopt;
        }
        first = (*added)->state.name;
        steps.erase(added);
    }
    std::string text = first;
    for (const Syntax::SourceStep *step : steps)
    {
        text += std::format(" {} {}", step->remove ? '-' : '+', step->state.name);
    }
    return text;
}

/// Splits a transition that leaves from its own target into one that
/// doesn't and one that restarts it, when the transition is a line of its own.
std::optional<Suggestion> SplitRestart(const Checker &checker, const Syntax::Transition &syntax)
{
    const Span sources = SourcesSpan(syntax);
    const std::string_view line = LineOf(checker, sources.where.line);
    const std::size_t sourcesEnd = sources.where.column - 1 + sources.length;
    const std::optional<std::string> kept = SourcesWithoutTarget(syntax);
    if (!kept.has_value() || sourcesEnd > line.size())
    {
        return std::nullopt;
    }
    const std::string_view rest = line.substr(sourcesEnd);
    if (!rest.ends_with(';'))
    {
        return std::nullopt;
    }
    const std::string_view indent = line.substr(0, sources.where.column - 1);
    return Suggestion{
        .message = std::format("leave \"{}\" out of the set, and restart it in a transition of its own",
                               syntax.target.name),
        .edits = {Edit{.text = *kept, .line = std::string{line}, .span = sources, .kind = EditKind::Replace},
                  Edit{.text = std::format("{}{}{}", indent, syntax.target.name, rest),
                       .line = std::string{line},
                       .span = sources,
                       .kind = EditKind::InsertAfter}}};
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
        Diagnostic &error =
            Fail(checker, SourcesSpan(syntax), "this transition leaves from no state", "these are no states");
        error.help = "removing states left none for it to leave from";
        return std::nullopt;
    }
    const bool restarts = std::ranges::find(transition.sources, *target) != transition.sources.end();
    if (restarts && transition.sources.size() > 1)
    {
        Diagnostic &error = Fail(checker, SourcesSpan(syntax),
                                 std::format("this transition goes to \"{}\", so it can't also leave from \"{}\"",
                                             syntax.target.name, syntax.target.name),
                                 std::format("these include \"{}\"", syntax.target.name));
        Relate(error, syntax.target.Extent(), std::format("and it goes to \"{}\"", syntax.target.name));
        std::optional<Suggestion> split = SplitRestart(checker, syntax);
        if (split.has_value())
        {
            error.suggestions.push_back(std::move(*split));
        }
        else
        {
            error.help =
                std::format("to restart \"{}\" while it's playing, write \"{} -> {}\" as a transition of its own",
                            syntax.target.name, syntax.target.name, syntax.target.name);
        }
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
            Diagnostic &error = Fail(checker, StateSpan(block.children[i]),
                                     std::format("\"{}\" is unreachable from the \"{}\" {}", block.children[i].name,
                                                 block.name, kind),
                                     "never reached");
            Relate(error, StateSpan(block.children.front()), std::format("the {} starts here", kind));
            error.help = "no transition or clause leads to it from the first state; add a transition to it";
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

/// Where @p kind may be written, for the help when it's written elsewhere.
std::string AllowedParents(const BlockKind &kind)
{
    std::vector<std::string> places;
    if (kind.topLevel)
    {
        places.emplace_back("the top level of the file");
    }
    for (const std::string &parent : kind.parents)
    {
        places.push_back(std::format("a {}", parent));
    }
    std::string text;
    for (std::size_t i = 0; i < places.size(); ++i)
    {
        text += i == 0 ? "" : (i + 1 == places.size() ? " or " : ", ");
        text += places[i];
    }
    return text;
}

Block CheckBlock(Checker &checker, const Syntax::Block &syntax, const BlockKind *kind, uint32_t kindIndex);

/// Whether @p child can be checked as one of @p block's states, reporting why not.
bool AcceptChild(Checker &checker, const Syntax::Block &child, const BlockKind *kind, const Block &block,
                 std::optional<uint32_t> childKind)
{
    if (!childKind.has_value())
    {
        std::vector<std::string_view> names;
        for (const BlockKind &candidate : checker.vocabulary.blocks)
        {
            names.push_back(candidate.name);
        }
        const std::optional<std::string_view> closest = ClosestName(names, child.kind.name);
        Fail(checker, child.kind.Extent(), std::format("unknown block kind \"{}\"", child.kind.name),
             closest.has_value() ? std::format("did you mean \"{}\"?", *closest)
                                 : std::format("the {} vocabulary has no such block", checker.vocabulary.name));
        return false;
    }
    const BlockKind &childSpec = checker.vocabulary.blocks[*childKind];
    if (checker.program.library)
    {
        Diagnostic &error = Fail(checker, child.kind.Extent(), "a library can't hold blocks", "not allowed in a library");
        error.help = "a library holds only imports, enums and consts; remove \"sigiltype library;\" to make this an "
                     "ordinary file";
        return false;
    }
    if (!Allowed(kind, childSpec))
    {
        Diagnostic &error = Fail(checker, child.kind.Extent(),
                                 kind == nullptr ? std::format("a {} can't go at the top level of the file", childSpec.name)
                                                 : std::format("a {} can't go in a {}", childSpec.name, kind->name),
                                 "not allowed here");
        error.help = std::format("a {} goes in {}", childSpec.name, AllowedParents(childSpec));
        return false;
    }
    if (IsReserved(checker, child.name))
    {
        return false;
    }
    const std::optional<uint32_t> existing = FindState(block.children, child.name.name);
    if (existing.has_value())
    {
        Diagnostic &error = Fail(checker, child.name.Extent(),
                                 std::format("there are two {}s called \"{}\" here", childSpec.name, child.name.name),
                                 "declared again here");
        Relate(error, StateSpan(block.children[*existing]), "first declared here");
        error.help = "states in one block need different names";
        return false;
    }
    return true;
}

/// Checks @p syntax's child blocks into @p block, keeping which syntax each
/// came from in @p childSyntax.
void CheckChildren(Checker &checker, const Syntax::Block &syntax, const BlockKind *kind, Block &block,
                   std::vector<const Syntax::Block *> &childSyntax)
{
    for (const Syntax::Block &child : syntax.blocks)
    {
        const std::optional<uint32_t> childKind = FindKind(checker.vocabulary, child.kind.name);
        if (!AcceptChild(checker, child, kind, block, childKind))
        {
            continue;
        }
        block.children.push_back(CheckBlock(checker, child, &checker.vocabulary.blocks[*childKind], *childKind));
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
    const std::string owner = kind == nullptr ? std::string{"the file"}
                                              : std::format("{} \"{}\"", kind->name, block.name);
    if (!syntax.transitions.empty() && (kind == nullptr || !kind->holdsStates))
    {
        Diagnostic &error = Fail(checker, SourcesSpan(syntax.transitions.front()),
                                 std::format("transitions can't go in {}", owner), "not allowed here");
        error.help = kind == nullptr ? std::string{"transitions go inside a block that holds states"}
                                     : std::format("a {} holds no states to move between", kind->name);
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

/// The clauses written at the top of @p file: those the vocabulary marks
/// file-level, which a library, holding only enums and consts, takes none of.
std::vector<Clause> CheckFileClauses(Checker &checker, const Syntax::File &file)
{
    const Placement placement{.kind = {}, .transition = false, .file = true};
    std::vector<Clause> clauses;
    for (const Syntax::Clause &syntax : file.root.clauses)
    {
        const std::vector<ClauseSpec>::const_iterator spec =
            std::ranges::find_if(checker.vocabulary.clauses,
                                 [&syntax](const ClauseSpec &candidate) { return candidate.word == syntax.word.name; });
        // An unknown word is checked here too, so CheckClause suggests the closest.
        const bool belongsHere = spec == checker.vocabulary.clauses.end() || spec->fileLevel;
        if (checker.program.library && belongsHere)
        {
            Diagnostic &error =
                Fail(checker, syntax.word.Extent(), "a library can't hold clauses", "not allowed in a library");
            error.help = "a library holds only imports, enums and consts; write it in the files that import it";
            continue;
        }
        if (!belongsHere)
        {
            Diagnostic &error =
                Fail(checker, syntax.word.Extent(), "clauses go inside a block", "at the top level of the file");
            error.help = "move it into the block it belongs to";
            continue;
        }
        std::optional<Clause> clause = CheckClause(checker, syntax, placement, {});
        if (clause.has_value())
        {
            clauses.push_back(std::move(*clause));
        }
    }
    if (!checker.program.library)
    {
        CheckCardinality(checker, clauses, placement, file.use.vocabulary);
    }
    return clauses;
}

} // namespace

void CheckBlocks(Checker &checker, const Syntax::File &file)
{
    checker.program.root = CheckBlock(checker, file.root, nullptr, 0);
    checker.program.root.clauses = CheckFileClauses(checker, file);
}

} // namespace Assisi::Sigil::Compile::Detail
