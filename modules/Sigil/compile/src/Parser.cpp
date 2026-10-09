/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Sigil/Compile/Parser.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <utility>

namespace Assisi::Sigil::Compile
{

namespace Syntax
{

bool IsCoreWord(std::string_view word)
{
    return std::ranges::find(kCoreWords, word) != kCoreWords.end();
}

} // namespace Syntax

namespace
{

using Syntax::Named;

/// Binary operators by how tightly they bind, loosest first.
constexpr std::size_t kPrecedenceLevels = 6;
constexpr std::size_t kMostPerLevel = 4;
constexpr std::array<std::array<TokenKind, kMostPerLevel>, kPrecedenceLevels> kOperatorsByLevel{{
    {TokenKind::Or, TokenKind::End, TokenKind::End, TokenKind::End},
    {TokenKind::And, TokenKind::End, TokenKind::End, TokenKind::End},
    {TokenKind::Equal, TokenKind::NotEqual, TokenKind::End, TokenKind::End},
    {TokenKind::Less, TokenKind::LessEqual, TokenKind::Greater, TokenKind::GreaterEqual},
    {TokenKind::Plus, TokenKind::Minus, TokenKind::End, TokenKind::End},
    {TokenKind::Star, TokenKind::Slash, TokenKind::Percent, TokenKind::End},
}};

bool IsDeclarationWord(std::string_view word)
{
    return word == "import" || word == "enum" || word == "param" || word == "const" || word == "let";
}

/// Whether a missing @p kind is best pointed at just after what came before
/// it, the way `;` is missing from the end of a statement, rather than at
/// whatever came instead.
bool EndsSomething(TokenKind kind)
{
    return kind == TokenKind::Semicolon || kind == TokenKind::RightBrace || kind == TokenKind::RightParen;
}

Span SpanOf(const Token &token)
{
    return Span{.where = token.where, .length = token.length};
}

class Parser
{
  public:
    Parser(std::span<const Token> tokens, std::string_view file) : _tokens(tokens), _file(file) {}

    Syntax::File ParseFile()
    {
        Syntax::File file;
        if (!AtWord("use"))
        {
            Fail(SpanOf(Peek()), "missing \"use\" line", "expected \"use <vocabulary>;\" before this",
                 "every Sigil file starts by naming its vocabulary, like \"use animation;\"");
            return file;
        }
        if (!ParseHeader("use", file.use.vocabulary))
        {
            return file;
        }
        if (AtWord("sigiltype"))
        {
            Named kind;
            if (ParseHeader("sigiltype", kind))
            {
                file.sigilType = std::move(kind);
            }
        }
        ParseItems(file.root, 0, &file);
        return file;
    }

    [[nodiscard]] Diagnostics &Errors() { return _errors; }

  private:
    [[nodiscard]] const Token &Peek(std::size_t ahead = 0) const
    {
        return _tokens[std::min(_next + ahead, _tokens.size() - 1)];
    }

    const Token &Next()
    {
        const Token &token = Peek();
        _lastEnd = SourceLocation{.line = token.where.line, .column = token.where.column + token.length};
        if (_next + 1 < _tokens.size())
        {
            ++_next;
        }
        return token;
    }

    [[nodiscard]] bool At(TokenKind kind) const { return Peek().kind == kind; }
    [[nodiscard]] bool AtWord(std::string_view word) const { return At(TokenKind::Name) && Peek().text == word; }

    void Fail(Span span, std::string message, std::string label = {}, std::string help = {})
    {
        _errors.push_back(Diagnostic{.message = std::move(message),
                                     .label = std::move(label),
                                     .help = std::move(help),
                                     .file = std::string{_file},
                                     .where = span.where,
                                     .length = span.length});
    }

    /// What the next token is, for "found ..." in a message.
    [[nodiscard]] std::string Found() const
    {
        if (At(TokenKind::Name) || At(TokenKind::Int) || At(TokenKind::Float))
        {
            return std::format("\"{}\"", Peek().text);
        }
        return std::string{Describe(Peek().kind)};
    }

    /// From @p start to the end of the last token read, on @p start's line.
    [[nodiscard]] Span Since(SourceLocation start) const
    {
        const bool oneLine = _lastEnd.line == start.line && _lastEnd.column > start.column;
        return Span{.where = start, .length = oneLine ? _lastEnd.column - start.column : 1};
    }

