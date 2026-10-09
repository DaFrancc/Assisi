/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include "Checker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace Assisi::Sigil::Compile::Detail
{

namespace
{

using Result = std::expected<Constant, std::string>;

/// An int result, refused when it doesn't fit an int.
Result IntResult(int64_t value)
{
    if (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max())
    {
        return std::unexpected("the value is too large for an int");
    }
    return Constant{static_cast<int32_t>(value)};
}

Result IntArithmetic(Operator op, int64_t left, int64_t right)
{
    switch (op)
    {
    case Operator::Add:
        return IntResult(left + right);
    case Operator::Subtract:
        return IntResult(left - right);
    case Operator::Multiply:
        return IntResult(left * right);
    case Operator::Divide:
    case Operator::Remainder:
        if (right == 0)
        {
            return std::unexpected("this divides by zero");
        }
        return IntResult(op == Operator::Divide ? left / right : left % right);
    default:
        return std::unexpected("not an arithmetic operator");
    }
}

Result FloatArithmetic(Operator op, float left, float right)
{
    switch (op)
    {
    case Operator::Add:
        return Constant{left + right};
    case Operator::Subtract:
        return Constant{left - right};
    case Operator::Multiply:
        return Constant{left * right};
    case Operator::Divide:
        if (right == 0.f)
        {
            return std::unexpected("this divides by zero");
        }
        return Constant{left / right};
    default:
        return std::unexpected("not an arithmetic operator");
    }
}

template <typename T> bool Compare(Operator op, const T &left, const T &right)
{
    switch (op)
    {
    case Operator::Equal:
        return left == right;
    case Operator::NotEqual:
        return left != right;
    case Operator::Less:
        return left < right;
    case Operator::LessEqual:
        return left <= right;
    case Operator::Greater:
        return left > right;
    default:
        return left >= right;
    }
}

bool IsComparison(Operator op)
{
    return op == Operator::Equal || op == Operator::NotEqual || op == Operator::Less || op == Operator::LessEqual ||
           op == Operator::Greater || op == Operator::GreaterEqual;
}

Result Binary(Operator op, const Constant &left, const Constant &right)
{
    if (op == Operator::And || op == Operator::Or)
    {
        const bool a = std::get<bool>(left);
        const bool b = std::get<bool>(right);
        return Constant{op == Operator::And ? (a && b) : (a || b)};
    }
    if (std::holds_alternative<bool>(left))
    {
        return Constant{Compare(op, std::get<bool>(left), std::get<bool>(right))};
    }
    if (std::holds_alternative<float>(left))
    {
        if (IsComparison(op))
        {
            return Constant{Compare(op, std::get<float>(left), std::get<float>(right))};
        }
        return FloatArithmetic(op, std::get<float>(left), std::get<float>(right));
    }
    if (IsComparison(op))
    {
        return Constant{Compare(op, std::get<int32_t>(left), std::get<int32_t>(right))};
    }
    return IntArithmetic(op, std::get<int32_t>(left), std::get<int32_t>(right));
}

Result Unary(Operator op, const Constant &operand)
{
    if (op == Operator::Not)
    {
        return Constant{!std::get<bool>(operand)};
    }
    if (std::holds_alternative<float>(operand))
    {
        return Constant{-std::get<float>(operand)};
    }
    return IntResult(-static_cast<int64_t>(std::get<int32_t>(operand)));
}

/// abs, min, max or clamp, on values that are all ints or all floats.
template <typename T> Result Call(std::string_view name, std::span<const Constant> arguments)
{
    const T first = std::get<T>(arguments[0]);
    if (name == "abs")
    {
        if constexpr (std::is_same_v<T, int32_t>)
        {
            return IntResult(std::abs(static_cast<int64_t>(first)));
        }
        else
        {
            return Constant{std::abs(first)};
        }
    }
    const T second = std::get<T>(arguments[1]);
    if (name == "min")
    {
        return Constant{std::min(first, second)};
    }
    if (name == "max")
    {
        return Constant{std::max(first, second)};
    }
    const T third = std::get<T>(arguments[2]);
    if (second > third)
    {
        return std::unexpected("clamp's lowest value is above its highest");
    }
    return Constant{std::clamp(first, second, third)};
}

} // namespace

std::expected<Constant, std::string> Evaluate(const Expr &expr, std::span<const std::string> functions)
{
    std::vector<Constant> operands;
    for (const Expr &operand : expr.operands)
    {
        Result value = Evaluate(operand, functions);
        if (!value)
        {
            return value;
        }
        operands.push_back(std::move(*value));
    }
    switch (expr.kind)
    {
    case ExprKind::Literal:
        return expr.literal;
    case ExprKind::Widen:
        return Constant{static_cast<float>(std::get<int32_t>(operands[0]))};
    case ExprKind::Unary:
        return Unary(expr.op, operands[0]);
    case ExprKind::Binary:
        return Binary(expr.op, operands[0], operands[1]);
    case ExprKind::Call:
        if (std::holds_alternative<float>(operands[0]))
        {
            return Call<float>(functions[expr.index], operands);
        }
        return Call<int32_t>(functions[expr.index], operands);
    default:
        return std::unexpected("this isn't known until the game runs");
    }
}

} // namespace Assisi::Sigil::Compile::Detail
