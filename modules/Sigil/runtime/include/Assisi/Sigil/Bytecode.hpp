/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Bytecode.hpp
/// @brief The instructions a Sigil expression compiles to, and the words they
///        work on.
///
/// Code is a flat array of 32-bit words, so a cooked asset stores it as is.
/// Each instruction is one word, an opcode in the low bits and an operand
/// above it; a push carries its value in the word after. The machine is a
/// stack of words: ints are two's complement, floats are their bits, bools
/// are 0 or 1. Opcodes are typed when the compiler emits them, so running
/// code never asks what type a word holds.
///
/// Every operation has a result on every input:
///   - int arithmetic wraps; x / 0 and x % 0 are 0, and so is a float divided
///     by zero; INT_MIN / -1 is INT_MIN, INT_MIN % -1 is 0;
///   - float comparisons follow IEEE, so NaN equals nothing;
///   - min and max give their first value when a comparison can't order the
///     two, and clamp(x, lo, hi) is min(max(x, lo), hi), so lo > hi gives hi.
/// Host and client agree because both run the same binary on the same
/// replicated values.

#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace Assisi::Sigil
{

using Word = uint32_t;

enum class Opcode : uint8_t
{
    PushInt,   ///< The next word, an int.
    PushFloat, ///< The next word, a float.
    PushBool,  ///< The next word, a bool.
    Load,      ///< The block's slot named by the operand.
    IntToFloat,
    NegateInt,
    NegateFloat,
    Not,
    AddInt,
    AddFloat,
    SubtractInt,
    SubtractFloat,
    MultiplyInt,
    MultiplyFloat,
    DivideInt,
    DivideFloat,
    RemainderInt,
    EqualWord, ///< Ints, bools and enum values, which are equal when their words are.
    EqualFloat,
    NotEqualWord,
    NotEqualFloat,
    LessInt,
    LessFloat,
    LessEqualInt,
    LessEqualFloat,
    GreaterInt,
    GreaterFloat,
    GreaterEqualInt,
    GreaterEqualFloat,
    /// When the top is false, skips the operand's count of words and keeps it
    /// as the result; otherwise drops it and goes on to the right side.
    AndJump,
    /// When the top is true, skips the operand's count of words and keeps it
    /// as the result; otherwise drops it and goes on to the right side.
    OrJump,
    AbsInt,
    AbsFloat,
    MinInt,
    MinFloat,
    MaxInt,
    MaxFloat,
    ClampInt,
    ClampFloat,
    Return, ///< Ends the expression; the one word left is its value.
    Count_,
};

/// @brief What an instruction's operand means.
enum class OperandKind : uint8_t
{
    None,      ///< It has none, and the operand bits are zero.
    Slot,      ///< An index into the block.
    Immediate, ///< It has none; its value is the next word.
    Jump,      ///< A count of words to skip forward, from the word after it.
    Count_,
};

struct OpcodeInfo
{
    std::string_view name;
    OperandKind operand = OperandKind::None;
    uint8_t pops = 0;   ///< Words taken off the stack.
    uint8_t pushes = 0; ///< Words put back.
};

/// @brief Bits of a word that hold the opcode; the rest hold the operand.
inline constexpr uint32_t kOpcodeBits = 8;
inline constexpr Word kOpcodeMask = (Word{1} << kOpcodeBits) - 1;
/// @brief The largest operand an instruction can carry.
inline constexpr uint32_t kMaxOperand = (uint32_t{1} << (32 - kOpcodeBits)) - 1;
/// @brief The most words an expression may have on the stack at once. The
///        compiler refuses an expression that needs more, so the evaluator's
///        stack is a fixed array.
inline constexpr uint32_t kMaxStackDepth = 64;

[[nodiscard]] constexpr Word MakeWord(Opcode opcode, uint32_t operand = 0)
{
    return static_cast<Word>(opcode) | (operand << kOpcodeBits);
}

[[nodiscard]] constexpr Opcode OpcodeOf(Word word)
{
    return static_cast<Opcode>(word & kOpcodeMask);
}

[[nodiscard]] constexpr uint32_t OperandOf(Word word)
{
    return word >> kOpcodeBits;
}

[[nodiscard]] constexpr Word FromInt(int32_t value)
{
    return static_cast<Word>(value);
}

[[nodiscard]] constexpr int32_t ToInt(Word word)
{
    return static_cast<int32_t>(word);
}

[[nodiscard]] constexpr Word FromFloat(float value)
{
    return std::bit_cast<Word>(value);
}

[[nodiscard]] constexpr float ToFloat(Word word)
{
    return std::bit_cast<float>(word);
}

[[nodiscard]] constexpr Word FromBool(bool value)
{
    return value ? Word{1} : Word{0};
}

[[nodiscard]] constexpr bool ToBool(Word word)
{
    return word != 0;
}

/// @brief What @p opcode is called, what its operand means, and what it does
///        to the stack. A switch, so an opcode added without a row here fails
///        to compile.
[[nodiscard]] OpcodeInfo Describe(Opcode opcode);

/// @brief The expression at @p entry in @p code, one instruction per line, for
///        tests and tools. @p code must have passed Verify.
[[nodiscard]] std::string Disassemble(std::span<const Word> code, uint32_t entry);

} // namespace Assisi::Sigil