    bool Expect(TokenKind kind, std::string_view after)
    {
        if (At(kind))
        {
            Next();
            return true;
        }
        const std::string message = std::format("expected {} {}, found {}", Describe(kind), after, Found());
        if (EndsSomething(kind) && _next > 0)
        {
            Fail(Span{.where = _lastEnd, .length = 1}, message, std::format("add {} here", Describe(kind)));
        }
        else
        {
            Fail(SpanOf(Peek()), message, std::format("expected {}", Describe(kind)));
        }
        return false;
    }

    std::optional<Named> ExpectName(std::string_view what)
    {
        if (!At(TokenKind::Name))
        {
            Fail(SpanOf(Peek()), std::format("expected {}, found {}", what, Found()), "expected a name");
            return std::nullopt;
        }
        const Token &token = Next();
        return Named{.name = token.text, .where = token.where};
    }

    /// After an error, skips to the end of the statement it was in: past the
    /// next `;`, or up to the `}` that closes the block it was in.
    void Recover()
    {
        uint32_t depth = 0;
        while (!At(TokenKind::End))
        {
            if (At(TokenKind::LeftBrace))
            {
                ++depth;
            }
            else if (At(TokenKind::RightBrace))
            {
                if (depth == 0)
                {
                    return;
                }
                --depth;
            }
            else if (At(TokenKind::Semicolon) && depth == 0)
            {
                Next();
                return;
            }
            Next();
        }
    }

    /// `word name;`, for `use` and `sigiltype`.
    bool ParseHeader(std::string_view word, Named &into)
    {
        Next();
        std::optional<Named> name = ExpectName(std::format("a name after \"{}\"", word));
        if (!name.has_value() || !Expect(TokenKind::Semicolon, std::format("after \"{} {}\"", word, name->name)))
        {
            Recover();
            return false;
        }
        into = std::move(*name);
        return true;
    }

    /// The statements of a block, up to its `}` or the end of the file. Only
    /// the file itself, @p file, takes declarations.
    void ParseItems(Syntax::Block &into, uint32_t depth, Syntax::File *file)
    {
        while (!At(TokenKind::End) && !(At(TokenKind::RightBrace) && file == nullptr))
        {
            if (!ParseItem(into, depth, file))
            {
                Recover();
                if (file != nullptr && At(TokenKind::RightBrace))
                {
                    Fail(SpanOf(Peek()), "unmatched '}'", "this closes no block");
                    Next();
                }
            }
        }
    }

    bool ParseItem(Syntax::Block &into, uint32_t depth, Syntax::File *file)
    {
        if (AtWord("use") || AtWord("sigiltype"))
        {
            Fail(SpanOf(Peek()), std::format("\"{}\" can only come at the start of the file", Peek().text),
                 "not allowed here");
            return false;
        }
        if (At(TokenKind::Name) && IsDeclarationWord(Peek().text))
        {
            if (file == nullptr)
            {
                Fail(SpanOf(Peek()), std::format("\"{}\" declarations go at the top level of the file", Peek().text),
                     "inside a block", "move it above the blocks");
                return false;
            }
            return ParseDeclaration(*file);
        }
        if (IsTransitionStart())
        {
            Syntax::Transition transition;
            if (!ParseTransition(transition))
            {
                return false;
            }
            into.transitions.push_back(std::move(transition));
            return true;
        }
        if (At(TokenKind::Name) && Peek(1).kind == TokenKind::Name && Peek(2).kind == TokenKind::LeftBrace)
        {
            return ParseBlock(into, depth);
        }
        if (At(TokenKind::Name))
        {
            Syntax::Clause clause;
            if (!ParseClause(clause))
            {
                return false;
            }
            into.clauses.push_back(std::move(clause));
            return true;
        }
        Fail(SpanOf(Peek()), std::format("expected a block, a clause or a transition, found {}", Found()),
             "unexpected here");
        return false;
    }

    /// `name (+|- name)* ->` ahead, with `any` counting as a name.
    [[nodiscard]] bool IsTransitionStart() const
    {
        if (!At(TokenKind::Name))
        {
            return false;
        }
        std::size_t ahead = 1;
        while ((Peek(ahead).kind == TokenKind::Plus || Peek(ahead).kind == TokenKind::Minus) &&
               Peek(ahead + 1).kind == TokenKind::Name)
        {
            ahead += 2;
        }
        return Peek(ahead).kind == TokenKind::Arrow;
    }

