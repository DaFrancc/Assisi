/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimatorCompile.cpp
/// @brief A `.sgl` file compiled into the graph an Animator runs: layers,
/// nested states and what each plays, checked against the real UAL1 model and
/// clips, every mistake refused where it is written, and the cooked payload
/// read back only when every part of it is safe to run.

#include "AnimatorTesting.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace Assisi;
using namespace Assisi::Runtime::Testing;

namespace
{

std::expected<Runtime::AnimatorGraph, std::string> Compile(std::string_view source)
{
    const TestTree tree;
    return Runtime::Import::CompileAnimator(source, tree);
}

/// @p body after the header every file here starts with.
std::string File(std::string_view body)
{
    return std::string{"use animation;\nskeleton \"quaternius/UAL1/UAL1.glb\";\n"} + std::string{body};
}

/// Whether compiling @p source fails with @p says, pointing at @p line.
bool Refuses(std::string_view source, uint32_t line, std::string_view says)
{
    const std::expected<Runtime::AnimatorGraph, std::string> compiled = Compile(source);
    if (compiled.has_value())
    {
        MESSAGE("compiled, but should have been refused");
        return false;
    }
    const std::string where = std::format("characters/hero.sgl:{}:", line);
    const bool found =
        compiled.error().find(says) != std::string::npos && compiled.error().find(where) != std::string::npos;
    if (!found)
    {
        MESSAGE(compiled.error());
    }
    return found;
}

} // namespace

TEST_CASE("Animator compile: the vocabulary is one Sigil can use")
{
    CHECK(Sigil::Compile::CheckVocabulary(Runtime::Import::AnimationVocabulary(nullptr)).has_value());
}

TEST_CASE("Animator compile: the book's character compiles into its layers, states and fades")
{
    const std::expected<Runtime::AnimatorGraph, std::string> compiled = Compile(kCharacter);
    REQUIRE_MESSAGE(compiled.has_value(), compiled.error());
    const Runtime::AnimatorGraph &graph = *compiled;
    CHECK(graph.skeleton == Core::DerivedAssetId(kModel));
    CHECK(graph.triggerSlots.size() == 1);
    CHECK(graph.progressSlot != Runtime::kNotWritten);
    REQUIRE(graph.layers.size() == 2);

    // Nodes breadth first: base, locomotion, airborne, land, jump_start, fall.
    const Runtime::AnimatorLayer &base = graph.layers[0];
    REQUIRE(base.graph.nodes.size() == 6);
    CHECK(base.graph.nodes[2].name == "airborne");
    CHECK(base.graph.nodes[2].childCount == 2);
    CHECK(base.states[1].clip.asset == Core::DerivedAssetId(kSpace));
    CHECK(base.states[1].x != Runtime::kNotWritten);
    CHECK(base.states[1].then == Runtime::kNotWritten);
    // jump_start moves on to fall, its sibling at index 1.
    CHECK(base.states[4].then == 1);
    CHECK(base.states[3].then == 0);
    REQUIRE(base.transitions.size() == 3);
    CHECK(base.transitions[0].fade != Runtime::kNotWritten);
    CHECK_FALSE(base.transitions[1].interrupt);
    CHECK(base.transitions[2].interrupt);

    const Runtime::AnimatorLayer &upper = graph.layers[1];
    CHECK(upper.maskRoot.View() == "spine_01");
    CHECK(upper.weight != Runtime::kNotWritten);
    CHECK(upper.states[1].pose);
    CHECK(upper.states[1].clip.asset == Core::DerivedAssetId(Clip("Pistol_Aim_Neutral")));
}

TEST_CASE("Animator compile: a cooked graph reads back, and a damaged or empty one is refused")
{
    const std::expected<Runtime::AnimatorGraph, std::string> compiled = Compile(kCharacter);
    REQUIRE_MESSAGE(compiled.has_value(), compiled.error());
    const std::vector<std::byte> payload = Runtime::WriteAnimatorGraph(*compiled);
    const std::expected<Runtime::AnimatorGraph, Core::AssetError> read = Runtime::ReadAnimatorGraph(payload);
    REQUIRE_MESSAGE(read.has_value(), Core::Describe(read.error()));
    CHECK(read->code == compiled->code);
    CHECK(read->layers[0].graph.nodes[4].name == "jump_start");
    CHECK(read->layers[0].states[4].then == 1);
    CHECK(read->layers[1].maskRoot.View() == "spine_01");

    Runtime::AnimatorGraph pastSiblings = *compiled;
    pastSiblings.layers[0].states[4].then = 2;
    CHECK_FALSE(Runtime::ReadAnimatorGraph(Runtime::WriteAnimatorGraph(pastSiblings)).has_value());

    Runtime::AnimatorGraph badCode = *compiled;
    badCode.layers[1].weight = static_cast<uint32_t>(badCode.code.size());
    CHECK_FALSE(Runtime::ReadAnimatorGraph(Runtime::WriteAnimatorGraph(badCode)).has_value());

    Runtime::AnimatorGraph maskedBase = *compiled;
    maskedBase.layers[0].maskRoot = Core::InternedString{"spine_01"};
    CHECK_FALSE(Runtime::ReadAnimatorGraph(Runtime::WriteAnimatorGraph(maskedBase)).has_value());

    CHECK_FALSE(Runtime::ReadAnimatorGraph(Runtime::WriteAnimatorGraph(Runtime::AnimatorGraph{})).has_value());
    const std::vector<std::byte> truncated{payload.begin(),
                                           payload.begin() + static_cast<std::ptrdiff_t>(payload.size() / 2)};
    CHECK_FALSE(Runtime::ReadAnimatorGraph(truncated).has_value());
}

