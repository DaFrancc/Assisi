/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestAnimatorParams.cpp
/// @brief Game code feeds an animator's params through handles: each finds
///        its param once per file, a wrong one is reported once, a float can
///        be eased, and a trigger can follow a count.

#include "AnimatorTesting.hpp"
#include "TestRig.hpp"

#include <doctest/doctest.h>

#include <Assisi/Core/Logger.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/Runtime/AnimatorParams.hpp>
#include <Assisi/Runtime/AnimatorStep.hpp>
#include <Assisi/Runtime/Components.hpp>
#include <Assisi/Runtime/Import/AnimatorCompiler.hpp>
#include <Assisi/Sigil/Bytecode.hpp>
#include <Assisi/Sigil/Layout.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace Assisi;

namespace
{

/// Whether @p Param's Set takes a @p Value as it is.
template <typename Param, typename Value>
concept SetTakes = requires(Param param, ECS::Entity entity, Runtime::AnimatorRun run, Value value) {
    param.Set(entity, run, value);
};

static_assert(SetTakes<Runtime::AnimatorParam<float>, float>);
static_assert(!SetTakes<Runtime::AnimatorParam<float>, bool>, "true given to a float param must not build");
static_assert(!SetTakes<Runtime::AnimatorParam<bool>, float>);
static_assert(!SetTakes<Runtime::AnimatorParam<int32_t>, float>);

constexpr std::string_view kModel = "tests/params.glb";
constexpr std::string_view kClip = "tests/idle.glb";

/// A file with a param of each kind code sets.
constexpr std::string_view kFile = R"(use animation;
skeleton "tests/params.glb";
param speed: float;
param grounded: bool;
param jump: trigger;
param gait: int;
layer base { state idle { play "tests/idle.glb"; } }
)";

/// The same params after the file was changed: one more ahead of them, and in
/// another order, so each lives in another slot.
constexpr std::string_view kChangedFile = R"(use animation;
skeleton "tests/params.glb";
param lean: float;
param gait: int;
param grounded: bool;
param speed: float;
param jump: trigger;
layer base { state idle { play "tests/idle.glb"; } }
)";

/// How long a damped float takes to close about two thirds of the way (s), and
/// one frame (s).
constexpr float kEase = 0.1f;
constexpr float kFrame = 0.05f;

/// How far apart two eased values may be: float rounding.
constexpr float kEaseTolerance = 1e-5f;

std::shared_ptr<const Runtime::AnimatorGraph> Compiled(std::string_view source)
{
    Runtime::Testing::RigTree tree;
    tree.Model(std::string{kModel}, Runtime::Testing::BuildSkeleton(Runtime::Testing::TwoLimbs()));
    tree.Clip(std::string{kClip});
    std::expected<Runtime::AnimatorGraph, std::string> graph = Runtime::Import::CompileAnimator(source, tree);
    REQUIRE_MESSAGE(graph.has_value(), graph.error());
    return std::make_shared<const Runtime::AnimatorGraph>(std::move(*graph));
}

/// A character running a file, as the engine keeps one.
struct Character
{
    Runtime::Animator animator;
    Runtime::AnimatorRun run;
    Runtime::AnimationPlayback playback;
};

void Bind(Character &character, std::shared_ptr<const Runtime::AnimatorGraph> graph)
{
    Runtime::BindAnimator(character.animator, character.run, character.playback, std::move(graph));
}

/// Whether trigger @p name is set in @p run's block.
bool TriggerSet(const Runtime::AnimatorRun &run, std::string_view name)
{
    const std::optional<uint32_t> slot = Sigil::FindSlot(run.graph->layout, name);
    REQUIRE(slot.has_value());
    return Sigil::ToBool(run.block[*slot]);
}

/// Every line logged while it is listening.
class Lines final : public Core::Sink
{
public:
    void Write(Core::LogLevel level, std::string_view message) override
    {
        (void)level;
        const std::lock_guard<std::mutex> lock(_mutex);
        _lines.emplace_back(message);
    }