    bool ParseBlock(Syntax::Block &into, uint32_t depth)
    {
        Syntax::Block block;
        const Token &kind = Next();
        block.kind = Named{.name = kind.text, .where = kind.where};
        const Token &name = Next();
        block.name = Named{.name = name.text, .where = name.where};
        Next();
        if (depth + 1 > kMaxBlockDepth)
        {
            Fail(block.kind.Extent(), std::format("blocks nest more than {} deep here", kMaxBlockDepth),
                 "too deep");
            return false;
        }
        ParseItems(block, depth + 1, nullptr);
        if (!Expect(TokenKind::RightBrace, std::format("to close {} \"{}\"", block.kind.name, block.name.name)))
        {
            return false;
        }
        into.blocks.push_back(std::move(block));
        if (At(TokenKind::Semicolon))
        {
            Fail(SpanOf(Peek()), "unexpected ';' after a block", "remove this ';'",
                 "a block ends at its '}', with no ';' after it");
            Next();
        }
        return true;
    }

    bool ParseClause(Syntax::Clause &clause)
    {
        const Token &word = Next();
        clause.word = Named{.name = word.text, .where = word.where};
        if (!At(TokenKind::Semicolon))
        {
            do
            {
                std::optional<Syntax::Expr> argument = ParseExpression(0);
                if (!argument.has_value())
                {
                    return false;
                }
                clause.arguments.push_back(std::move(*argument));
            } while (At(TokenKind::Comma) && (Next(), true));
        }
        return Expect(TokenKind::Semicolon, std::format("at the end of the \"{}\" clause", clause.word.name));
    }

    bool ParseTransition(Syntax::Transition &transition)
    {
        const Token &first = Next();
        transition.first = Named{.name = first.text, .where = first.where};
        transition.fromAny = first.text == "any";
        while (At(TokenKind::Plus) || At(TokenKind::Minus))
        {
            const bool remove = Next().kind == TokenKind::Minus;
            const Token &state = Next();
            transition.steps.push_back(
                Syntax::SourceStep{.state = Named{.name = state.text, .where = state.where}, .remove = remove});
        }
        Next();
        std::optional<Named> target = ExpectName("the state this transition goes to");
        if (!target.has_value())
        {
            return false;
        }
        transition.target = std::move(*target);
        if (!AtWord("when"))
        {
            Fail(SpanOf(Peek()), std::format("expected \"when\" after \"-> {}\", found {}", transition.target.name, Found()),
                 "expected \"when\"",
                 std::format("every transition needs a condition, like \"-> {} when speed > 1.0;\"",
                             transition.target.name));
            return false;
        }
        Next();
        std::optional<Syntax::Expr> condition = ParseExpression(0);
        if (!condition.has_value())
        {
            return false;
        }
        transition.condition = std::move(*condition);
        if (At(TokenKind::LeftBrace))
        {
            Next();
            while (At(TokenKind::Name))
            {
                Syntax::Clause clause;
                if (!ParseClause(clause))
                {
                    return false;
                }
                transition.clauses.push_back(std::move(clause));
            }
            if (!Expect(TokenKind::RightBrace, "to close the transition's clauses"))
            {
                return false;
            }
        }
        return Expect(TokenKind::Semicolon, "at the end of the transition");
    }

    bool ParseDeclaration(Syntax::File &file)
    {
        const std::string word = Peek().text;
        if (word == "import")
        {
            return ParseImport(file);
        }
        if (word == "enum")
        {
            return ParseEnum(file);
        }
        if (word == "param")
        {
            return ParseParam(file);
        }
        return ParseValue(file, word == "const");
    }

    bool ParseImport(Syntax::File &file)
    {
        const SourceLocation where = Next().where;
        if (!At(TokenKind::String))
        {
            Fail(SpanOf(Peek()), std::format("expected the path of a file after \"import\", found {}", Found()),
                 "expected a path in quotes");
            return false;
        }
        const Token &path = Next();
        Syntax::Import statement{.path = path.text, .where = where, .pathSpan = SpanOf(path)};
        if (!Expect(TokenKind::Semicolon, "after the import"))
        {
            return false;
        }
        file.declarations.emplace_back(std::move(statement));
        return true;
    }

