/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Verify.hpp>

#include <algorithm>
#include <format>
#include <vector>

namespace Assisi::Sigil
{

namespace
{

/// A jump not yet reached: where it lands, and the stack depth it lands with.
struct Landing
{
    uint32_t target = 0;
    uint32_t depth = 0;
};

using Result = std::expected<void, std::string>;

class Verifier
{
  public:
    Verifier(std::span<const Word> code, uint32_t slotCount) : _landings(), _code(code), _slotCount(slotCount)
    {
    }

    Result Run(uint32_t entry)
    {
        for (uint32_t pc = entry; pc < _code.size();)
        {
            if (Result landed = Land(pc); !landed)
            {
                return landed;
            }
            const Opcode opcode = OpcodeOf(_code[pc]);
            if (opcode >= Opcode::Count_)
            {
                return std::unexpected(std::format("word {} isn't an instruction", pc));
            }
            const OpcodeInfo info = Describe(opcode);
            if (Result operand = CheckOperand(pc, info); !operand)
            {
                return operand;
            }
            if (Result stacked = Step(pc, info); !stacked)
            {
                return stacked;
            }
            if (opcode == Opcode::Return)
            {
                return Finish(pc);
            }
            pc += info.operand == OperandKind::Immediate ? 2 : 1;
        }
        return std::unexpected("the code ends without a Return");
    }

  private:
    /// Checks the jumps landing at @p pc arrive with the depth it has, and
    /// that none was meant to land on a word already passed.
    Result Land(uint32_t pc)
    {
        for (const Landing &landing : _landings)
        {
            if (landing.target < pc)
            {
                return std::unexpected(std::format("a jump lands inside the instruction before word {}", pc));
            }
            if (landing.target == pc && landing.depth != _depth)
            {
                return std::unexpected(std::format("the paths meeting at word {} leave different stacks", pc));
            }
        }
        std::erase_if(_landings, [pc](const Landing &landing) { return landing.target == pc; });
        return {};
    }

    Result CheckOperand(uint32_t pc, const OpcodeInfo &info) const
    {
        const uint32_t operand = OperandOf(_code[pc]);
        switch (info.operand)
        {
        case OperandKind::Slot:
            if (operand >= _slotCount)
            {
                return std::unexpected(std::format("word {} reads slot {} of {}", pc, operand, _slotCount));
            }
            return {};
        case OperandKind::Immediate:
            if (pc + 1 >= _code.size())
            {
                return std::unexpected(std::format("the push at word {} has no value", pc));
            }
            if (OpcodeOf(_code[pc]) == Opcode::PushBool && _code[pc + 1] > FromBool(true))
            {
                return std::unexpected(std::format("the bool pushed at word {} isn't 0 or 1", pc));
            }
            return CheckNoOperand(pc, operand);
        case OperandKind::Jump:
            return {};
        default:
            return CheckNoOperand(pc, operand);
        }
    }

    static Result CheckNoOperand(uint32_t pc, uint32_t operand)
    {
        if (operand != 0)
        {
            return std::unexpected(std::format("word {} has an operand it doesn't take", pc));
        }
        return {};
    }

    /// Applies the instruction at @p pc to the depth, recording where a jump lands.
    Result Step(uint32_t pc, const OpcodeInfo &info)
    {
        if (_depth < info.pops)
        {
            return std::unexpected(std::format("word {} takes more than the stack holds", pc));
        }
        if (info.operand == OperandKind::Jump)
        {
            const uint64_t target = uint64_t{pc} + 1 + OperandOf(_code[pc]);
            if (target >= _code.size())
            {
                return std::unexpected(std::format("the jump at word {} lands past the end", pc));
            }
            _landings.push_back(Landing{.target = static_cast<uint32_t>(target), .depth = _depth});
        }
        _depth = _depth - info.pops + info.pushes;
        if (_depth > kMaxStackDepth)
        {
            return std::unexpected(std::format("word {} needs more than {} words of stack", pc, kMaxStackDepth));
        }
        return {};
    }

    /// The Return at @p pc ends the expression: the value it returns is the
    /// only word that was left, and no jump lands beyond it.
    Result Finish(uint32_t pc) const
    {
        if (_depth != 0)
        {
            return std::unexpected(std::format("the Return at word {} leaves {} words behind", pc, _depth));
        }
        if (!_landings.empty())
        {
            return std::unexpected(std::format("a jump lands past the Return at word {}", pc));
        }
        return {};
    }

    std::vector<Landing> _landings;
    std::span<const Word> _code;
    uint32_t _slotCount = 0;
    uint32_t _depth = 0;
};

} // namespace

Result Verify(std::span<const Word> code, uint32_t entry, uint32_t slotCount)
{
    return Verifier{code, slotCount}.Run(entry);
}

} // namespace Assisi::Sigil
