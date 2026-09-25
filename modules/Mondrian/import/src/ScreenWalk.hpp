/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file ScreenWalk.hpp
/// @brief The state a screen compile carries through the file, and the steps
/// that read one element, one instance and one template at a time.
///
/// Internal to the import library.

#include <Assisi/Mondrian/Import/Markup.hpp>

#include <Assisi/Mondrian/ScreenDocument.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Assisi::Core
{
class EventCatalog;
}

namespace Assisi::Mondrian::Import
{

/// The root of a screen file.
inline constexpr std::string_view kScreenElement = "screen";

/// The root of a template library.
inline constexpr std::string_view kTemplatesElement = "templates";

/// The element declaring a template, which only a file's root holds.
inline constexpr std::string_view kTemplateElement = "template";

/// The element bringing a library's templates into a file.
inline constexpr std::string_view kImportElement = "import";

/// What a template library's path ends in, and what a screen's does.
inline constexpr std::string_view kLibraryExtension = ".amdt";
inline constexpr std::string_view kScreenExtension = ".amdn";

/// What joins an instance's name to the names inside it: `music.slider`, and an
/// import's prefix to the names it brings: `dialogs.confirm`.
inline constexpr char kNameSeparator = '.';

/// The attribute on a `<template>` listing what its instances pass it.
inline constexpr std::string_view kParamsAttribute = "params";

/// What marks a parameter's use inside a template: `@target`. Written twice,
/// it is itself.
inline constexpr char kParameterMark = '@';

/// A node a name was given to: where it sits in the table, and where the name
/// was written.
struct NamedNode
{
    uint32_t index = kNoNode;
    uint32_t line = 0;
    uint32_t column = 0;
};

/// One parameter a template takes, as its `params` declares it.
struct ParameterDeclaration
{
    std::string name;
    /// What an instance that passes nothing gets, when hasDefault.
    std::string fallback;
    bool hasDefault = false;
};

struct Template;

/// The template names one file can write, each meaning one template: those it
/// declares, and those it imports under the names its imports give them.
using Namespace = std::unordered_map<std::string, const Template *>;

/// A template a file declares: the declaration, for where to point, and the
/// one element it holds, which every instance is built from. Both point into
/// the parsed file, which outlives the compile.
struct Template
{
    std::vector<ParameterDeclaration> parameters;
    /// As its file declares it.
    std::string name;
    /// The library it is declared in, or empty for the file being compiled.
    std::string file;
    const MarkupElement *declaration = nullptr;
    const MarkupElement *root = nullptr;
    /// The names its body can write: its own file's, wherever it is used.
    const Namespace *home = nullptr;
};

/// The templates one file declares, and the names it can write.
///
/// Held where it does not move once filled: every Template it declares points
/// at `visible`, and every Namespace that imports one points into `declared`.
struct TemplateFile
{
    /// By the name the file declares it under. Node-based, so a Template stays
    /// where it is as more are added.
    std::unordered_map<std::string, Template> declared;
    Namespace visible;
    /// Every prefix an import has given, so two cannot share one.
    std::unordered_set<std::string> prefixes;
    /// The library's path, or empty for the file being compiled.
    std::string file;
};

/// One parameter's value in one instance.
struct Parameter
{
    std::string value;
    /// The prefix a node name in the value is qualified with: that of the
    /// place the value was written. A passed value was written where the
    /// instance sits; a default was written in the template.
    std::string resolveIn;
    /// False only in the check a template gets on its own, where a parameter
    /// nobody passed has no value; what uses it is checked per instance.
    bool known = true;
};

/// One template instance being compiled: what names inside it are qualified
/// with, and where it was written, for an error found inside it to say so.
struct Scope
{
    /// What `@name` means inside this instance.
    std::unordered_map<std::string, Parameter> parameters;
    /// What names inside this instance are qualified with: the instance's own
    /// qualified name, or its enclosing scope's prefix when it has no name.
    std::string prefix;
    /// The template as the instance names it.
    std::string_view templateName;
    const Template *used = nullptr;
    uint32_t line = 0;
    uint32_t column = 0;
    bool named = false;
    /// Whether this is the stand-in a template is checked through when the
    /// file may never use it, which was written nowhere.
    bool standIn = false;
};

/// A target a verb names, waiting for the walk to finish: a button may come
/// before the control it acts on, so a name can only be looked up once every
/// node has been read.
struct PendingTarget
{
    std::string name;
    uint32_t node = kNoNode; ///< the button whose verb names it
    uint32_t line = 0;
    uint32_t column = 0;
};

/// What compiling one element needs to know about the file around it.
struct Walk
{
    ScreenDocument &document;
    const Core::EventCatalog &catalog;
    /// Every node name written so far, so a second node carrying one is
    /// refused with the first's place in hand, and a target can be resolved.
    std::unordered_map<std::string, NamedNode> names;
    std::vector<PendingTarget> targets;
    /// The instances being compiled, outermost first.
    std::vector<Scope> scopes;
    /// The template names the file being compiled can write.
    const Namespace *space = nullptr;
    /// The file being compiled: empty for a screen, a library's path when one
    /// is checked on its own.
    std::string_view file;
    /// Whether some node has already claimed focus, so a second can be refused
    /// rather than quietly winning.
    bool focusClaimed = false;
};

/// Text with every parameter it uses replaced by its value.
struct Substituted
{
    std::string text;
    /// The parameter the text is, when it is exactly one `@name` and nothing
    /// else — which is when a node name in it is resolved where that
    /// parameter's value was written.
    const Parameter *sole = nullptr;
    /// False when any parameter used has no value yet.
    bool known = true;
};

// ── ScreenWalk.cpp ───────────────────────────────────────────────────────────

/// What a name written here is qualified with: the innermost instance's prefix,
/// or none outside every template.
[[nodiscard]] std::string_view CurrentPrefix(const Walk &walk);

/// @p name qualified with @p prefix.
[[nodiscard]] std::string QualifyIn(std::string_view prefix, std::string_view name);

/// Whether @p name is one a parameter or an import prefix can have: letters,
/// digits and '_', not starting with a digit.
[[nodiscard]] bool IsParameterName(std::string_view name);

/// @p text with each `@name` replaced by the innermost instance's value for it,
/// and each `@@` by `@`. Outside every template there are no parameters, and
/// `@` is plain text. @p line and @p column are where an error points.
[[nodiscard]] std::expected<Substituted, MarkupError> Substitute(const Walk &walk, std::string_view text, uint32_t line,
                                                                 uint32_t column);

/// The attributes an element carries, in the order the tables are tried: the
/// node's own, then its style. The root adds the screen's before both.
[[nodiscard]] std::expected<void, MarkupError> ApplyAttributes(Walk &walk, const MarkupElement &element,
                                                               uint32_t index);

/// Compiles @p element and everything inside it under node @p parent.
[[nodiscard]] std::expected<void, MarkupError> CompileElement(Walk &walk, const MarkupElement &element,
                                                              uint32_t parent);

/// Compiles an instance of @p used under @p parent. @p standIn marks the check
/// a template gets when nothing may use it.
[[nodiscard]] std::expected<void, MarkupError> CompileInstance(Walk &walk, const MarkupElement &instance,
                                                               const Template &used, uint32_t parent, bool standIn);

// ── ScreenActions.cpp ────────────────────────────────────────────────────────

/// What a control does when it fires, from its `on_click` as the file writes
/// it: a call is a verb, and a bare name is an event.
[[nodiscard]] std::expected<void, MarkupError> ApplyAction(Walk &walk, uint32_t index, const MarkupAttribute &written);

/// Every target a verb named, looked up now that every node has been read.
[[nodiscard]] std::expected<void, MarkupError> ResolveTargets(Walk &walk);

// ── ScreenTemplates.cpp ──────────────────────────────────────────────────────

/// Reads the templates @p root declares into @p into, refusing any that is
/// not one name over one built-in element.
///
/// Read before anything is compiled, so a template may be used above where it
/// is declared.
[[nodiscard]] std::expected<void, MarkupError> CollectTemplates(TemplateFile &into, const MarkupElement &root);

/// Compiles every template @p file declares once on its own, as though an
/// unnamed instance of it stood where it is declared, so a mistake in one
/// nothing uses still fails the cook. What it builds is thrown away. @p root is
/// the file's parsed root, walked for the order the file declares them in.
[[nodiscard]] std::expected<void, MarkupError> CheckTemplates(const TemplateFile &file, const MarkupElement &root,
                                                              const Core::EventCatalog &catalog);

} // namespace Assisi::Mondrian::Import