    bool ParseEnum(Syntax::File &file)
    {
        Next();
        std::optional<Named> name = ExpectName("the enum's name");
        if (!name.has_value() || !Expect(TokenKind::LeftBrace, std::format("after \"enum {}\"", name->name)))
        {
            return false;
        }
        Syntax::EnumDecl declaration{.name = std::move(*name), .values = {}};
        while (!At(TokenKind::RightBrace))
        {
            std::optional<Named> value = ExpectName(std::format("a value of {}", declaration.name.name));
            if (!value.has_value())
            {
                return false;
            }
            declaration.values.push_back(std::move(*value));
            if (!At(TokenKind::Comma))
            {
                break;
            }
            Next();
        }
        if (!Expect(TokenKind::RightBrace, std::format("to close enum {}", declaration.name.name)))
        {
            return false;
        }
        file.declarations.emplace_back(std::move(declaration));
        if (At(TokenKind::Semicolon))
        {
            Fail(SpanOf(Peek()), "unexpected ';' after an enum", "remove this ';'",
                 "an enum ends at its '}', with no ';' after it");
            Next();
        }
        return true;
    }

    bool ParseParam(Syntax::File &file)
    {
        Next();
        std::optional<Named> name = ExpectName("the param's name");
        if (!name.has_value() || !Expect(TokenKind::Colon, std::format("and a type after \"param {}\"", name->name)))
        {
            return false;
        }
        std::optional<Named> type = ExpectName(std::format("the type of param {}", name->name));
        if (!type.has_value() || !Expect(TokenKind::Semicolon, "at the end of the param"))
        {
            return false;
        }
        file.declarations.emplace_back(Syntax::ParamDecl{.name = std::move(*name), .type = std::move(*type)});
        return true;
    }

    bool ParseValue(Syntax::File &file, bool isConst)
    {
        const std::string_view word = isConst ? "const" : "let";
        Next();
        std::optional<Named> name = ExpectName(std::format("the {}'s name", word));
        if (!name.has_value())
        {
            return false;
        }
        Syntax::ValueDecl declaration{.name = std::move(*name), .type = std::nullopt, .value = {}, .isConst = isConst};
        if (At(TokenKind::Colon))
        {
            Next();
            declaration.type = ExpectName(std::format("the type of {}", declaration.name.name));
            if (!declaration.type.has_value())
            {
                return false;
            }
        }
        if (!Expect(TokenKind::Assign, std::format("and a value after \"{} {}\"", word, declaration.name.name)))
        {
            return false;
        }
        std::optional<Syntax::Expr> value = ParseExpression(0);
        if (!value.has_value() || !Expect(TokenKind::Semicolon, std::format("at the end of the {}", word)))
        {
            return false;
        }
        declaration.value = std::move(*value);
        file.declarations.emplace_back(std::move(declaration));
        return true;
    }

    [[nodiscard]] bool AtOperatorOf(std::size_t level) const
    {
        return std::ranges::find(kOperatorsByLevel[level], Peek().kind) != kOperatorsByLevel[level].end() &&
               !At(TokenKind::End);
    }

    std::optional<Syntax::Expr> ParseExpression(uint32_t depth) { return ParseBinary(0, depth); }

    /// An operator node over @p operands, the operator being @p op.
    Syntax::Expr Operation(const Token &op, Syntax::ExprKind kind, SourceLocation start)
    {
        Syntax::Expr expr{.operands = {},
                          .text = {},
                          .member = {},
                          .extent = Since(start),
                          .anchor = SpanOf(op),
                          .kind = kind,
                          .op = op.kind};
        return expr;
    }

    std::optional<Syntax::Expr> ParseBinary(std::size_t level, uint32_t depth)
    {
        if (level == kPrecedenceLevels)
        {
            return ParseUnary(depth);
        }
        std::optional<Syntax::Expr> left = ParseBinary(level + 1, depth);
        while (left.has_value() && AtOperatorOf(level))
        {
            const Token &op = Next();
            std::optional<Syntax::Expr> right = ParseBinary(level + 1, depth);
            if (!right.has_value())
            {
                return std::nullopt;
            }
            Syntax::Expr binary = Operation(op, Syntax::ExprKind::Binary, left->extent.where);
            binary.operands.push_back(std::move(*left));
            binary.operands.push_back(std::move(*right));
            left = std::move(binary);
        }
        return left;
    }