TEST_CASE("Animator compile: names are checked against the asset tree and the skeleton")
{
    CHECK(Refuses(File("layer base {\n    state a { play \"nope/Walk.glb\"; }\n}\n"), 4,
                  "there is no \"nope/Walk.glb\" in the asset tree"));
    CHECK(Refuses(File("layer base {\n    state a { play \"" + std::string{kMaterial} + "\"; }\n}\n"), 4,
                  "not a clip or blend space"));
    CHECK(Refuses(File("layer base {\n    state a { play \"" + Clip("Walk_Loop") +
                       "\"; }\n}\nlayer upper {\n    mask \"spine_O1\";\n    state b { pose \"" +
                       Clip("Pistol_Aim_Neutral") + "\"; }\n}\n"),
                  7, "did you mean \"spine_01\"?"));
    CHECK(Refuses("use animation;\nskeleton \"" + std::string{kSpace} + "\";\nlayer base { }\n", 2,
                  "is a blend space, not a model"));
    CHECK(Refuses("use animation;\nlayer base {\n    state a { play \"" + Clip("Walk_Loop") + "\"; }\n}\n", 1,
                  "the file has no \"skeleton\" clause"));
}

TEST_CASE("Animator compile: each state plays one thing, and only a state holding none plays")
{
    const std::string walk = Clip("Walk_Loop");
    CHECK(Refuses(File("layer base {\n    state a { }\n}\n"), 4, "state \"a\" plays nothing"));
    CHECK(Refuses(File("layer base {\n    state a { pose \"" + std::string{kSpace} + "\"; }\n}\n"), 4,
                  "a blend space can't be held as a pose"));
    CHECK(Refuses(File("layer base {\n    state a {\n        play \"" + walk +
                       "\";\n        state b { play \"" + walk + "\"; }\n    }\n}\n"),
                  5, "holds states, so it plays nothing"));
    CHECK(Refuses(File("layer base {\n    state a { pose \"" + walk + "\"; then b; }\n    state b { play \"" + walk +
                       "\"; }\n}\n"),
                  4, "a pose holds still, so it never finishes"));
    CHECK(Refuses(File("layer base {\n    state a { play \"" + walk + "\"; pose \"" + walk + "\"; }\n}\n"), 4,
                  "plays two things"));
}

TEST_CASE("Animator compile: the first layer is the whole body")
{
    const std::string walk = Clip("Walk_Loop");
    CHECK(Refuses(File("layer base {\n    mask \"spine_01\";\n    state a { play \"" + walk + "\"; }\n}\n"), 4,
                  "the first layer can't have \"mask\""));
    CHECK(Refuses(File("layer base {\n    weight 0.5;\n    state a { play \"" + walk + "\"; }\n}\n"), 4,
                  "the first layer can't have \"weight\""));
    const std::expected<Runtime::AnimatorGraph, std::string> faded =
        Compile(File("layer base {\n    fade 0.3;\n    state a { play \"" + walk + "\"; }\n}\n"));
    REQUIRE_MESSAGE(faded.has_value(), faded.error());
    CHECK(faded->layers[0].fade != Runtime::kNotWritten);
}

TEST_CASE("Animator compile: a clip param is bound on the Animator, and a library plays nothing")
{
    const std::expected<Runtime::AnimatorGraph, std::string> bound =
        Compile(File("param run_clip: clip;\nlayer base {\n    state run { play run_clip; }\n}\n"));
    REQUIRE_MESSAGE(bound.has_value(), bound.error());
    CHECK(bound->clipParams == std::vector<std::string>{"run_clip"});
    CHECK(bound->layers[0].states[1].clip.param == 0);
    CHECK(bound->layers[0].states[1].clip.asset == Core::AssetId{});

    const std::expected<Runtime::AnimatorGraph, std::string> library =
        Compile("use animation;\nsigiltype library;\nconst walk_speed = 1.5;\n");
    REQUIRE_MESSAGE(library.has_value(), library.error());
    CHECK(library->layers.empty());

    TestTree tree;
    tree.Library("shared/moves.sgl", "use animation;\nsigiltype library;\nconst walk = \"" + Clip("Walk_Loop") +
                                         "\";\n");
    const std::expected<Runtime::AnimatorGraph, std::string> importing = Runtime::Import::CompileAnimator(
        File("import \"shared/moves.sgl\";\nlayer base {\n    state a { play walk; }\n}\n"), tree);
    REQUIRE_MESSAGE(importing.has_value(), importing.error());
    CHECK(importing->layers[0].states[1].clip.asset == Core::DerivedAssetId(Clip("Walk_Loop")));
}
