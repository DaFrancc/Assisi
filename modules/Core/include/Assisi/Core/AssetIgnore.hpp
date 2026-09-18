/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file AssetIgnore.hpp
/// @brief The one list saying which files under the asset root are not content.
///
/// `.assisiignore` files carry the list, in `.gitignore` syntax, and every
/// walker of the asset tree consults the same loaded copy: AssetDatabase's
/// reconcile pass mints no sidecar for an ignored file, and the editor's asset
/// browser does not list one.
///
/// This answers a different question from `.gitignore`, which is why it cannot
/// be that file: `.gitignore` says what is not worth versioning, and it already
/// excludes `assets/models/` and every `.spv`, which between them are most of
/// the art and all of the shaders the renderer loads.
///
/// **Ignoring is not deleting.** An ignored file stays on disk and stays
/// editable; it is invisible to the pipeline, which is what makes this the right
/// home for import sources that must sit beside their output.
///
/// ### The decision procedure
///
/// Rules from every `.assisiignore` form one ordered list, shallowest directory
/// first, each file's rules in the order written. A path is tested against each
/// rule together with every one of its ancestor directories, and the **last**
/// rule that matches any of them decides: ignored unless that rule is a `!`
/// negation.
///
/// That last part is a deliberate divergence from git, which cannot re-include a
/// file whose parent directory is excluded because it prunes the directory and
/// never descends. Here nothing is pruned, so excluding a directory wholesale
/// and keeping one file inside it works — the only tolerable way to express an
/// editor-only directory holding one asset a non-editor build still needs.
///
/// ### The supported syntax
///
/// Blank lines and `#` comments are skipped, as is trailing whitespace. A
/// leading `!` negates. A trailing `/` restricts a rule to directories. A `/`
/// anywhere else anchors the pattern to the directory holding the
/// `.assisiignore`; without one, the pattern matches a name at any depth below
/// it. Within a path segment `*` matches any run of characters and `?` matches
/// one, neither crossing a `/`; a whole segment of `**` matches zero or more
/// segments. Character classes and backslash escapes are not supported and a
/// line using them is dropped with a warning rather than silently misread.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Assisi::Core
{

/// @brief The name every ignore file in the asset tree carries.
inline constexpr std::string_view kAssetIgnoreFileName = ".assisiignore";

/// @brief Every `.assisiignore` rule under one asset root, ready to query.
class AssetIgnoreList
{
public:
    /// @brief Collect every `.assisiignore` under @p root into one rule list.
    ///
    /// Shallower files are added first, so a nested file's rules come last and
    /// therefore win — that is what lets a subdirectory re-include something its
    /// parent excluded. An unreadable ignore file is warned about and skipped;
    /// a root that cannot be walked yields an empty list, which ignores nothing.
    [[nodiscard]] static AssetIgnoreList Load(const std::filesystem::path &root);

    /// @brief Append the rules in @p text, as if read from a `.assisiignore`
    ///        sitting in @p baseDir (a virtual directory path, empty for the
    ///        asset root). Order of calls is the order rules are consulted.
    void AddRules(std::string_view text, std::string_view baseDir);

    /// @brief Whether the file at virtual path @p vpath is not content.
    ///        A `.assisiignore` is always ignored: it describes the pipeline
    ///        rather than feeding it, and minting it a sidecar would make the
    ///        list itself an addressable asset.
    [[nodiscard]] bool IsFileIgnored(std::string_view vpath) const;

    /// @brief Whether the directory at virtual path @p vdir is not content.
    ///
    /// A directory being ignored does not mean everything beneath it is: a later
    /// negation can re-include a file inside. Callers that must not lose such a
    /// file — the browser, which would otherwise make it unreachable — check the
    /// files themselves rather than stopping here.
    [[nodiscard]] bool IsDirectoryIgnored(std::string_view vdir) const;

    /// @brief How many rules were parsed. Zero means nothing is ignored.
    [[nodiscard]] std::size_t RuleCount() const noexcept;

private:
    /// @brief One parsed line of one `.assisiignore`.
    struct Rule
    {
        /// Virtual directory of the `.assisiignore` this came from, empty for
        /// the asset root. A rule never reaches outside its own directory.
        std::string baseDir;

        /// The pattern split on '/'. A segment of `**` matches zero or more
        /// segments; any other segment matches exactly one.
        std::vector<std::string> segments;

        /// How many path segments `baseDir` spans. Cached so a query can find
        /// where a candidate's own segments begin without re-splitting it.
        std::uint32_t baseSegments = 0;

        bool negate   = false; ///< A leading `!`: re-include rather than exclude.
        bool dirOnly  = false; ///< A trailing `/`: matches directories only.
        bool anchored = false; ///< Matched from `baseDir`, not against a bare name.
    };

    /// @brief Shared spine of the two public queries: @p isDirectory only
    ///        changes how the last path component is classified.
    [[nodiscard]] bool Matches(std::string_view vpath, bool isDirectory) const;

    std::vector<Rule> _rules;
};

} // namespace Assisi::Core
