/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Bytecode.hpp>

#include <format>
#include <utility>

namespace Assisi::Sigil
{

namespace
{

/// The row for an instruction with no operand.
constexpr OpcodeInfo Plain(std::string_view name, uint8_t pops, uint8_t pushes)
{
    return OpcodeInfo{.name = name, .operand = OperandKind::None, .pops = pops, .pushes = pushes};
}

/// The row for a push of a value of the type named by @p name.
constexpr OpcodeInfo Push(std::string_view name)
{
    return OpcodeInfo{.name = name, .operand = OperandKind::Immediate, .pops = 0, .pushes = 1};
}

/// The row for a jump that drops the top when it falls through.
constexpr OpcodeInfo Jump(std::string_view name)
{
    return OpcodeInfo{.name = name, .operand = OperandKind::Jump, .pops = 1, .pushes = 0};
}

std::string Immediate(Opcode opcode, Word value)
{
    switch (opcode)
    {
    case Opcode::PushInt:
        return std::format("{}", ToInt(value));
    case Opcode::PushFloat:
        return std::format("{}", ToFloat(value));
    default:
        return ToBool(value) ? "true" : "false";
    }
}

} // namespace

OpcodeInfo Describe(Opcode opcode)
{
    switch (opcode)
    {
    case Opcode::PushInt:
        return Push("PushInt");
    case Opcode::PushFloat:
        return Push("PushFloat");
    case Opcode::PushBool:
        return Push("PushBool");
    case Opcode::Load:
        return OpcodeInfo{.name = "Load", .operand = OperandKind::Slot, .pops = 0, .pushes = 1};
    case Opcode::IntToFloat:
        return Plain("IntToFloat", 1, 1);
    case Opcode::NegateInt:
        return Plain("NegateInt", 1, 1);
    case Opcode::NegateFloat:
        return Plain("NegateFloat", 1, 1);
    case Opcode::Not:
        return Plain("Not", 1, 1);
    case Opcode::AddInt:
        return Plain("AddInt", 2, 1);
    case Opcode::AddFloat:
        return Plain("AddFloat", 2, 1);
    case Opcode::SubtractInt:
        return Plain("SubtractInt", 2, 1);
    case Opcode::SubtractFloat:
        return Plain("SubtractFloat", 2, 1);
    case Opcode::MultiplyInt:
        return Plain("MultiplyInt", 2, 1);
    case Opcode::MultiplyFloat:
        return Plain("MultiplyFloat", 2, 1);
    case Opcode::DivideInt:
        return Plain("DivideInt", 2, 1);
    case Opcode::DivideFloat:
        return Plain("DivideFloat", 2, 1);
    case Opcode::RemainderInt:
        return Plain("RemainderInt", 2, 1);
    case Opcode::EqualWord:
        return Plain("EqualWord", 2, 1);
    case Opcode::EqualFloat:
        return Plain("EqualFloat", 2, 1);
    case Opcode::NotEqualWord:
        return Plain("NotEqualWord", 2, 1);
    case Opcode::NotEqualFloat:
        return Plain("NotEqualFloat", 2, 1);
    case Opcode::LessInt:
        return Plain("LessInt", 2, 1);
    case Opcode::LessFloat:
        return Plain("LessFloat", 2, 1);
    case Opcode::LessEqualInt:
        return Plain("LessEqualInt", 2, 1);
    case Opcode::LessEqualFloat:
        return Plain("LessEqualFloat", 2, 1);
    case Opcode::GreaterInt:
        return Plain("GreaterInt", 2, 1);
    case Opcode::GreaterFloat:
        return Plain("GreaterFloat", 2, 1);
    case Opcode::GreaterEqualInt:
        return Plain("GreaterEqualInt", 2, 1);
    case Opcode::GreaterEqualFloat:
        return Plain("GreaterEqualFloat", 2, 1);
    case Opcode::AndJump:
        return Jump("AndJump");
    case Opcode::OrJump:
        return Jump("OrJump");
    case Opcode::AbsInt:
        return Plain("AbsInt", 1, 1);
    case Opcode::AbsFloat:
        return Plain("AbsFloat", 1, 1);
    case Opcode::MinInt:
        return Plain("MinInt", 2, 1);
    case Opcode::MinFloat:
        return Plain("MinFloat", 2, 1);
    case Opcode::MaxInt:
        return Plain("MaxInt", 2, 1);
    case Opcode::MaxFloat:
        return Plain("MaxFloat", 2, 1);
    case Opcode::ClampInt:
        return Plain("ClampInt", 3, 1);
    case Opcode::ClampFloat:
        return Plain("ClampFloat", 3, 1);
    case Opcode::Return:
        return Plain("Return", 1, 0);
    case Opcode::Count_:
        break;
    }
    std::unreachable();
}

std::string Disassemble(std::span<const Word> code, uint32_t entry)
{
    std::string text;
    for (uint32_t pc = entry; pc < code.size(); ++pc)
    {
        const Opcode opcode = OpcodeOf(code[pc]);
        const OpcodeInfo info = Describe(opcode);
        text += std::format("{}: {}", pc - entry, info.name);
        switch (info.operand)
        {
        case OperandKind::Slot:
            text += std::format(" {}", OperandOf(code[pc]));
            break;
        case OperandKind::Jump:
            text += std::format(" -> {}", pc + 1 + OperandOf(code[pc]) - entry);
            break;
        case OperandKind::Immediate:
            ++pc;
            text += " " + Immediate(opcode, code[pc]);
            break;
        default:
            break;
        }
        text += '\n';
        if (opcode == Opcode::Return)
        {
            break;
        }
    }
    return text;
}

} // namespace Assisi::Sigil