    std::optional<Syntax::Expr> ParseUnary(uint32_t depth)
    {
        if (depth > kMaxExpressionDepth)
        {
            Fail(SpanOf(Peek()), std::format("this expression nests more than {} deep", kMaxExpressionDepth),
                 "too deep");
            return std::nullopt;
        }
        if (!At(TokenKind::Not) && !At(TokenKind::Minus))
        {
            return ParsePrimary(depth);
        }
        const Token &op = Next();
        std::optional<Syntax::Expr> operand = ParseUnary(depth + 1);
        if (!operand.has_value())
        {
            return std::nullopt;
        }
        Syntax::Expr unary = Operation(op, Syntax::ExprKind::Unary, op.where);
        unary.operands.push_back(std::move(*operand));
        return unary;
    }

    std::optional<Syntax::Expr> ParsePrimary(uint32_t depth)
    {
        const Token &token = Peek();
        Syntax::Expr expr{.operands = {},
                          .text = token.text,
                          .member = {},
                          .extent = SpanOf(token),
                          .anchor = SpanOf(token),
                          .kind = Syntax::ExprKind::Int,
                          .op = TokenKind::End};
        switch (token.kind)
        {
        case TokenKind::Int:
            Next();
            return expr;
        case TokenKind::Float:
            Next();
            expr.kind = Syntax::ExprKind::Float;
            return expr;
        case TokenKind::String:
            Next();
            expr.kind = Syntax::ExprKind::String;
            return expr;
        case TokenKind::LeftParen:
        {
            Next();
            std::optional<Syntax::Expr> inner = ParseBinary(0, depth + 1);
            if (!inner.has_value() || !Expect(TokenKind::RightParen, "to close the '('"))
            {
                return std::nullopt;
            }
            return inner;
        }
        case TokenKind::Name:
            return ParseNamed(std::move(expr), depth);
        default:
            Fail(SpanOf(token), std::format("expected a value, found {}", Found()), "expected a value");
            return std::nullopt;
        }
    }

    /// A name, `true` or `false`, `Enum.value`, or `function(arguments)`.
    std::optional<Syntax::Expr> ParseNamed(Syntax::Expr expr, uint32_t depth)
    {
        if (expr.text == "true" || expr.text == "false")
        {
            Next();
            expr.kind = Syntax::ExprKind::Bool;
            return expr;
        }
        if (Syntax::IsCoreWord(expr.text))
        {
            Fail(expr.anchor, std::format("expected a value, found \"{}\"", expr.text), "a reserved word");
            return std::nullopt;
        }
        Next();
        expr.kind = Syntax::ExprKind::Name;
        if (At(TokenKind::Dot))
        {
            Next();
            std::optional<Named> member = ExpectName(std::format("a value of {} after the '.'", expr.text));
            if (!member.has_value())
            {
                return std::nullopt;
            }
            expr.kind = Syntax::ExprKind::Member;
            expr.member = std::move(member->name);
            expr.extent = Since(expr.extent.where);
            expr.anchor = expr.extent;
            return expr;
        }
        if (!At(TokenKind::LeftParen))
        {
            return expr;
        }
        Next();
        expr.kind = Syntax::ExprKind::Call;
        if (!At(TokenKind::RightParen))
        {
            do
            {
                std::optional<Syntax::Expr> argument = ParseBinary(0, depth + 1);
                if (!argument.has_value())
                {
                    return std::nullopt;
                }
                expr.operands.push_back(std::move(*argument));
            } while (At(TokenKind::Comma) && (Next(), true));
        }
        if (!Expect(TokenKind::RightParen, std::format("to close the call to {}", expr.text)))
        {
            return std::nullopt;
        }
        expr.extent = Since(expr.extent.where);
        return expr;
    }

    std::span<const Token> _tokens;
    std::string_view _file;
    Diagnostics _errors;
    std::size_t _next = 0;
    /// Just after the last token read: where a missing `;` belongs.
    SourceLocation _lastEnd;
};

} // namespace

std::expected<Syntax::File, Diagnostics> Parse(std::span<const Token> tokens, std::string_view file)
{
    Parser parser{tokens, file};
    Syntax::File parsed = parser.ParseFile();
    if (!parser.Errors().empty())
    {
        return std::unexpected(std::move(parser.Errors()));
    }
    return parsed;
}

} // namespace Assisi::Sigil::Compile
