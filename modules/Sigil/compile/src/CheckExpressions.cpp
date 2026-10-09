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

Expr Failed(Span span)
{
    Expr expr;
    expr.span = span;
    expr.type = kError;
    return expr;
}

Expr Literal(Constant value, Type type, Span span)
{
    Expr expr;
    expr.literal = std::move(value);
    expr.span = span;
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
    widened.span = expr.span;
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
        return Literal(value, kInt, syntax.extent);
    }
    case Syntax::ExprKind::Float:
    {
        float value = 0.f;
        std::from_chars(syntax.text.data(), syntax.text.data() + syntax.text.size(), value);
        return Literal(value, kFloat, syntax.extent);
    }
    case Syntax::ExprKind::String:
        return Literal(syntax.text, Type{.index = 0, .kind = TypeKind::String}, syntax.extent);
    default:
        return Literal(syntax.text == "true", kBool, syntax.extent);
    }
}

void MarkUsed(Checker &checker, Symbol &symbol)
{
    symbol.used = true;
    if (symbol.from >= 0)
    {
        checker.imports[static_cast<std::size_t>(symbol.from)].used = true;
    }
}

/// The label under an unknown name: the closest one it might be, or that it
/// names nothing.
std::string UnknownLabel(std::span<const std::string_view> names, std::string_view name)
{
    const std::optional<std::string_view> closest = ClosestName(names, name);
    if (closest.has_value())
    {
        return std::format("did you mean \"{}\"?", *closest);
    }
    return "not declared in this file";
}

std::string_view TrimSpace(std::string_view text)
{
    const std::size_t first = text.find_first_not_of(" \t");
    const std::size_t last = text.find_last_not_of(" \t");
    return first == std::string_view::npos ? std::string_view{} : text.substr(first, last - first + 1);
}

/// Moves the declaration on @p declarationLine to just above @p useLine, when
/// it's a whole line of its own; nothing when it shares its line.
std::optional<Suggestion> MoveAbove(const Checker &checker, uint32_t declarationLine, uint32_t useLine)
{
    const std::string_view declaration = TrimSpace(LineOf(checker, declarationLine));
    const bool ownLine = (declaration.starts_with("let ") || declaration.starts_with("const ")) &&
                         declaration.ends_with(';') && std::ranges::count(declaration, ';') == 1;
    if (!ownLine || declarationLine == useLine)
    {
        return std::nullopt;
    }
    const std::string_view use = LineOf(checker, useLine);
    const std::string_view indent = use.substr(0, use.find_first_not_of(" \t"));
    return Suggestion{
        .message = "move the declaration above where it's used",
        .edits = {Edit{.text = std::string{indent} + std::string{declaration},
                       .line = std::string{use},
                       .span = Span{.where = SourceLocation{.line = useLine, .column = 1}, .length = 0},
                       .kind = EditKind::InsertBefore},
                  Edit{.text = {},
                       .line = std::string{LineOf(checker, declarationLine)},
                       .span = Span{.where = SourceLocation{.line = declarationLine, .column = 1}, .length = 0},
                       .kind = EditKind::Delete}}};
}

void ReportUnknown(Checker &checker, const Syntax::Expr &syntax)
{
    if (checker.refused.contains(syntax.text))
    {
        return;
    }
    const std::unordered_map<std::string, Span>::const_iterator later = checker.declaredLater.find(syntax.text);
    if (later != checker.declaredLater.end())
    {
        Diagnostic &error =
            Fail(checker, syntax.anchor, std::format("\"{}\" is used before it is declared", syntax.text), "used here");
        Relate(error, later->second, "declared here, further down");
        std::optional<Suggestion> move = MoveAbove(checker, later->second.where.line, syntax.anchor.where.line);
        if (move.has_value())
        {
            error.suggestions.push_back(std::move(*move));
        }
        else
        {
            error.help = "move the declaration above where it's used";
        }
        return;
    }
    const std::vector<std::string_view> names = SymbolNames(checker);
    Fail(checker, syntax.anchor, std::format("unknown name \"{}\"", syntax.text), UnknownLabel(names, syntax.text));
}

