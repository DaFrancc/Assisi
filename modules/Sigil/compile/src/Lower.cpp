/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Lower.hpp>

#include <Assisi/Sigil/Evaluate.hpp>

#include <format>
#include <utility>
#include <variant>

namespace Assisi::Sigil::Compile
{

namespace
{

/// Whether the lowered code of an expression is a single push of a value
/// known now, which an operator over such values folds into one push too.
enum class Folded : uint8_t
{
    No,
    Yes,
    Count_,
};

using Result = std::expected<Folded, std::string>;

SlotType SlotTypeOf(Type type)
{
    switch (type.kind)
    {
    case TypeKind::Float:
        return SlotType::Float;
    case TypeKind::Int:
    case TypeKind::Enum:
        return SlotType::Int;
    case TypeKind::Bool:
    case TypeKind::Trigger:
        return SlotType::Bool;
    default:
        return SlotType::None;
    }
}

SlotType SlotTypeOf(std::string_view name)
{
    if (name == TypeNames::kFloat)
    {
        return SlotType::Float;
    }
    if (name == TypeNames::kInt)
    {
        return SlotType::Int;
    }
    return name == TypeNames::kBool ? SlotType::Bool : SlotType::None;
}

bool IsFloat(const Expr &expr)
{
    return expr.type.kind == TypeKind::Float;
}

/// The opcode of a binary operator other than && and ||, on operands of the
/// type @p operand has.
Opcode BinaryOpcode(Operator op, const Expr &operand)
{
    const bool real = IsFloat(operand);
    switch (op)
    {
    case Operator::Equal:
        return real ? Opcode::EqualFloat : Opcode::EqualWord;
    case Operator::NotEqual:
        return real ? Opcode::NotEqualFloat : Opcode::NotEqualWord;
    case Operator::Less:
        return real ? Opcode::LessFloat : Opcode::LessInt;
    case Operator::LessEqual:
        return real ? Opcode::LessEqualFloat : Opcode::LessEqualInt;
    case Operator::Greater:
        return real ? Opcode::GreaterFloat : Opcode::GreaterInt;
    case Operator::GreaterEqual:
        return real ? Opcode::GreaterEqualFloat : Opcode::GreaterEqualInt;
    case Operator::Add:
        return real ? Opcode::AddFloat : Opcode::AddInt;
    case Operator::Subtract:
        return real ? Opcode::SubtractFloat : Opcode::SubtractInt;
    case Operator::Multiply:
        return real ? Opcode::MultiplyFloat : Opcode::MultiplyInt;
    case Operator::Divide:
        return real ? Opcode::DivideFloat : Opcode::DivideInt;
    default:
        return Opcode::RemainderInt;
    }
}

Opcode UnaryOpcode(const Expr &expr)
{
    if (expr.op == Operator::Not)
    {
        return Opcode::Not;
    }
    return IsFloat(expr) ? Opcode::NegateFloat : Opcode::NegateInt;
}

Opcode CallOpcode(CoreFunction function, const Expr &call)
{
    const bool real = IsFloat(call);
    switch (function)
    {
    case CoreFunction::Abs:
        return real ? Opcode::AbsFloat : Opcode::AbsInt;
    case CoreFunction::Min:
        return real ? Opcode::MinFloat : Opcode::MinInt;
    case CoreFunction::Max:
        return real ? Opcode::MaxFloat : Opcode::MaxInt;
    default:
        return real ? Opcode::ClampFloat : Opcode::ClampInt;
    }
}

Opcode PushOpcode(Type type)
{
    switch (SlotTypeOf(type))
    {
    case SlotType::Float:
        return Opcode::PushFloat;
    case SlotType::Bool:
        return Opcode::PushBool;
    default:
        return Opcode::PushInt;
    }
}

Word LiteralWord(const Constant &literal)
{
    if (const float *real = std::get_if<float>(&literal))
    {
        return FromFloat(*real);
    }
    if (const bool *truth = std::get_if<bool>(&literal))
    {
        return FromBool(*truth);
    }
    return FromInt(std::get<int32_t>(literal));
}

class Lowerer
{
  public:
    Lowerer(const Layout &layout, std::vector<Word> &code) : _layout(layout), _code(code)
    {
    }

