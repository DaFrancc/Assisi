/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Evaluate.hpp>

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace Assisi::Sigil
{

namespace
{

/// Int arithmetic runs on the unsigned words, where overflow wraps rather
/// than being undefined.
Word AddInt(Word left, Word right)
{
    return left + right;
}

Word SubtractInt(Word left, Word right)
{
    return left - right;
}

Word MultiplyInt(Word left, Word right)
{
    return left * right;
}

Word NegateInt(Word value)
{
    return Word{0} - value;
}

Word DivideInt(Word left, Word right)
{
    const int32_t numerator = ToInt(left);
    const int32_t denominator = ToInt(right);
    if (denominator == 0)
    {
        return FromInt(0);
    }
    if (denominator == -1)
    {
        return NegateInt(left);
    }
    return FromInt(numerator / denominator);
}

Word RemainderInt(Word left, Word right)
{
    const int32_t denominator = ToInt(right);
    if (denominator == 0 || denominator == -1)
    {
        return FromInt(0);
    }
    return FromInt(ToInt(left) % denominator);
}

Word DivideFloat(Word left, Word right)
{
    const float denominator = ToFloat(right);
    if (denominator == 0.f)
    {
        return FromFloat(0.f);
    }
    return FromFloat(ToFloat(left) / denominator);
}

Word AbsInt(Word value)
{
    return ToInt(value) < 0 ? NegateInt(value) : value;
}

/// The smaller of two values, or the first when they can't be ordered.
template <typename T> T Smaller(T first, T second)
{
    return second < first ? second : first;
}

/// The larger of two values, or the first when they can't be ordered.
template <typename T> T Larger(T first, T second)
{
    return first < second ? second : first;
}

/// The stack an expression runs on. Verify guarantees it never holds more
/// than kMaxStackDepth words or runs dry.
class Stack
{
  public:
    void Push(Word value)
    {
        _words[_size++] = value;
    }

    Word Pop()
    {
        return _words[--_size];
    }

    Word &Top()
    {
        return _words[_size - 1];
    }

    /// The top as left of a binary operation, after popping its right.
    Word &PopToLeft(Word &right)
    {
        right = Pop();
        return Top();
    }

  private:
    std::array<Word, kMaxStackDepth> _words;
    uint32_t _size = 0;
};

/// Replaces the top two words with @p op applied to them as ints.
template <typename Op> void IntBinary(Stack &stack, Op op)
{
    Word right = 0;
    Word &left = stack.PopToLeft(right);
    left = op(left, right);
}

/// Replaces the top two words with @p op applied to them as @p T.
template <typename T, typename Op> void Binary(Stack &stack, Op op)
{
    Word right = 0;
    Word &left = stack.PopToLeft(right);
    left = std::bit_cast<Word>(op(std::bit_cast<T>(left), std::bit_cast<T>(right)));
}

/// Replaces the top two words with the bool @p op gives on them as @p T.
template <typename T, typename Op> void Compare(Stack &stack, Op op)
{
    Word right = 0;
    Word &left = stack.PopToLeft(right);
    left = FromBool(op(std::bit_cast<T>(left), std::bit_cast<T>(right)));
}

/// Replaces the top three words, x, lo and hi, with x clamped as @p T.
template <typename T> void Clamp(Stack &stack)
{
    const T high = std::bit_cast<T>(stack.Pop());
    const T low = std::bit_cast<T>(stack.Pop());
    Word &value = stack.Top();
    value = std::bit_cast<Word>(Smaller(Larger(std::bit_cast<T>(value), low), high));
}

/// Moves @p pc past a jump at it when the top of @p stack is @p when, keeping
/// the top; otherwise drops the top and steps over the jump.
void Branch(Stack &stack, std::span<const Word> code, uint32_t &pc, bool when)
{
    if (ToBool(stack.Top()) == when)
    {
        pc += 1 + OperandOf(code[pc]);
        return;
    }
    stack.Pop();
    ++pc;
}

} // namespace

Word Evaluate(std::span<const Word> code, uint32_t entry, std::span<const Word> block)
{
    Stack stack;
    uint32_t pc = entry;
    while (true)
    {
        const Word word = code[pc];
        switch (OpcodeOf(word))
        {
        case Opcode::PushInt:
        case Opcode::PushFloat:
        case Opcode::PushBool:
            stack.Push(code[pc + 1]);
            pc += 2;
            continue;
        case Opcode::Load:
            stack.Push(block[OperandOf(word)]);
            break;
        case Opcode::IntToFloat:
            stack.Top() = FromFloat(static_cast<float>(ToInt(stack.Top())));
            break;
        case Opcode::NegateInt:
            stack.Top() = NegateInt(stack.Top());
            break;
        case Opcode::NegateFloat:
            stack.Top() = FromFloat(-ToFloat(stack.Top()));
            break;
        case Opcode::Not:
            stack.Top() = FromBool(!ToBool(stack.Top()));
            break;
        case Opcode::AddInt:
            IntBinary(stack, AddInt);
            break;
        case Opcode::AddFloat:
            Binary<float>(stack, std::plus<float>{});
            break;
        case Opcode::SubtractInt:
            IntBinary(stack, SubtractInt);
            break;
        case Opcode::SubtractFloat:
            Binary<float>(stack, std::minus<float>{});
            break;
        case Opcode::MultiplyInt:
            IntBinary(stack, MultiplyInt);
            break;
        case Opcode::MultiplyFloat:
            Binary<float>(stack, std::multiplies<float>{});
            break;
        case Opcode::DivideInt:
            IntBinary(stack, DivideInt);
            break;
        case Opcode::DivideFloat:
            IntBinary(stack, DivideFloat);
            break;
        case Opcode::RemainderInt:
            IntBinary(stack, RemainderInt);
            break;
        case Opcode::EqualWord:
            Compare<Word>(stack, std::equal_to<Word>{});
            break;
        case Opcode::EqualFloat:
            Compare<float>(stack, std::equal_to<float>{});
            break;
        case Opcode::NotEqualWord:
            Compare<Word>(stack, std::not_equal_to<Word>{});
            break;
        case Opcode::NotEqualFloat:
            Compare<float>(stack, std::not_equal_to<float>{});
            break;
        case Opcode::LessInt:
            Compare<int32_t>(stack, std::less<int32_t>{});
            break;
        case Opcode::LessFloat:
            Compare<float>(stack, std::less<float>{});
            break;
        case Opcode::LessEqualInt:
            Compare<int32_t>(stack, std::less_equal<int32_t>{});
            break;
        case Opcode::LessEqualFloat:
            Compare<float>(stack, std::less_equal<float>{});
            break;
        case Opcode::GreaterInt:
            Compare<int32_t>(stack, std::greater<int32_t>{});
            break;
        case Opcode::GreaterFloat:
            Compare<float>(stack, std::greater<float>{});
            break;
        case Opcode::GreaterEqualInt:
            Compare<int32_t>(stack, std::greater_equal<int32_t>{});
            break;
        case Opcode::GreaterEqualFloat:
            Compare<float>(stack, std::greater_equal<float>{});
            break;
        case Opcode::AndJump:
            Branch(stack, code, pc, false);
            continue;
        case Opcode::OrJump:
            Branch(stack, code, pc, true);
            continue;
        case Opcode::AbsInt:
            stack.Top() = AbsInt(stack.Top());
            break;
        case Opcode::AbsFloat:
            stack.Top() = FromFloat(std::fabs(ToFloat(stack.Top())));
            break;
        case Opcode::MinInt:
            Binary<int32_t>(stack, Smaller<int32_t>);
            break;
        case Opcode::MinFloat:
            Binary<float>(stack, Smaller<float>);
            break;
        case Opcode::MaxInt:
            Binary<int32_t>(stack, Larger<int32_t>);
            break;
        case Opcode::MaxFloat:
            Binary<float>(stack, Larger<float>);
            break;
        case Opcode::ClampInt:
            Clamp<int32_t>(stack);
            break;
        case Opcode::ClampFloat:
            Clamp<float>(stack);
            break;
        case Opcode::Return:
            return stack.Pop();
        case Opcode::Count_:
            // Verify refuses any word that isn't an opcode.
            std::unreachable();
        }
        ++pc;
    }
}

void EvaluateLets(std::span<const Word> code, std::span<const uint32_t> letEntries, const Layout &layout,
                  std::span<Word> block)
{
    for (uint32_t let = 0; let < letEntries.size(); ++let)
    {
        block[layout.LetSlot(let)] = Evaluate(code, letEntries[let], block);
    }
}

} // namespace Assisi::Sigil