Expr CheckName(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    const std::unordered_map<std::string, Symbol>::iterator found = checker.symbols.find(syntax.text);
    if (found == checker.symbols.end())
    {
        ReportUnknown(checker, syntax);
        return Failed(syntax.extent);
    }
    Symbol &symbol = found->second;
    MarkUsed(checker, symbol);
    const bool readsFrame = symbol.kind == SymbolKind::Param || symbol.kind == SymbolKind::Let;
    if (mode == ExprMode::Constant && readsFrame)
    {
        const std::string_view kind = symbol.kind == SymbolKind::Param ? "param" : "let";
        Diagnostic &error = Fail(checker, syntax.anchor, std::format("a const can't use \"{}\"", syntax.text),
                                 std::format("a {}, which changes while the game runs", kind));
        Relate(error, symbol.span, std::format("declared as a {} here", kind));
        error.help = "a const is worked out once, when the file cooks";
        if (checker.declaring != nullptr)
        {
            error.suggestions.push_back(SwapKeyword(checker, *checker.declaring, "let"));
        }
        return Failed(syntax.extent);
    }
    Expr expr;
    expr.span = syntax.extent;
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
                       syntax.extent);
    case SymbolKind::Enum:
    {
        Diagnostic &error = Fail(checker, syntax.anchor, std::format("expected a value, found enum \"{}\"", syntax.text),
                                 "an enum is a type, not a value");
        error.help = std::format("write one of its values, like {}.{}", syntax.text,
                                 checker.program.enums[symbol.index].values.front());
        return Failed(syntax.extent);
    }
    default:
    {
        Diagnostic &error = Fail(checker, syntax.anchor,
                                 std::format("expected a value, found function \"{}\"", syntax.text), "not called");
        error.help = std::format("call it: {}(...)", syntax.text);
        return Failed(syntax.extent);
    }
    }
}

Expr CheckMember(Checker &checker, const Syntax::Expr &syntax)
{
    const std::unordered_map<std::string, Symbol>::iterator found = checker.symbols.find(syntax.text);
    if (found == checker.symbols.end())
    {
        ReportUnknown(checker, syntax);
        return Failed(syntax.extent);
    }
    if (found->second.kind != SymbolKind::Enum)
    {
        Fail(checker, syntax.anchor, std::format("\"{}\" isn't an enum", syntax.text), "so it has no values");
        return Failed(syntax.extent);
    }
    MarkUsed(checker, found->second);
    const uint32_t enumIndex = found->second.index;
    const std::vector<std::string> &values = checker.program.enums[enumIndex].values;
    const std::vector<std::string>::const_iterator value = std::ranges::find(values, syntax.member);
    if (value == values.end())
    {
        const std::vector<std::string_view> names{values.begin(), values.end()};
        const std::optional<std::string_view> closest = ClosestName(names, syntax.member);
        Diagnostic &error = Fail(checker, syntax.anchor, std::format("{} has no value \"{}\"", syntax.text, syntax.member),
                                 closest.has_value() ? std::format("did you mean {}.{}?", syntax.text, *closest)
                                                     : std::string{"no such value"});
        Relate(error, found->second.span,
               found->second.from >= 0 ? std::format("{} is imported here", syntax.text)
                                       : std::format("{} is declared here", syntax.text));
        return Failed(syntax.extent);
    }
    return Literal(static_cast<int32_t>(value - values.begin()), Type{.index = enumIndex, .kind = TypeKind::Enum},
                   syntax.extent);
}