    /// How many lines logged since the last call hold every one of @p parts.
    std::size_t CountHolding(std::initializer_list<std::string_view> parts)
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        std::size_t count = 0;
        for (const std::string &line : _lines)
        {
            bool all = true;
            for (const std::string_view part : parts)
            {
                all = all && line.find(part) != std::string::npos;
            }
            count += all ? 1u : 0u;
        }
        _lines.clear();
        return count;
    }

private:
    std::mutex _mutex;
    std::vector<std::string> _lines;
};

/// A Lines the logger writes to.
std::shared_ptr<Lines> Listening()
{
    std::shared_ptr<Lines> lines = std::make_shared<Lines>();
    Core::GetLogger().AddSink(lines);
    return lines;
}

/// The logger has no way to take a sink away, so one listens for the whole run.
Lines &Logged()
{
    static const std::shared_ptr<Lines> lines = Listening();
    return *lines;
}

} // namespace

TEST_CASE("Animator params: a handle finds its param once, and again in a changed file")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    const std::shared_ptr<const Runtime::AnimatorGraph> file = Compiled(kFile);
    Character first;
    Character second;
    Bind(first, file);
    Bind(second, file);

    Runtime::AnimatorParam<float> speed{"speed"};
    CHECK(speed.Set(entity, first.run, 2.5f));
    CHECK(speed.Set(entity, second.run, 1.5f));
    CHECK(speed.Get(first.run) == std::optional<float>{2.5f});
    CHECK(speed.Get(second.run) == std::optional<float>{1.5f});
    CHECK(speed.Lookups() == 1u);

    // Changed: the value is carried over by name, and the handle finds where it
    // lives now.
    const std::shared_ptr<const Runtime::AnimatorGraph> changed = Compiled(kChangedFile);
    REQUIRE(Sigil::FindSlot(changed->layout, "speed") != Sigil::FindSlot(file->layout, "speed"));
    Bind(first, changed);
    CHECK(speed.Get(first.run) == std::optional<float>{2.5f});
    CHECK(speed.Set(entity, first.run, 4.f));
    CHECK(Sigil::ToFloat(first.run.block[*Sigil::FindSlot(changed->layout, "speed")]) == 4.f);
    CHECK(speed.Lookups() == 2u);
}

TEST_CASE("Animator params: each setter writes its own type")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    Character character;
    Bind(character, Compiled(kFile));

    Runtime::AnimatorParam<bool> grounded{"grounded"};
    Runtime::AnimatorParam<int32_t> gait{"gait"};
    Runtime::AnimatorTrigger jump{"jump"};
    CHECK(grounded.Set(entity, character.run, true));
    CHECK(gait.Set(entity, character.run, 2));
    CHECK(grounded.Get(character.run) == std::optional<bool>{true});
    CHECK(gait.Get(character.run) == std::optional<int32_t>{2});
    CHECK_FALSE(TriggerSet(character.run, "jump"));
    CHECK(jump.Fire(entity, character.run));
    CHECK(TriggerSet(character.run, "jump"));

    // A file not loaded yet takes nothing, and nothing is wrong.
    Runtime::AnimatorRun unbound;
    CHECK_FALSE(grounded.Set(entity, unbound, true));
    CHECK_FALSE(grounded.Get(unbound).has_value());
}

