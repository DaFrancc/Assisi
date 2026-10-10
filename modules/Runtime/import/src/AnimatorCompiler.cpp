/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Runtime/Import/AnimatorCompiler.hpp>

#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Geometry/AnimationClip.hpp>
#include <Assisi/Geometry/BlendSpace.hpp>
#include <Assisi/Geometry/MeshImporter.hpp>
#include <Assisi/Sigil/Compile/Compile.hpp>
#include <Assisi/Sigil/Compile/Lower.hpp>
#include <Assisi/Sigil/Compile/Suggest.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Assisi::Runtime::Import
{

namespace
{

using namespace Sigil::Compile;

constexpr std::string_view kVocabularyName = "animation";
constexpr std::string_view kLayerKind = "layer";
constexpr std::string_view kStateKind = "state";
constexpr std::string_view kProgressFunction = "progress";

/// How a clause is written: its word and the type of each value.
ClauseSpec SpecOf(AnimationClause clause)
{
    const ArgumentSpec number{.type = std::string{TypeNames::kFloat}, .kind = ArgumentKind::Value};
    const ArgumentSpec state{.type = {}, .kind = ArgumentKind::State};
    const std::vector<std::string> layer{std::string{kLayerKind}};
    const std::vector<std::string> inState{std::string{kStateKind}};
    switch (clause)
    {
    case AnimationClause::Skeleton:
        return ClauseSpec{.word = "skeleton", .arguments = {{"model"}}, .blocks = {}, .onTransition = false,
                          .fileLevel = true, .cardinality = Cardinality::ExactlyOnce};
    case AnimationClause::Mask:
        return ClauseSpec{.word = "mask", .arguments = {{"joint"}}, .blocks = layer};
    case AnimationClause::Exclude:
        return ClauseSpec{.word = "exclude", .arguments = {{"joint"}}, .blocks = layer, .onTransition = false,
                          .fileLevel = false, .cardinality = Cardinality::Any};
    case AnimationClause::Additive:
        return ClauseSpec{.word = "additive", .arguments = {}, .blocks = layer};
    case AnimationClause::Weight:
        return ClauseSpec{.word = "weight", .arguments = {number}, .blocks = layer};
    case AnimationClause::Fade:
        return ClauseSpec{.word = "fade", .arguments = {number}, .blocks = layer, .onTransition = true};
    case AnimationClause::Play:
        return ClauseSpec{.word = "play", .arguments = {{"clip"}}, .blocks = inState};
    case AnimationClause::Pose:
        return ClauseSpec{.word = "pose", .arguments = {{"clip"}}, .blocks = inState};
    case AnimationClause::X:
        return ClauseSpec{.word = "x", .arguments = {number}, .blocks = inState};
    case AnimationClause::Y:
        return ClauseSpec{.word = "y", .arguments = {number}, .blocks = inState};
    case AnimationClause::Rate:
        return ClauseSpec{.word = "rate", .arguments = {number}, .blocks = inState};
    case AnimationClause::Then:
        return ClauseSpec{.word = "then", .arguments = {state}, .blocks = inState};
    case AnimationClause::Interrupt:
        return ClauseSpec{.word = "interrupt", .arguments = {}, .blocks = {}, .onTransition = true};
    case AnimationClause::Count_:
        break;
    }
    return {};
}

std::expected<void, std::string> NotEmpty(std::string_view value, std::string_view what)
{
    if (value.empty())
    {
        return std::unexpected(std::format("an empty name can't be a {}", what));
    }
    return {};
}

/// A type and the check its strings pass: @p assets' when there is one.
ValueType TypeOf(AnimationType type, const AnimationAssets *assets)
{
    switch (type)
    {
    case AnimationType::Clip:
        return ValueType{.name = "clip", .validate = [assets](std::string_view value)
                         { return assets != nullptr ? assets->CheckClip(value) : NotEmpty(value, "clip"); }};
    case AnimationType::Joint:
        return ValueType{.name = "joint", .validate = [assets](std::string_view value)
                         { return assets != nullptr ? assets->CheckJoint(value) : NotEmpty(value, "joint"); }};
    case AnimationType::Model:
        return ValueType{.name = "model", .validate = [assets](std::string_view value)
                         { return assets != nullptr ? assets->CheckModel(value) : NotEmpty(value, "model"); }};
    case AnimationType::Count_:
        break;
    }
    return {};
}

/// Names checked against the asset tree the cook reads: clips, the model the
/// file names as its skeleton, and that model's joints.
class TreeAssets final : public AnimationAssets
{
  public:
    TreeAssets(const Core::AssetCookContext &context, std::optional<std::string> skeleton)
        : _skeleton(std::move(skeleton).value_or(std::string{})), _context(&context)
    {
        if (!_skeleton.empty() && IsModel(_skeleton))
        {
            ReadJoints();
        }
    }

    [[nodiscard]] std::expected<void, std::string> CheckClip(std::string_view vpath) const override
    {
        const std::optional<std::string> kind = KindOf(vpath);
        if (!kind.has_value())
        {
            return std::unexpected(std::format("there is no \"{}\" in the asset tree", vpath));
        }
        if (*kind != Geometry::kAnimationKindName && *kind != Geometry::kBlendSpaceKindName)
        {
            return std::unexpected(std::format("\"{}\" is {}, not a clip or blend space", vpath, Article(*kind)));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> CheckModel(std::string_view vpath) const override
    {
        const std::optional<std::string> kind = KindOf(vpath);
        if (!kind.has_value())
        {
            return std::unexpected(std::format("there is no \"{}\" in the asset tree", vpath));
        }
        if (!IsModel(vpath))
        {
            return std::unexpected(std::format("\"{}\" is {}, not a model", vpath, Article(*kind)));
        }
        if (vpath == _skeleton && !_modelError.empty())
        {
            return std::unexpected(_modelError);
        }
        if (vpath == _skeleton && _joints.empty())
        {
            return std::unexpected(std::format("\"{}\" has no skeleton", vpath));
        }
        return {};
    }

    [[nodiscard]] std::expected<void, std::string> CheckJoint(std::string_view name) const override
    {
        // With no skeleton read, the skeleton clause already says why.
        if (_joints.empty() || std::ranges::find(_joints, name) != _joints.end())
        {
            return {};
        }
        std::vector<std::string_view> joints{_joints.begin(), _joints.end()};
        return std::unexpected(
            std::format("\"{}\" has no joint \"{}\"{}", _skeleton, name, DidYouMean(joints, name)));
    }

  private:
    [[nodiscard]] std::optional<std::string> KindOf(std::string_view vpath) const
    {
        const std::optional<Core::AssetId> id = _context->IdFor(vpath);
        return id.has_value() ? _context->KindNameOf(*id) : std::nullopt;
    }

    [[nodiscard]] bool IsModel(std::string_view vpath) const
    {
        return KindOf(vpath) == std::optional<std::string>{Core::BuiltInKindName(Core::kMeshKind)};
    }

    [[nodiscard]] static std::string Article(std::string_view kind)
    {
        const bool vowel = !kind.empty() && std::string_view{"aeiou"}.find(kind.front()) != std::string_view::npos;
        return std::format("{} {}", vowel ? "an" : "a", kind);
    }

    void ReadJoints()
    {
        std::expected<Geometry::MeshData, Geometry::MeshImportError> mesh = Geometry::ImportMesh(_skeleton);
        if (!mesh)
        {
            _modelError = std::format("\"{}\" can't be read as a model: {}", _skeleton,
                                      Geometry::ToString(mesh.error()));
            return;
        }
        _joints = std::move(mesh->Skeleton.Names);
    }

    std::vector<std::string> _joints;
    std::string _skeleton;
    std::string _modelError;
    const Core::AssetCookContext *_context;
};

// ── Lowering a checked file ──────────────────────────────────────────────────

/// What lowering one file shares: the checked file, where it came from, and
/// the graph and errors so far.
struct Lowering
{
    AnimatorGraph graph;
    Diagnostics errors;
    const Program &program;
    const Vocabulary &vocabulary;
    const Core::AssetCookContext &context;
};

AnimationClause ClauseOf(const Clause &clause)
{
    return static_cast<AnimationClause>(clause.spec);
}

uint32_t WordLength(const Lowering &lowering, const Clause &clause)
{
    return static_cast<uint32_t>(lowering.vocabulary.clauses[clause.spec].word.size());
}

Diagnostic &Fail(Lowering &lowering, SourceLocation where, uint32_t length, std::string message, std::string label)
{
    lowering.errors.push_back(Diagnostic{.message = std::move(message),
                                         .label = std::move(label),
                                         .file = std::string{lowering.context.Path()},
                                         .where = where,
                                         .length = length});
    return lowering.errors.back();
}

Diagnostic &FailClause(Lowering &lowering, const Clause &clause, std::string message, std::string label)
{
    return Fail(lowering, clause.where, WordLength(lowering, clause), std::move(message), std::move(label));
}

/// The code for @p expr, appended to the graph's.
uint32_t LowerValue(Lowering &lowering, const Clause &clause, const Expr &expr)
{
    std::expected<uint32_t, std::string> entry = LowerExpression(lowering.graph.layout, expr, lowering.graph.code);
    if (!entry)
    {
        FailClause(lowering, clause, std::format("this value can't be worked out: {}", entry.error()),
                   "in this clause");
        return kNotWritten;
    }
    return *entry;
}

/// The string a clause's value was written as, which the checker has already
/// accepted for its type. Empty when a param stands in for it.
std::string LiteralOf(const Clause &clause)
{
    const Expr &value = clause.arguments.front().value;
    const std::string *text = std::get_if<std::string>(&value.literal);
    return value.kind == ExprKind::Literal && text != nullptr ? *text : std::string{};
}

AnimatorClip LowerClip(Lowering &lowering, const Clause &clause)
{
    const Expr &value = clause.arguments.front().value;
    if (value.kind == ExprKind::Param)
    {
        const std::string &name = lowering.program.params[value.index].name;
        const std::vector<std::string>::iterator found = std::ranges::find(lowering.graph.clipParams, name);
        return AnimatorClip{.asset = {},
                            .param = static_cast<uint32_t>(found - lowering.graph.clipParams.begin())};
    }
    const std::string path = LiteralOf(clause);
    return AnimatorClip{.asset = lowering.context.IdFor(path).value_or(Core::AssetId{}), .param = kNotWritten};
}

/// Whether @p clip names a blend space written in the file.
bool IsBlendSpace(const Lowering &lowering, const AnimatorClip &clip)
{
    if (clip.param != kNotWritten)
    {
        return false;
    }
    return lowering.context.KindNameOf(clip.asset) == std::optional<std::string>{Geometry::kBlendSpaceKindName};
}

void LowerPlayed(Lowering &lowering, const Block &block, const Clause &clause, AnimatorState &state)
{
    const bool pose = ClauseOf(clause) == AnimationClause::Pose;
    if (state.clip.asset != Core::AssetId{} || state.clip.param != kNotWritten)
    {
        Diagnostic &error = FailClause(lowering, clause, std::format("state \"{}\" plays two things", block.name),
                                       "a second clip here");
        error.help = "a state plays one clip, blend space or pose; give the other one a state of its own";
        return;
    }
    state.clip = LowerClip(lowering, clause);
    state.pose = pose;
    if (pose && IsBlendSpace(lowering, state.clip))
    {
        Diagnostic &error = FailClause(lowering, clause, "a blend space can't be held as a pose", "a blend space");
        error.help = "pose takes one clip; play the blend space instead";
    }
}

/// A state holding states plays nothing itself; one holding none plays
/// exactly one clip, blend space or pose.
void LowerState(Lowering &lowering, const Block &block, AnimatorState &state)
{
    const Clause *then = nullptr;
    for (const Clause &clause : block.clauses)
    {
        if (!block.children.empty())
        {
            Diagnostic &error =
                FailClause(lowering, clause, std::format("state \"{}\" holds states, so it plays nothing", block.name),
                           "not allowed here");
            error.help = "write it in the states inside it";
            continue;
        }
        switch (ClauseOf(clause))
        {
        case AnimationClause::Play:
        case AnimationClause::Pose:
            LowerPlayed(lowering, block, clause, state);
            break;
        case AnimationClause::X:
            state.x = LowerValue(lowering, clause, clause.arguments.front().value);
            break;
        case AnimationClause::Y:
            state.y = LowerValue(lowering, clause, clause.arguments.front().value);
            break;
        case AnimationClause::Rate:
            state.rate = LowerValue(lowering, clause, clause.arguments.front().value);
            break;
        case AnimationClause::Then:
            state.then = static_cast<uint32_t>(clause.arguments.front().state);
            then = &clause;
            break;
        default:
            break;
        }
    }
    const bool plays = state.clip.asset != Core::AssetId{} || state.clip.param != kNotWritten;
    if (block.children.empty() && !plays)
    {
        Diagnostic &error =
            Fail(lowering, block.where, static_cast<uint32_t>(std::max<std::size_t>(block.name.size(), 1)),
                 std::format("state \"{}\" plays nothing", block.name), "no play or pose");
        error.help = "give it \"play\" with a clip or blend space, or \"pose\" with a clip";
    }
    if (then != nullptr && state.pose)
    {
        Diagnostic &error = FailClause(lowering, *then, "a pose holds still, so it never finishes", "never reached");
        error.help = "\"then\" follows a clip played once; use \"play\" instead of \"pose\"";
    }
}

/// The first layer is the whole body under every other, so it takes nothing
/// that limits or weighs it.
void RefuseOnBase(Lowering &lowering, const Clause &clause)
{
    const std::string &word = lowering.vocabulary.clauses[clause.spec].word;
    Diagnostic &error = FailClause(lowering, clause, std::format("the first layer can't have \"{}\"", word),
                                   "on the first layer");
    error.help = "the first layer plays on the whole body under every other; write a layer after it for this";
}

void LowerLayerClause(Lowering &lowering, const Clause &clause, bool base, AnimatorLayer &layer)
{
    const AnimationClause which = ClauseOf(clause);
    if (base && which != AnimationClause::Fade)
    {
        RefuseOnBase(lowering, clause);
        return;
    }
    switch (which)
    {
    case AnimationClause::Mask:
        layer.maskRoot = Core::InternedString{LiteralOf(clause)};
        break;
    case AnimationClause::Exclude:
        layer.exclusions.emplace_back(LiteralOf(clause));
        break;
    case AnimationClause::Additive:
        layer.mode = LayerMode::Additive;
        break;
    case AnimationClause::Weight:
        layer.weight = LowerValue(lowering, clause, clause.arguments.front().value);
        break;
    case AnimationClause::Fade:
        layer.fade = LowerValue(lowering, clause, clause.arguments.front().value);
        break;
    default:
        break;
    }
}

void LowerTransition(Lowering &lowering, const Transition &transition, AnimatorTransition &lowered)
{
    for (const Clause &clause : transition.clauses)
    {
        if (ClauseOf(clause) == AnimationClause::Fade)
        {
            lowered.fade = LowerValue(lowering, clause, clause.arguments.front().value);
        }
        else if (ClauseOf(clause) == AnimationClause::Interrupt)
        {
            lowered.interrupt = true;
        }
    }
}

void LowerLayer(Lowering &lowering, const Block &block, bool base)
{
    std::expected<LoweredGraph, std::string> graph = LowerGraph(lowering.graph.layout, block, lowering.graph.code);
    if (!graph)
    {
        Fail(lowering, block.where, static_cast<uint32_t>(block.name.size()),
             std::format("layer \"{}\" can't be worked out: {}", block.name, graph.error()), "in this layer");
        return;
    }
    AnimatorLayer layer;
    layer.graph = std::move(graph->graph);
    layer.name = block.name;
    for (const Clause &clause : block.clauses)
    {
        LowerLayerClause(lowering, clause, base, layer);
    }
    layer.states.resize(layer.graph.nodes.size());
    for (std::size_t node = 1; node < graph->blocks.size(); ++node)
    {
        LowerState(lowering, *graph->blocks[node], layer.states[node]);
    }
    layer.transitions.resize(layer.graph.transitions.size());
    for (std::size_t t = 0; t < graph->transitions.size(); ++t)
    {
        LowerTransition(lowering, *graph->transitions[t], layer.transitions[t]);
    }
    lowering.graph.layers.push_back(std::move(layer));
}

/// The params of the vocabulary's clip type, which an Animator binds, and the
/// triggers, which it clears each frame.
void LowerParams(Lowering &lowering)
{
    for (std::size_t param = 0; param < lowering.program.params.size(); ++param)
    {
        const Param &declared = lowering.program.params[param];
        const bool clip = declared.type.kind == TypeKind::Vocabulary &&
                          declared.type.index == static_cast<uint32_t>(AnimationType::Clip);
        if (clip)
        {
            lowering.graph.clipParams.push_back(declared.name);
        }
        if (declared.type.kind == TypeKind::Trigger)
        {
            lowering.graph.triggerSlots.push_back(lowering.graph.layout.ParamSlot(static_cast<uint32_t>(param)));
        }
    }
    const std::optional<uint32_t> progress = Sigil::FindSlot(lowering.graph.layout, kProgressFunction);
    lowering.graph.progressSlot = progress.value_or(kNotWritten);
}

void LowerFile(Lowering &lowering)
{
    lowering.graph.layout = MakeLayout(lowering.program, lowering.vocabulary);
    std::expected<std::vector<uint32_t>, std::string> lets =
        LowerLets(lowering.program, lowering.graph.layout, lowering.graph.code);
    if (!lets)
    {
        Fail(lowering, SourceLocation{}, 1, std::format("a let can't be worked out: {}", lets.error()), "");
        return;
    }
    lowering.graph.letEntries = std::move(*lets);
    LowerParams(lowering);
    for (const Clause &clause : lowering.program.root.clauses)
    {
        if (ClauseOf(clause) == AnimationClause::Skeleton)
        {
            lowering.graph.skeleton = lowering.context.IdFor(LiteralOf(clause)).value_or(Core::AssetId{});
        }
    }
    for (std::size_t layer = 0; layer < lowering.program.root.children.size(); ++layer)
    {
        LowerLayer(lowering, lowering.program.root.children[layer], layer == 0);
    }
}

// ── Reporting ────────────────────────────────────────────────────────────────

std::vector<std::string_view> SplitLines(std::string_view source)
{
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= source.size())
    {
        const std::size_t end = std::min(source.find('\n', start), source.size());
        std::string_view line = source.substr(start, end - start);
        if (line.ends_with('\r'))
        {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        start = end + 1;
    }
    return lines;
}

/// Every diagnostic in @p diagnostics, formatted; those lowering found get the
/// line they point at from @p source, as the compile gives its own.
std::string Describe(Diagnostics diagnostics, std::string_view source)
{
    const std::vector<std::string_view> lines = SplitLines(source);
    std::string text;
    for (Diagnostic &diagnostic : diagnostics)
    {
        if (diagnostic.excerpt.empty() && diagnostic.where.line >= 1 && diagnostic.where.line <= lines.size())
        {
            diagnostic.excerpt = std::string{lines[diagnostic.where.line - 1]};
        }
        text += Format(diagnostic);
    }
    return text;
}

std::expected<std::string, std::string> ReadText(const Core::AssetCookContext &context, std::string_view vpath)
{
    std::expected<std::vector<std::byte>, std::string> bytes = context.Read(vpath);
    if (!bytes)
    {
        return std::unexpected(bytes.error());
    }
    return std::string{reinterpret_cast<const char *>(bytes->data()), bytes->size()};
}

} // namespace

Vocabulary AnimationVocabulary(const AnimationAssets *assets)
{
    Vocabulary animation;
    animation.name = std::string{kVocabularyName};
    for (uint8_t type = 0; type < static_cast<uint8_t>(AnimationType::Count_); ++type)
    {
        animation.types.push_back(TypeOf(static_cast<AnimationType>(type), assets));
    }
    animation.blocks = {
        BlockKind{.name = std::string{kLayerKind}, .parents = {}, .topLevel = true, .holdsStates = true},
        BlockKind{.name = std::string{kStateKind},
                  .parents = {std::string{kLayerKind}, std::string{kStateKind}},
                  .topLevel = false,
                  .holdsStates = true},
    };
    for (uint8_t clause = 0; clause < static_cast<uint8_t>(AnimationClause::Count_); ++clause)
    {
        animation.clauses.push_back(SpecOf(static_cast<AnimationClause>(clause)));
    }
    animation.functions = {FunctionSpec{.name = std::string{kProgressFunction},
                                        .parameters = {},
                                        .result = std::string{TypeNames::kFloat},
                                        .use = FunctionUse::WhenOnly}};
    return animation;
}

std::expected<AnimatorGraph, std::string> CompileAnimator(std::string_view source,
                                                          const Core::AssetCookContext &context)
{
    const TreeAssets assets{context, FileClauseString(source, "skeleton")};
    const std::array<Vocabulary, 1> vocabularies{AnimationVocabulary(&assets)};
    const SourceReader read = [&context](std::string_view vpath) { return ReadText(context, vpath); };
    std::expected<Program, Diagnostics> program = CompileSource(source, context.Path(), vocabularies, read);
    if (!program)
    {
        return std::unexpected(Describe(std::move(program.error()), source));
    }
    if (program->library)
    {
        return AnimatorGraph{};
    }
    Lowering lowering{
        .graph = {}, .errors = {}, .program = *program, .vocabulary = vocabularies[0], .context = context};
    LowerFile(lowering);
    if (!lowering.errors.empty())
    {
        return std::unexpected(Describe(std::move(lowering.errors), source));
    }
    return std::move(lowering.graph);
}

} // namespace Assisi::Runtime::Import
