/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestVerify.cpp
/// @brief Code from a damaged asset is refused before it runs: each way it
///        could read or jump out of bounds, or leave the stack wrong, is
///        caught on its own.

#include <Assisi/Sigil/Verify.hpp>

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace Assisi::Sigil;

namespace
{

/// Why @p code is refused against a block of @p slots words; empty when it isn't.
std::string Refusal(const std::vector<Word> &code, uint32_t slots = 2)
{
    const std::expected<void, std::string> verified = Verify(code, 0, slots);
    return verified ? std::string{} : verified.error();
}

constexpr Word kPushInt = MakeWord(Opcode::PushInt);
constexpr Word kPushBool = MakeWord(Opcode::PushBool);
constexpr Word kReturn = MakeWord(Opcode::Return);

} // namespace

TEST_CASE("Verify: sound code passes, starting where its entry says")
{
    CHECK(Refusal({kPushInt, 1, kReturn}).empty());
    CHECK(Refusal({MakeWord(Opcode::Load, 1), kReturn}).empty());
    const std::vector<Word> two{kReturn, kPushBool, 0, MakeWord(Opcode::AndJump, 2), kPushBool, 1, kReturn};
    CHECK(Verify(two, 1, 0).has_value());
}

TEST_CASE("Verify: each kind of damage is refused")
{
    CHECK(Refusal({MakeWord(Opcode::Count_), kReturn}).find("isn't an instruction") != std::string::npos);
    CHECK(Refusal({MakeWord(Opcode::Load, 2), kReturn}).find("reads slot 2 of 2") != std::string::npos);
    CHECK(Refusal({kPushInt}).find("has no value") != std::string::npos);
    CHECK(Refusal({kPushBool, 2, kReturn}).find("isn't 0 or 1") != std::string::npos);
    CHECK(Refusal({MakeWord(Opcode::AddInt, 1), kReturn}).find("doesn't take") != std::string::npos);
    CHECK(Refusal({kPushInt, 1, MakeWord(Opcode::AddInt), kReturn}).find("more than the stack holds") !=
          std::string::npos);
    CHECK(Refusal({kPushInt, 1}).find("without a Return") != std::string::npos);
    CHECK(Refusal({kPushInt, 1, kPushInt, 2, kReturn}).find("leaves 1 words behind") != std::string::npos);
    CHECK(Refusal({kReturn}).find("more than the stack holds") != std::string::npos);

    std::vector<Word> deep;
    for (uint32_t i = 0; i <= kMaxStackDepth; ++i)
    {
        deep.push_back(kPushInt);
        deep.push_back(0);
    }
    deep.push_back(kReturn);
    CHECK(Refusal(deep).find("words of stack") != std::string::npos);
}

TEST_CASE("Verify: a jump must land on an instruction, no further than the Return, with the same stack")
{
    // Lands on the value of the push after it.
    CHECK(Refusal({kPushBool, 0, MakeWord(Opcode::AndJump, 1), kPushBool, 1, kReturn}).find("lands inside") !=
          std::string::npos);
    CHECK(Refusal({kPushBool, 0, MakeWord(Opcode::AndJump, 9), kPushBool, 1, kReturn}).find("past the end") !=
          std::string::npos);
    CHECK(Refusal({kPushBool, 0, MakeWord(Opcode::AndJump, 3), kPushBool, 1, kReturn, kReturn})
              .find("past the Return") != std::string::npos);
    // The right side leaves two words where the jump arrives with one.
    CHECK(Refusal({kPushBool, 0, MakeWord(Opcode::OrJump, 4), kPushBool, 1, kPushBool, 1, kReturn})
              .find("different stacks") != std::string::npos);
}