TEST_CASE("Animator params: a wrong name or type is reported once, naming the entity and the param")
{
    ECS::Scene scene;
    (void)scene.Create();
    const ECS::Entity entity = scene.Create();
    const std::string index = "index " + std::to_string(entity.index) + ",";
    Character character;
    Bind(character, Compiled(kFile));
    Lines &logged = Logged();
    (void)logged.CountHolding({});

    Runtime::AnimatorParam<float> misspelt{"sped"};
    CHECK_FALSE(misspelt.Set(entity, character.run, 1.f));
    CHECK_FALSE(misspelt.Set(entity, character.run, 1.f));
    CHECK(logged.CountHolding({"\"sped\"", index}) == 1u);

    Runtime::AnimatorParam<float> wrongType{"grounded"};
    CHECK_FALSE(wrongType.Set(entity, character.run, 1.f));
    CHECK_FALSE(wrongType.SetDamped(entity, character.run, 1.f, kEase, kFrame));
    CHECK(logged.CountHolding({"\"grounded\"", "bool", index}) == 1u);

    // A trigger is not a bool to hold, nor a bool a trigger to fire.
    Runtime::AnimatorParam<bool> triggerAsBool{"jump"};
    CHECK_FALSE(triggerAsBool.Set(entity, character.run, true));
    CHECK(logged.CountHolding({"\"jump\"", "trigger", index}) == 1u);
    Runtime::AnimatorTrigger boolAsTrigger{"grounded"};
    CHECK_FALSE(boolAsTrigger.Fire(entity, character.run));
    CHECK(logged.CountHolding({"\"grounded\"", index}) == 1u);

    // A file not loaded yet is not a mistake.
    Runtime::AnimatorRun unbound;
    CHECK_FALSE(misspelt.Set(entity, unbound, 1.f));
    CHECK(logged.CountHolding({"\"sped\""}) == 0u);
}

TEST_CASE("Animator params: a damped float closes the same share of the way in the same time, at any frame rate")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    Character once;
    Character twice;
    const std::shared_ptr<const Runtime::AnimatorGraph> file = Compiled(kFile);
    Bind(once, file);
    Bind(twice, file);

    Runtime::AnimatorParam<float> speed{"speed"};
    REQUIRE(speed.SetDamped(entity, once.run, 1.f, kEase, 2.f * kFrame));
    REQUIRE(speed.SetDamped(entity, twice.run, 1.f, kEase, kFrame));
    REQUIRE(speed.SetDamped(entity, twice.run, 1.f, kEase, kFrame));
    const float closed = speed.Get(once.run).value_or(0.f);
    CHECK(closed == doctest::Approx(1.f - std::exp(-2.f * kFrame / kEase)).epsilon(kEaseTolerance));
    CHECK(speed.Get(twice.run).value_or(0.f) == doctest::Approx(closed).epsilon(kEaseTolerance));

    // No time to ease over goes straight there.
    REQUIRE(speed.SetDamped(entity, once.run, 3.f, 0.f, kFrame));
    CHECK(speed.Get(once.run) == std::optional<float>{3.f});
}

TEST_CASE("Animator params: a trigger fed a count fires when it changes, wrapping included, and not on first sight")
{
    ECS::Scene scene;
    const ECS::Entity entity = scene.Create();
    Character character;
    Bind(character, Compiled(kFile));
    Runtime::AnimatorTrigger jump{"jump"};

    // The first count is only remembered: a character appearing has not
    // jumped.
    CHECK_FALSE(jump.FireOnChange(entity, character.run, 7u));
    CHECK_FALSE(TriggerSet(character.run, "jump"));
    CHECK_FALSE(jump.FireOnChange(entity, character.run, 7u));
    CHECK(jump.FireOnChange(entity, character.run, 8u));
    CHECK(TriggerSet(character.run, "jump"));

    Runtime::StepAnimator(character.animator, character.run, character.playback);
    REQUIRE_FALSE(TriggerSet(character.run, "jump"));
    const uint32_t last = std::numeric_limits<uint32_t>::max();
    CHECK(jump.FireOnChange(entity, character.run, last));
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    CHECK(jump.FireOnChange(entity, character.run, 0u));

    // A changed file keeps the count it last saw, where the trigger lives now:
    // the next count is a change, not a first sight.
    Runtime::StepAnimator(character.animator, character.run, character.playback);
    const std::shared_ptr<const Runtime::AnimatorGraph> changed = Compiled(kChangedFile);
    REQUIRE(Sigil::FindSlot(changed->layout, "jump") != Sigil::FindSlot(character.run.graph->layout, "jump"));
    Bind(character, changed);
    CHECK(jump.FireOnChange(entity, character.run, 1u));
}
