/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SigilTesting.hpp
/// @brief A vocabulary nothing in the engine uses, and helpers to compile
///        against it from memory.
///
/// "robot" has machines of nodes, a `signal` type and a when-only `tick()`.
/// Every test compiles against it and nothing else, which is what shows the
/// compiler needs no animation words to work.

#include <Assisi/Sigil/Compile/Compile.hpp>
#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Assisi::Sigil::Testing
{

using namespace Assisi::Sigil::Compile;

inline std::expected<void, std::string> ValidateSignal(std::string_view value)
{
    constexpr std::array<std::string_view, 3> kSignals{"beep", "boop", "whirr"};
    if (std::ranges::find(kSignals, value) != kSignals.end())
    {
        return {};
    }
    return std::unexpected(std::format("unknown signal \"{}\"{}", value, DidYouMean(kSignals, value)));
}

inline Vocabulary Robot()
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

/// A second vocabulary, for a file imported across vocabularies.
inline Vocabulary Other()
{
    Vocabulary other;
    other.name = "other";
    return other;
}

/// Source files by path, served to a compile in place of a disk.
using Files = std::unordered_map<std::string, std::string>;

inline SourceReader ReaderOf(const Files &files)
{
    return [&files](std::string_view path) -> std::expected<std::string, std::string> {
        const Files::const_iterator found = files.find(std::string{path});
        if (found == files.end())
        {
            return std::unexpected("no such file");
        }
        return found->second;
    };
}

/// @p source compiled as `main.sgl` against robot and other, with @p files to import.
inline std::expected<Program, Diagnostics> CompileRobot(std::string_view source, const Files &files = {})
{
    const std::array<Vocabulary, 2> vocabularies{Robot(), Other()};
    return CompileSource(source, "main.sgl", vocabularies, ReaderOf(files));
}

/// Every diagnostic, one per line, for a failing check to print.
inline std::string Dump(std::span<const Diagnostic> diagnostics)
{
    std::string text;
    for (const Diagnostic &diagnostic : diagnostics)
    {
        text += Format(diagnostic) + "\n";
    }
    return text;
}

/// Whether an error on @p line of @p file says @p says.
inline bool HasError(std::span<const Diagnostic> diagnostics, uint32_t line, std::string_view says,
                     std::string_view file = "main.sgl")
{
    return std::ranges::any_of(diagnostics, [&](const Diagnostic &diagnostic) {
        return diagnostic.severity == Severity::Error && diagnostic.where.line == line && diagnostic.file == file &&
               diagnostic.message.find(says) != std::string::npos;
    });
}

/// The errors of a compile that was meant to fail; empty if it didn't.
inline Diagnostics Errors(const std::expected<Program, Diagnostics> &compiled)
{
    return compiled ? Diagnostics{} : compiled.error();
}

} // namespace Assisi::Sigil::Testing