    Result Lower(const Expr &expr)
    {
        const std::size_t start = _code.size();
        Result lowered = LowerNode(expr);
        if (lowered && *lowered == Folded::Yes && expr.kind != ExprKind::Literal)
        {
            Fold(start, expr.type);
        }
        return lowered;
    }

  private:
    Result LowerNode(const Expr &expr)
    {
        switch (expr.kind)
        {
        case ExprKind::Literal:
            return Push(expr);
        case ExprKind::Param:
            return Load(_layout.ParamSlot(expr.index), expr);
        case ExprKind::Let:
            return Load(_layout.LetSlot(expr.index), expr);
        case ExprKind::Widen:
            return Apply(expr, Opcode::IntToFloat);
        case ExprKind::Unary:
            return Apply(expr, UnaryOpcode(expr));
        case ExprKind::Call:
            return LowerCall(expr);
        default:
            if (expr.op == Operator::And || expr.op == Operator::Or)
            {
                return LowerShortCircuit(expr);
            }
            return Apply(expr, BinaryOpcode(expr.op, expr.operands[0]));
        }
    }

    Result Push(const Expr &expr)
    {
        if (SlotTypeOf(expr.type) == SlotType::None)
        {
            return std::unexpected("only numbers and bools can be worked out while the game runs");
        }
        _code.push_back(MakeWord(PushOpcode(expr.type)));
        _code.push_back(LiteralWord(expr.literal));
        return Folded::Yes;
    }

    Result Load(uint32_t slot, const Expr &expr)
    {
        if (SlotTypeOf(expr.type) == SlotType::None)
        {
            return std::unexpected("only numbers and bools can be worked out while the game runs");
        }
        if (slot > kMaxOperand)
        {
            return std::unexpected("the file has more values than bytecode can address");
        }
        _code.push_back(MakeWord(Opcode::Load, slot));
        return Folded::No;
    }

    /// A vocabulary function reads the slot the engine writes its value to; a
    /// core function runs on its operands.
    Result LowerCall(const Expr &expr)
    {
        const uint32_t coreCount = static_cast<uint32_t>(CoreFunction::Count_);
        if (expr.index >= coreCount)
        {
            return Load(_layout.ImpliedSlot(expr.index - coreCount), expr);
        }
        return Apply(expr, CallOpcode(static_cast<CoreFunction>(expr.index), expr));
    }

    /// The operands, then @p opcode on them; folded when every operand was.
    Result Apply(const Expr &expr, Opcode opcode)
    {
        Folded folded = Folded::Yes;
        for (const Expr &operand : expr.operands)
        {
            Result lowered = Lower(operand);
            if (!lowered)
            {
                return lowered;
            }
            if (*lowered == Folded::No)
            {
                folded = Folded::No;
            }
        }
        _code.push_back(MakeWord(opcode));
        return folded;
    }

    /// The left side, a jump past the right side that keeps the left's value
    /// when it decides the result, then the right side.
    Result LowerShortCircuit(const Expr &expr)
    {
        const Result left = Lower(expr.operands[0]);
        if (!left)
        {
            return left;
        }
        const std::size_t jump = _code.size();
        _code.push_back(0);
        const Result right = Lower(expr.operands[1]);
        if (!right)
        {
            return right;
        }
        const std::size_t distance = _code.size() - jump - 1;
        if (distance > kMaxOperand)
        {
            return std::unexpected("an expression is too long for bytecode to jump over");
        }
        const Opcode opcode = expr.op == Operator::And ? Opcode::AndJump : Opcode::OrJump;
        _code[jump] = MakeWord(opcode, static_cast<uint32_t>(distance));
        return *left == Folded::Yes && *right == Folded::Yes ? Folded::Yes : Folded::No;
    }

