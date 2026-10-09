/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBenchmark.cpp
/// @brief What a frame of Sigil costs: 1000 characters, each working out its
///        lets and then 10 transition conditions.
///
/// A measurement, not a threshold. The number printed is the point, and only a
/// gcc-ship build's number means anything; the bound is loose enough that a
/// debug build or a loaded machine can't fail it.

#include "BytecodeTesting.hpp"

#include <doctest/doctest.h>

// A sanitizer instruments every load and store, so its numbers measure the
// sanitizer rather than the evaluator.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#    define ASSISI_SIGIL_SANITIZED 1
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#        define ASSISI_SIGIL_SANITIZED 1
#    endif
#endif

#if !defined(ASSISI_SIGIL_SANITIZED)

#    include <algorithm>
#    include <chrono>
#    include <cstdio>

using namespace Assisi::Sigil;
using namespace Assisi::Sigil::Testing;

namespace
{

constexpr uint32_t kCharacters = 1000;
constexpr uint32_t kConditions = 10;
/// Frames timed; the fastest is reported, since a preemption only slows the
/// frame it lands in.
constexpr int32_t kFrames = 50;
/// A frame of a debug build on a busy machine stays well under this; a
/// shipped one is orders of magnitude faster.
constexpr double kLooseBoundMillis = 500.0;
/// Multiplier and increment of the generator that scatters param values, so
/// branches don't all go one way.
constexpr uint32_t kScatterMultiplier = 1664525;
constexpr uint32_t kScatterIncrement = 1013904223;

/// A character's worth of logic of the shape a locomotion machine has.
constexpr std::string_view kSource = "use robot;\n"
                                     "enum Stance { stand, crouch, prone }\n"
                                     "param pace: float;\nparam grounded: bool;\nparam jump: trigger;\n"
                                     "param health: int;\nparam stance: Stance;\nparam aim: float;\n"
                                     "let moving = pace > 0.1;\n"
                                     "let running = moving && pace > 4.0 && stance == Stance.stand;\n"
                                     "let hurt = health < 30;\n"
                                     "let effort = clamp(pace * 0.25, 0.0, 1.0) + abs(aim) * 0.5;\n"
                                     "machine body {\n"
                                     "    node idle { }\n    node walk { }\n    node run { }\n    node air { }\n"
                                     "    node limp { }\n"
                                     "    idle -> walk when moving && grounded;\n"
                                     "    walk -> run when running && !hurt;\n"
                                     "    run -> walk when !running || effort > 1.2;\n"
                                     "    walk -> idle when !moving;\n"
                                     "    any -> air when jump && grounded;\n"
                                     "    air -> idle when grounded && !moving;\n"
                                     "    air -> walk when grounded && moving;\n"
                                     "    walk -> limp when hurt && stance != Stance.prone;\n"
                                     "    limp -> walk when health >= 30 || battery() > 0.5;\n"
                                     "    run -> limp when hurt && pace * 2.0 > 9.0 - aim;\n"
                                     "}\n";

struct Frame
{
    std::vector<Word> code;
    std::vector<Word> blocks; ///< kCharacters blocks, one after another.
    std::vector<uint32_t> lets;
    std::vector<uint32_t> conditions;
    Layout layout;
};

Frame Build()
{
    const std::expected<Program, Diagnostics> program = CompileRobot(kSource);
    REQUIRE_MESSAGE(program.has_value(), Dump(Errors(program)));
    Frame frame;
    frame.layout = MakeLayout(*program, Robot());
    frame.lets = *LowerLets(*program, frame.layout, frame.code);
    for (const Transition &transition : program->root.children[0].transitions)
    {
        frame.conditions.push_back(*LowerExpression(frame.layout, transition.condition, frame.code));
    }
    REQUIRE(frame.conditions.size() == kConditions);

    const uint32_t slots = static_cast<uint32_t>(frame.layout.slots.size());
    frame.blocks.resize(std::size_t{kCharacters} * slots);
    uint32_t scatter = 1;
    for (uint32_t character = 0; character < kCharacters; ++character)
    {
        const std::span<Word> block{frame.blocks.data() + std::size_t{character} * slots, slots};
        const std::array<uint32_t, 6> raw{scatter % 8, scatter % 2, (scatter >> 3) % 2, scatter % 100,
                                          (scatter >> 5) % 3, scatter % 5};
        block[0] = FromFloat(static_cast<float>(raw[0]));
        block[1] = FromBool(raw[1] != 0);
        block[2] = FromBool(raw[2] != 0);
        block[3] = FromInt(static_cast<int32_t>(raw[3]));
        block[4] = FromInt(static_cast<int32_t>(raw[4]));
        block[5] = FromFloat(static_cast<float>(raw[5]) - 2.f);
        scatter = scatter * kScatterMultiplier + kScatterIncrement;
    }
    return frame;
}

/// One frame: every character's lets, then its conditions. Gives how many
/// conditions held, so none of the work can be skipped.
uint32_t RunFrame(Frame &frame)
{
    const uint32_t slots = static_cast<uint32_t>(frame.layout.slots.size());
    uint32_t held = 0;
    for (uint32_t character = 0; character < kCharacters; ++character)
    {
        const std::span<Word> block{frame.blocks.data() + std::size_t{character} * slots, slots};
        EvaluateLets(frame.code, frame.lets, frame.layout, block);
        for (const uint32_t condition : frame.conditions)
        {
            held += Evaluate(frame.code, condition, block);
        }
    }
    return held;
}

} // namespace

TEST_CASE("Benchmark: a frame of 1000 characters with 10 conditions each")
{
    Frame frame = Build();
    std::chrono::steady_clock::duration fastest = std::chrono::steady_clock::duration::max();
    uint32_t held = 0;
    for (int32_t run = 0; run < kFrames; ++run)
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        held = RunFrame(frame);
        fastest = std::min(fastest, std::chrono::steady_clock::now() - start);
    }
    const double micros =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(fastest).count()) / 1000.0;
    const double perCondition = micros * 1000.0 / static_cast<double>(kCharacters * kConditions);
    std::printf("\n[sigil] %u characters x %u conditions, %zu lets each: %.1f us a frame, fastest of %d "
                "(%.1f ns a condition, lets included); %u held\n\n",
                kCharacters, kConditions, frame.lets.size(), micros, kFrames, perCondition, held);

    CHECK(held > 0);
    CHECK(micros < kLooseBoundMillis * 1000.0);
}

#endif // !ASSISI_SIGIL_SANITIZED