Expr CheckUnary(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    Expr operand = CheckExpression(checker, syntax.operands[0], mode);
    Expr expr;
    expr.span = syntax.extent;
    expr.kind = ExprKind::Unary;
    expr.op = ToOperator(syntax.op, true);
    if (IsError(operand.type))
    {
        return Failed(syntax.extent);
    }
    const bool fits = expr.op == Operator::Not ? IsBoolish(operand.type) : IsNumeric(operand.type);
    if (!fits)
    {
        Diagnostic &error = Fail(checker, syntax.anchor,
                                 std::format("{} needs {}", Describe(syntax.op),
                                             expr.op == Operator::Not ? "a bool" : "a number"),
                                 std::format("expected {}", expr.op == Operator::Not ? "a bool" : "a number"));
        Relate(error, operand.span, std::format("this is {}", TypeName(checker, operand.type)));
        return Failed(syntax.extent);
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

/// What @p op takes, for the label under it when it's given the wrong types.
std::string_view Takes(Operator op)
{
    switch (op)
    {
    case Operator::Or:
    case Operator::And:
        return "takes two bools";
    case Operator::Equal:
    case Operator::NotEqual:
        return "compares two values of one type";
    case Operator::Remainder:
        return "takes two ints";
    default:
        return "takes two numbers";
    }
}

Expr CheckBinary(Checker &checker, const Syntax::Expr &syntax, ExprMode mode)
{
    Expr left = CheckExpression(checker, syntax.operands[0], mode);
    Expr right = CheckExpression(checker, syntax.operands[1], mode);
    if (IsError(left.type) || IsError(right.type))
    {
        return Failed(syntax.extent);
    }
    const std::string leftName = TypeName(checker, left.type);
    const std::string rightName = TypeName(checker, right.type);
    const Span leftSpan = left.span;
    const Span rightSpan = right.span;
    Expr expr;
    expr.span = syntax.extent;
    expr.kind = ExprKind::Binary;
    expr.op = ToOperator(syntax.op, false);
    expr.type = BinaryType(expr.op, left, right);
    if (IsError(expr.type))
    {
        Diagnostic &error = Fail(checker, syntax.anchor,
                                 std::format("{} can't take {} and {}", Describe(syntax.op), leftName, rightName),
                                 std::string{Takes(expr.op)});
        Relate(error, leftSpan, leftName);
        Relate(error, rightSpan, rightName);
        return Failed(syntax.extent);
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

/// Checks a call's arguments against @p function's parameters, into @p expr.
bool CheckArguments(Checker &checker, const Syntax::Expr &syntax, const FunctionSpec &function, Expr &expr)
{
    const Type numeric = NumericType(function, expr.operands);
    for (std::size_t i = 0; i < expr.operands.size(); ++i)
    {
        const bool takesNumber = function.parameters[i] == TypeNames::kNumeric;
        if (takesNumber && !IsNumeric(expr.operands[i].type))
        {
            Diagnostic &error = Fail(checker, expr.operands[i].span, std::format("{}() takes numbers", syntax.text),
                                     std::format("this is {}", TypeName(checker, expr.operands[i].type)));
            Relate(error, syntax.anchor, "in this call");
            return false;
        }
        const std::optional<Type> wanted = CallType(checker, function.parameters[i], numeric);
        const Reason because{.span = syntax.anchor,
                             .label = std::format("{}() takes {} here", syntax.text, function.parameters[i])};
        expr.operands[i] = Coerce(checker, std::move(expr.operands[i]), wanted.value_or(kError), because);
        if (IsError(expr.operands[i].type))
        {
            return false;
        }
    }
    expr.type = CallType(checker, function.result, numeric).value_or(kError);
    return true;
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
        const std::optional<std::string_view> closest = ClosestName(names, syntax.text);
        Fail(checker, syntax.anchor, std::format("unknown function \"{}\"", syntax.text),
             closest.has_value() ? std::format("did you mean {}()?", *closest) : std::string{"no such function"});
        return Failed(syntax.extent);
    }
    if (mode == ExprMode::Constant && index >= CoreFunctions().size())
    {
        Diagnostic &error = Fail(checker, syntax.anchor, std::format("a const can't call {}()", syntax.text),
                                 "only known while the game runs");
        error.help = "a const is worked out when the file cooks; make this a let, or call only abs, min, max and "
                     "clamp";
        return Failed(syntax.extent);
    }
    if (syntax.operands.size() != function->parameters.size())
    {
        Fail(checker, syntax.extent,
             std::format("{}() takes {} values, got {}", syntax.text, function->parameters.size(),
                         syntax.operands.size()),
             std::format("expected {} values", function->parameters.size()));
        return Failed(syntax.extent);
    }
    Expr expr;
    expr.span = syntax.extent;
    expr.kind = ExprKind::Call;
    expr.index = index;
    for (const Syntax::Expr &argument : syntax.operands)
    {
        expr.operands.push_back(CheckExpression(checker, argument, mode));
    }
    if (std::ranges::any_of(expr.operands, [](const Expr &operand) { return IsError(operand.type); }) ||
        !CheckArguments(checker, syntax, *function, expr))
    {
        return Failed(syntax.extent);
    }
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

Expr Coerce(Checker &checker, Expr value, Type wanted, const std::optional<Reason> &because)
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
            Diagnostic &error = Fail(checker, value.span, accepted.error(), std::format("not a {}", type.name));
            if (because.has_value())
            {
                Relate(error, because->span, because->label);
            }
            return Failed(value.span);
        }
        value.type = wanted;
        return value;
    }
    Diagnostic &error =
        Fail(checker, value.span, "mismatched types",
             std::format("expected {}, found {}", TypeName(checker, wanted), TypeName(checker, value.type)));
    if (because.has_value())
    {
        Relate(error, because->span, because->label);
    }
    return Failed(value.span);
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