    /// Runs the code from @p start, which reads no slot, and puts a push of
    /// its value in its place.
    void Fold(std::size_t start, Type type)
    {
        _code.push_back(MakeWord(Opcode::Return));
        const Word value = Evaluate(_code, static_cast<uint32_t>(start), {});
        _code.resize(start);
        _code.push_back(MakeWord(PushOpcode(type)));
        _code.push_back(value);
    }

    const Layout &_layout;
    std::vector<Word> &_code;
};

} // namespace

Layout MakeLayout(const Program &program, const Vocabulary &vocabulary)
{
    Layout layout;
    for (const Param &param : program.params)
    {
        layout.slots.push_back(Slot{.name = param.name, .type = SlotTypeOf(param.type), .kind = SlotKind::Param});
    }
    for (const FunctionSpec &function : vocabulary.functions)
    {
        layout.slots.push_back(
            Slot{.name = function.name, .type = SlotTypeOf(function.result), .kind = SlotKind::Implied});
    }
    for (const Let &let : program.lets)
    {
        layout.slots.push_back(Slot{.name = let.name, .type = SlotTypeOf(let.type), .kind = SlotKind::Let});
    }
    layout.paramCount = static_cast<uint32_t>(program.params.size());
    layout.impliedCount = static_cast<uint32_t>(vocabulary.functions.size());
    layout.letCount = static_cast<uint32_t>(program.lets.size());
    return layout;
}

std::expected<uint32_t, std::string> LowerExpression(const Layout &layout, const Expr &expr, std::vector<Word> &code)
{
    const uint32_t entry = static_cast<uint32_t>(code.size());
    Lowerer lowerer{layout, code};
    if (Result lowered = lowerer.Lower(expr); !lowered)
    {
        code.resize(entry);
        return std::unexpected(std::move(lowered.error()));
    }
    code.push_back(MakeWord(Opcode::Return));
    return entry;
}

std::expected<std::vector<uint32_t>, std::string> LowerLets(const Program &program, const Layout &layout,
                                                            std::vector<Word> &code)
{
    std::vector<uint32_t> entries;
    for (const Let &let : program.lets)
    {
        std::expected<uint32_t, std::string> entry = LowerExpression(layout, let.value, code);
        if (!entry)
        {
            return std::unexpected(std::format("let \"{}\": {}", let.name, entry.error()));
        }
        entries.push_back(*entry);
    }
    return entries;
}

std::expected<LoweredGraph, std::string> LowerGraph(const Layout &layout, const Block &root, std::vector<Word> &code)
{
    LoweredGraph lowered;
    lowered.blocks.push_back(&root);
    lowered.graph.nodes.push_back(Node{.name = {}});
    // Breadth first, so each node's states sit together, after it.
    for (std::size_t index = 0; index < lowered.blocks.size(); ++index)
    {
        const Block &block = *lowered.blocks[index];
        Node &node = lowered.graph.nodes[index];
        node.firstChild = static_cast<uint32_t>(lowered.blocks.size());
        node.childCount = static_cast<uint32_t>(block.children.size());
        node.firstTransition = static_cast<uint32_t>(lowered.graph.transitions.size());
        node.transitionCount = static_cast<uint32_t>(block.transitions.size());
        for (const Block &child : block.children)
        {
            lowered.blocks.push_back(&child);
            lowered.graph.nodes.push_back(Node{.name = child.name});
        }
        for (const Transition &transition : block.transitions)
        {
            std::expected<uint32_t, std::string> condition = LowerExpression(layout, transition.condition, code);
            if (!condition)
            {
                return std::unexpected(std::format("a transition in \"{}\": {}", block.name, condition.error()));
            }
            GraphTransition lowerTransition{.sources = transition.sources,
                                            .triggersRead = {},
                                            .condition = *condition,
                                            .target = transition.target};
            for (const uint32_t param : transition.triggersRead)
            {
                lowerTransition.triggersRead.push_back(layout.ParamSlot(param));
            }
            lowered.graph.transitions.push_back(std::move(lowerTransition));
            lowered.transitions.push_back(&transition);
        }
    }
    return lowered;
}

} // namespace Assisi::Sigil::Compile
