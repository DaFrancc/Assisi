/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Vocabularies.hpp"

#include <Assisi/Runtime/Import/AnimatorCompiler.hpp>
#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <array>
#include <expected>
#include <format>
#include <string>
#include <string_view>

namespace Assisi::Sglc
{

namespace
{

using namespace Sigil::Compile;

std::expected<void, std::string> ValidateSignal(std::string_view value)
{
    constexpr std::array<std::string_view, 3> kSignals{"beep", "boop", "whirr"};
    if (std::ranges::find(kSignals, value) != kSignals.end())
    {
        return {};
    }
    return std::unexpected(std::format("unknown signal \"{}\"{}", value, DidYouMean(kSignals, value)));
}

/// A small made-up vocabulary for trying the language before a real one
/// exists: machines of nodes that emit signals.
Vocabulary Robot()
{
    Vocabulary robot;
    robot.name = "robot";
    robot.types = {ValueType{.name = "signal", .validate = ValidateSignal}};
    robot.blocks = {
        BlockKind{.name = "machine", .parents = {}, .topLevel = true, .holdsStates = true},
        BlockKind{.name = "node", .parents = {"machine", "node"}, .topLevel = false, .holdsStates = true},
        BlockKind{.name = "dock", .parents = {}, .topLevel = true, .holdsStates = false},
    };
    const ArgumentSpec state{.type = {}, .kind = ArgumentKind::State};
    robot.clauses = {
        ClauseSpec{.word = "emit", .arguments = {{"signal"}}, .blocks = {"node"}, .onTransition = false,
                   .cardinality = Cardinality::Any},
        ClauseSpec{.word = "goto", .arguments = {state}, .blocks = {"node"}, .onTransition = false,
                   .cardinality = Cardinality::AtMostOnce},
        ClauseSpec{.word = "cost", .arguments = {{"int"}}, .blocks = {"node"}, .onTransition = false,
                   .cardinality = Cardinality::AtMostOnce},
        ClauseSpec{.word = "enabled", .arguments = {{"bool"}}, .blocks = {"node"}, .onTransition = false,
                   .cardinality = Cardinality::AtMostOnce},
        ClauseSpec{.word = "speed", .arguments = {{"float"}}, .blocks = {"machine"}, .onTransition = false,
                   .cardinality = Cardinality::AtMostOnce},
        ClauseSpec{.word = "after", .arguments = {{"float"}}, .blocks = {}, .onTransition = true,
                   .cardinality = Cardinality::AtMostOnce},
        ClauseSpec{.word = "port", .arguments = {{"int"}}, .blocks = {"dock"}, .onTransition = false,
                   .cardinality = Cardinality::ExactlyOnce},
    };
    robot.functions = {
        FunctionSpec{.name = "tick", .parameters = {}, .result = "int", .use = FunctionUse::WhenOnly},
        FunctionSpec{.name = "battery", .parameters = {}, .result = "float", .use = FunctionUse::Anywhere},
    };
    return robot;
}

} // namespace

std::vector<Sigil::Compile::Vocabulary> Vocabularies()
{
    // sglc has no asset tree, so animation names are checked only for being
    // written; the cook checks them against the tree.
    return {Robot(), Runtime::Import::AnimationVocabulary(nullptr)};
}

} // namespace Assisi::Sglc
