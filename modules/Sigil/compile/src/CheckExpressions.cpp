/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Checker.hpp"

#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <utility>

namespace Assisi::Sigil::Compile::Detail
{

namespace
{

constexpr Type kError{.index = 0, .kind = TypeKind::Error};
constexpr Type kBool{.index = 0, .kind = TypeKind::Bool};
constexpr Type kInt{.index = 0, .kind = TypeKind::Int};
constexpr Type kFloat{.index = 0, .kind = TypeKind::Float};

Expr Failed(SourceLocation where)
{
    Expr expr;
    expr.where = where;
    expr.type = kError;
    return expr;
}

Expr Literal(Constant value, Type type, SourceLocation where)
{
    Expr expr;
    expr.literal = std::move(value);
    expr.where = where;
    expr.type = type;
    return expr;
}

bool IsNumeric(Type type)
{
    return type.kind == TypeKind::Int || type.kind == TypeKind::Float;
}

bool IsBoolish(Type type)
{
    return type.kind == TypeKind::Bool || type.kind == TypeKind::Trigger;
}

bool IsError(Type type)
{
    return type.kind == TypeKind::Error;
}

Expr Widened(Expr expr)
{
    if (expr.type.kind != TypeKind::Int)
    {
        return expr;
    }
    Expr widened;
    widened.where = expr.where;
    widened.type = kFloat;
    widened.kind = ExprKind::Widen;
    widened.operands.push_back(std::move(expr));
    return widened;
}

Operator ToOperator(TokenKind kind, bool unary)
{
    switch (kind)
    {
    case TokenKind::Or:
        return Operator::Or;
    case TokenKind::And:
        return Operator::And;
    case TokenKind::Equal:
        return Operator::Equal;
    case TokenKind::NotEqual:
        return Operator::NotEqual;
    case TokenKind::Less:
        return Operator::Less;
    case TokenKind::LessEqual:
        return Operator::LessEqual;
    case TokenKind::Greater:
        return Operator::Greater;
    case TokenKind::GreaterEqual:
        return Operator::GreaterEqual;
    case TokenKind::Plus:
        return Operator::Add;
    case TokenKind::Minus:
        return unary ? Operator::Negate : Operator::Subtract;
    case TokenKind::Star:
        return Operator::Multiply;
    case TokenKind::Slash:
        return Operator::Divide;
    case TokenKind::Percent:
        return Operator::Remainder;
    case TokenKind::Not:
        return Operator::Not;
    default:
        return Operator::Count_;
    }
}

Expr CheckLiteral(const Syntax::Expr &syntax)
{
    switch (syntax.kind)
    {
    case Syntax::ExprKind::Int:
    {
        int32_t value = 0;
        std::from_chars(syntax.text.data(), syntax.text.data() + syntax.text.size(), value);
        return Literal(value, kInt, syntax.where);
    }
    case Syntax::ExprKind::Float:
    {
        float value = 0.f;
        std::from_chars(syntax.text.data(), syntax.text.data() + syntax.text.size(), value);
        return Literal(value, kFloat, syntax.where);
    }
    case Syntax::ExprKind::String:
        return Literal(syntax.text, Type{.index = 0, .kind = TypeKind::String}, syntax.where);
    default:
        return Literal(syntax.text == "true", kBool, syntax.where);
    }
}

void MarkUsed(Checker &checker, Symbol &symbol)
{
    symbol.used = true;
    if (symbol.import >= 0)
    {
        checker.imports[static_cast<std::size_t>(symbol.import)].used = true;
    }
}

void ReportUnknown(Checker &checker, const Syntax::Expr &syntax)
{
    if (checker.refused.contains(syntax.text))
    {
        return;
    }
    const std::unordered_map<std::string, SourceLocation>::const_iterator later =
        checker.declaredLater.find(syntax.text);
    if (later != checker.declaredLater.end())
    {
        Fail(checker, syntax.where,
             std::format("\"{}\" is used before it is declared, on line {}", syntax.text, later->second.line),
             "move the declaration above this line");
        return;
    }
    const std::vector<std::string_view> names = SymbolNames(checker);
    Fail(checker, syntax.where, std::format("unknown name \"{}\"{}", syntax.text, DidYouMean(names, syntax.text)));
}

Expr CheckName(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    const std::unordered_map<std::string, Symbol>::iterator found = checker.symbols.find(syntax.text);
    if (found == checker.symbols.end())
    {
        ReportUnknown(checker, syntax);
        return Failed(syntax.where);
    }
    Symbol &symbol = found->second;
    MarkUsed(checker, symbol);
    const bool readsFrame = symbol.kind == SymbolKind::Param || symbol.kind == SymbolKind::Let;
    if (mode == ExprMode::Constant && readsFrame)
    {
        Fail(checker, syntax.where,
             std::format("a const can't use \"{}\", which is a {}", syntax.text,
                         symbol.kind == SymbolKind::Param ? "param" : "let"),
             "a const is worked out once, when the file cooks, so it can only use numbers, strings, enum values "
             "and other consts; to use this, make it a let");
        return Failed(syntax.where);
    }
    Expr expr;
    expr.where = syntax.where;
    expr.index = symbol.index;
    switch (symbol.kind)
    {
    case SymbolKind::Param:
        expr.kind = ExprKind::Param;
        expr.type = checker.program.params[symbol.index].type;
        return expr;
    case SymbolKind::Let:
        expr.kind = ExprKind::Let;
        expr.type = checker.program.lets[symbol.index].type;
        return expr;
    case SymbolKind::Const:
        return Literal(checker.program.consts[symbol.index].value, checker.program.consts[symbol.index].type,
                       syntax.where);
    case SymbolKind::Enum:
        Fail(checker, syntax.where, std::format("\"{}\" is an enum, which is a type, not a value", syntax.text),
             std::format("write one of its values, like {}.{}", syntax.text,
                         checker.program.enums[symbol.index].values.front()));
        return Failed(syntax.where);
    default:
        Fail(checker, syntax.where, std::format("\"{}\" is a function, not a value", syntax.text),
             std::format("call it: {}(...)", syntax.text));
        return Failed(syntax.where);
    }
}

Expr CheckMember(Checker &checker, const Syntax::Expr &syntax)
{
    const std::unordered_map<std::string, Symbol>::iterator found = checker.symbols.find(syntax.text);
    if (found == checker.symbols.end())
    {
        ReportUnknown(checker, syntax);
        return Failed(syntax.where);
    }
    if (found->second.kind != SymbolKind::Enum)
    {
        Fail(checker, syntax.where, std::format("\"{}\" isn't an enum, so it has no values", syntax.text));
        return Failed(syntax.where);
    }
    MarkUsed(checker, found->second);
    const uint32_t enumIndex = found->second.index;
    const std::vector<std::string> &values = checker.program.enums[enumIndex].values;
    const std::vector<std::string>::const_iterator value = std::ranges::find(values, syntax.member);
    if (value == values.end())
    {
        const std::vector<std::string_view> names{values.begin(), values.end()};
        Fail(checker, syntax.where,
             std::format("{} has no value \"{}\"{}", syntax.text, syntax.member, DidYouMean(names, syntax.member)));
        return Failed(syntax.where);
    }
    return Literal(static_cast<int32_t>(value - values.begin()), Type{.index = enumIndex, .kind = TypeKind::Enum},
                   syntax.where);
}

Expr CheckUnary(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    Expr operand = CheckExpression(checker, syntax.operands[0], mode);
    Expr expr;
    expr.where = syntax.where;
    expr.kind = ExprKind::Unary;
    expr.op = ToOperator(syntax.op, true);
    if (IsError(operand.type))
    {
        return Failed(syntax.where);
    }
    const bool fits = expr.op == Operator::Not ? IsBoolish(operand.type) : IsNumeric(operand.type);
    if (!fits)
    {
        Fail(checker, syntax.where,
             std::format("{} needs {}, got {}", Describe(syntax.op), expr.op == Operator::Not ? "a bool" : "a number",
                         TypeName(checker, operand.type)));
        return Failed(syntax.where);
    }
    expr.type = expr.op == Operator::Not ? kBool : operand.type;
    expr.operands.push_back(std::move(operand));
    return expr;
}

/// The result type of `left op right`, widening a number operand where the
/// other is a float. Error when the operator can't take these types.
Type BinaryType(Operator op, Expr &left, Expr &right)
{
    const bool numbers = IsNumeric(left.type) && IsNumeric(right.type);
    if (numbers && (left.type.kind == TypeKind::Float || right.type.kind == TypeKind::Float))
    {
        left = Widened(std::move(left));
        right = Widened(std::move(right));
    }
    switch (op)
    {
    case Operator::Or:
    case Operator::And:
        return IsBoolish(left.type) && IsBoolish(right.type) ? kBool : kError;
    case Operator::Equal:
    case Operator::NotEqual:
    {
        const bool sameEnum = left.type.kind == TypeKind::Enum && left.type == right.type;
        return numbers || sameEnum || (IsBoolish(left.type) && IsBoolish(right.type)) ? kBool : kError;
    }
    case Operator::Less:
    case Operator::LessEqual:
    case Operator::Greater:
    case Operator::GreaterEqual:
        return numbers ? kBool : kError;
    case Operator::Remainder:
        return left.type.kind == TypeKind::Int && right.type.kind == TypeKind::Int ? kInt : kError;
    default:
        return numbers ? left.type : kError;
    }
}

Expr CheckBinary(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    Expr left = CheckExpression(checker, syntax.operands[0], mode);
    Expr right = CheckExpression(checker, syntax.operands[1], mode);
    if (IsError(left.type) || IsError(right.type))
    {
        return Failed(syntax.where);
    }
    const std::string leftName = TypeName(checker, left.type);
    const std::string rightName = TypeName(checker, right.type);
    Expr expr;
    expr.where = syntax.where;
    expr.kind = ExprKind::Binary;
    expr.op = ToOperator(syntax.op, false);
    expr.type = BinaryType(expr.op, left, right);
    if (IsError(expr.type))
    {
        Fail(checker, syntax.where,
             std::format("{} can't take {} and {}", Describe(syntax.op), leftName, rightName));
        return Failed(syntax.where);
    }
    expr.operands.push_back(std::move(left));
    expr.operands.push_back(std::move(right));
    return expr;
}

const FunctionSpec *FindFunction(const Checker &checker, std::string_view name, uint32_t &index)
{
    for (std::size_t i = 0; i < checker.functions.size(); ++i)
    {
        if (checker.functions[i].name == name)
        {
            index = static_cast<uint32_t>(i);
            return &checker.functions[i];
        }
    }
    return nullptr;
}

/// The type a `numeric` parameter or result stands for in one call: float if
/// any numeric argument is a float.
Type NumericType(const FunctionSpec &function, std::span<const Expr> arguments)
{
    for (std::size_t i = 0; i < arguments.size(); ++i)
    {
        if (function.parameters[i] == TypeNames::kNumeric && arguments[i].type.kind == TypeKind::Float)
        {
            return kFloat;
        }
    }
    return kInt;
}

/// The type a parameter or result named @p name stands for in one call.
std::optional<Type> CallType(const Checker &checker, std::string_view name, Type numeric)
{
    if (name == TypeNames::kNumeric)
    {
        return numeric;
    }
    if (name == TypeNames::kFloat)
    {
        return kFloat;
    }
    if (name == TypeNames::kInt)
    {
        return kInt;
    }
    if (name == TypeNames::kBool)
    {
        return kBool;
    }
    for (std::size_t i = 0; i < checker.vocabulary.types.size(); ++i)
    {
        if (checker.vocabulary.types[i].name == name)
        {
            return Type{.index = static_cast<uint32_t>(i), .kind = TypeKind::Vocabulary};
        }
    }
    return std::nullopt;
}

Expr CheckCall(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    uint32_t index = 0;
    const FunctionSpec *function = FindFunction(checker, syntax.text, index);
    if (function == nullptr)
    {
        std::vector<std::string_view> names;
        for (const FunctionSpec &candidate : checker.functions)
        {
            names.push_back(candidate.name);
        }
        Fail(checker, syntax.where,
             std::format("unknown function \"{}\"{}", syntax.text, DidYouMean(names, syntax.text)));
        return Failed(syntax.where);
    }
    if (mode == ExprMode::Constant && index >= CoreFunctions().size())
    {
        Fail(checker, syntax.where, std::format("a const can't call {}()", syntax.text),
             std::format("{}() is only known while the game runs, and a const is worked out when the file cooks; "
                         "make this a let, or use only abs, min, max and clamp",
                         syntax.text));
        return Failed(syntax.where);
    }
    if (syntax.operands.size() != function->parameters.size())
    {
        Fail(checker, syntax.where,
             std::format("{} takes {} values, got {}", syntax.text, function->parameters.size(), syntax.operands.size()));
        return Failed(syntax.where);
    }
    Expr expr;
    expr.where = syntax.where;
    expr.kind = ExprKind::Call;
    expr.index = index;
    for (const Syntax::Expr &argument : syntax.operands)
    {
        expr.operands.push_back(CheckExpression(checker, argument, mode));
    }
    if (std::ranges::any_of(expr.operands, [](const Expr &operand) { return IsError(operand.type); }))
    {
        return Failed(syntax.where);
    }
    const Type numeric = NumericType(*function, expr.operands);
    for (std::size_t i = 0; i < expr.operands.size(); ++i)
    {
        const bool takesNumber = function->parameters[i] == TypeNames::kNumeric;
        if (takesNumber && !IsNumeric(expr.operands[i].type))
        {
            Fail(checker, expr.operands[i].where,
                 std::format("{} takes numbers, got {}", syntax.text, TypeName(checker, expr.operands[i].type)));
            return Failed(syntax.where);
        }
        const std::optional<Type> wanted = CallType(checker, function->parameters[i], numeric);
        expr.operands[i] = Coerce(checker, std::move(expr.operands[i]), wanted.value_or(kError));
        if (IsError(expr.operands[i].type))
        {
            return Failed(syntax.where);
        }
    }
    expr.type = CallType(checker, function->result, numeric).value_or(kError);
    return expr;
}

} // namespace

Expr CheckExpression(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    switch (syntax.kind)
    {
    case Syntax::ExprKind::Name:
        return CheckName(checker, syntax, mode);
    case Syntax::ExprKind::Member:
        return CheckMember(checker, syntax);
    case Syntax::ExprKind::Unary:
        return CheckUnary(checker, syntax, mode);
    case Syntax::ExprKind::Binary:
        return CheckBinary(checker, syntax, mode);
    case Syntax::ExprKind::Call:
        return CheckCall(checker, syntax, mode);
    default:
        return CheckLiteral(syntax);
    }
}

Expr Coerce(Checker &checker, Expr value, Type wanted)
{
    if (IsError(value.type) || IsError(wanted) || value.type == wanted)
    {
        return value;
    }
    if (wanted.kind == TypeKind::Float && value.type.kind == TypeKind::Int)
    {
        return Widened(std::move(value));
    }
    if (wanted.kind == TypeKind::Bool && value.type.kind == TypeKind::Trigger)
    {
        return value;
    }
    const bool stringLiteral = value.type.kind == TypeKind::String && value.kind == ExprKind::Literal;
    if (wanted.kind == TypeKind::Vocabulary && stringLiteral)
    {
        const ValueType &type = checker.vocabulary.types[wanted.index];
        const std::expected<void, std::string> accepted =
            type.validate ? type.validate(std::get<std::string>(value.literal)) : std::expected<void, std::string>{};
        if (!accepted)
        {
            Fail(checker, value.where, accepted.error());
            return Failed(value.where);
        }
        value.type = wanted;
        return value;
    }
    Fail(checker, value.where,
         std::format("expected {}, got {}", TypeName(checker, wanted), TypeName(checker, value.type)));
    return Failed(value.where);
}

bool ReadsWhenOnly(const Checker &checker, const Expr &expr)
{
    if (expr.kind == ExprKind::Param && expr.type.kind == TypeKind::Trigger)
    {
        return true;
    }
    if (expr.kind == ExprKind::Let && checker.program.lets[expr.index].whenOnly)
    {
        return true;
    }
    if (expr.kind == ExprKind::Call && checker.functions[expr.index].use == FunctionUse::WhenOnly)
    {
        return true;
    }
    return std::ranges::any_of(expr.operands,
                               [&checker](const Expr &operand) { return ReadsWhenOnly(checker, operand); });
}

void CollectTriggers(const Checker &checker, const Expr &expr, std::vector<uint32_t> &triggers)
{
    if (expr.kind == ExprKind::Param && expr.type.kind == TypeKind::Trigger &&
        std::ranges::find(triggers, expr.index) == triggers.end())
    {
        triggers.push_back(expr.index);
    }
    if (expr.kind == ExprKind::Let)
    {
        CollectTriggers(checker, checker.program.lets[expr.index].value, triggers);
    }
    for (const Expr &operand : expr.operands)
    {
        CollectTriggers(checker, operand, triggers);
    }
}

} // namespace Assisi::Sigil::Compile::Detail
