/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Core/AssetIgnore.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

#include <Assisi/Core/Logger.hpp>

namespace Assisi::Core
{
namespace
{
namespace fs = std::filesystem;

/// A whole pattern segment of this matches any number of path segments,
/// including none.
constexpr std::string_view kDoubleStar = "**";

/// Characters of `.gitignore` syntax this matcher does not implement. A line
/// carrying one is dropped rather than read as a literal: reading it literally
/// would match nothing and leave the author believing a file was excluded.
constexpr std::string_view kUnsupportedPatternChars = "[]\\";

/// @brief The path's components, ignoring empty ones so a stray leading,
///        trailing or doubled '/' costs nothing. Views into @p vpath.
std::vector<std::string_view> SplitSegments(std::string_view vpath)
{
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start < vpath.size())
    {
        const std::size_t slash = vpath.find('/', start);
        const std::size_t end   = (slash == std::string_view::npos) ? vpath.size() : slash;
        if (end > start)
        {
            segments.push_back(vpath.substr(start, end - start));
        }
        start = end + 1;
    }
    return segments;
}

/// @brief Number of path components in @p vpath.
std::uint32_t SegmentCount(std::string_view vpath)
{
    return static_cast<std::uint32_t>(SplitSegments(vpath).size());
}

/// @brief The last path component of @p vpath, or the whole thing if it has no
///        separator.
std::string_view BaseName(std::string_view vpath)
{
    const std::size_t slash = vpath.find_last_of('/');
    return (slash == std::string_view::npos) ? vpath : vpath.substr(slash + 1);
}

/// @brief Glob one path segment against one pattern segment: `*` matches any run
///        of characters, `?` exactly one. Neither can reach past the segment,
///        because a separator never appears in either argument.
///
/// Iterative with a single backtrack point, so a pattern of many stars cannot
/// drive exponential retries the way the naive recursion does.
bool MatchSegment(std::string_view pattern, std::string_view text)
{
    std::size_t patternIndex = 0;
    std::size_t textIndex    = 0;
    std::size_t starPattern  = std::string_view::npos;
    std::size_t starText     = 0;

    while (textIndex < text.size())
    {
        if (patternIndex < pattern.size() &&
            (pattern[patternIndex] == '?' || pattern[patternIndex] == text[textIndex]))
        {
            ++patternIndex;
            ++textIndex;
        }
        else if (patternIndex < pattern.size() && pattern[patternIndex] == '*')
        {
            // Remember where to resume if the rest fails, then try the shortest
            // possible run for this star first.
            starPattern = patternIndex;
            starText    = textIndex;
            ++patternIndex;
        }
        else if (starPattern != std::string_view::npos)
        {
            ++starText;
            patternIndex = starPattern + 1;
            textIndex    = starText;
        }
        else
        {
            return false;
        }
    }

    while (patternIndex < pattern.size() && pattern[patternIndex] == '*')
    {
        ++patternIndex;
    }
    return patternIndex == pattern.size();
}

/// @brief Glob a whole segment list. A `**` segment consumes zero or more text
///        segments; every other segment must match exactly one.
bool MatchSegments(std::span<const std::string> pattern, std::span<const std::string_view> text)
{
    std::size_t patternIndex = 0;
    std::size_t textIndex    = 0;

    while (patternIndex < pattern.size())
    {
        if (pattern[patternIndex] == kDoubleStar)
        {
            // Try every split point. The recursion depth is bounded by the
            // number of `**` segments in one pattern, not by the path's depth.
            for (std::size_t split = textIndex; split <= text.size(); ++split)
            {
                if (MatchSegments(pattern.subspan(patternIndex + 1), text.subspan(split)))
                {
                    return true;
                }
            }
            return false;
        }
        if (textIndex >= text.size() || !MatchSegment(pattern[patternIndex], text[textIndex]))
        {
            return false;
        }
        ++patternIndex;
        ++textIndex;
    }
    return textIndex == text.size();
}

/// @brief Read a whole file by absolute path. nullopt on any I/O failure.
///
/// Local rather than routed through AssetSystem: the ignore list is loaded from
/// an explicit root, so it works for a caller that has no asset root mounted.
std::optional<std::string> ReadWholeFile(const fs::path &path)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    if (!stream.good() && !stream.eof())
    {
        return std::nullopt;
    }
    return buffer.str();
}

} // namespace

AssetIgnoreList AssetIgnoreList::Load(const fs::path &root)
{
    AssetIgnoreList list;

    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec))
    {
        return list;
    }

    std::error_code walkEc;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, walkEc);
    const fs::recursive_directory_iterator end;
    if (walkEc)
    {
        Log::Warn("AssetIgnore: cannot walk '{}': {}; nothing will be ignored.", root.generic_string(),
                  walkEc.message());
        return list;
    }

    std::vector<std::string> baseDirs;
    for (; it != end; it.increment(walkEc))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || entryEc)
        {
            continue;
        }
        if (it->path().filename().generic_string() != kAssetIgnoreFileName)
        {
            continue;
        }

        std::string baseDir = fs::relative(it->path().parent_path(), root, entryEc).generic_string();
        if (entryEc)
        {
            continue;
        }
        if (baseDir == ".")
        {
            baseDir.clear();
        }
        baseDirs.push_back(std::move(baseDir));
    }
    if (walkEc)
    {
        Log::Warn("AssetIgnore: the walk of '{}' ended early: {}; some rules may be missing.", root.generic_string(),
                  walkEc.message());
    }

    // Shallowest first, so a nested file's rules are consulted last and win.
    // Depth decides; the path only breaks ties, to keep the order independent of
    // how the filesystem happened to enumerate.
    std::sort(baseDirs.begin(), baseDirs.end(),
              [](const std::string &left, const std::string &right)
              {
                  const std::uint32_t leftDepth  = SegmentCount(left);
                  const std::uint32_t rightDepth = SegmentCount(right);
                  if (leftDepth != rightDepth)
                  {
                      return leftDepth < rightDepth;
                  }
                  return left < right;
              });

    for (const std::string &baseDir : baseDirs)
    {
        const fs::path path =
            baseDir.empty() ? root / kAssetIgnoreFileName : root / baseDir / kAssetIgnoreFileName;
        const std::optional<std::string> text = ReadWholeFile(path);
        if (!text.has_value())
        {
            Log::Warn("AssetIgnore: cannot read '{}', skipping it.", path.generic_string());
            continue;
        }
        list.AddRules(*text, baseDir);
    }

    return list;
}

void AssetIgnoreList::AddRules(std::string_view text, std::string_view baseDir)
{
    std::string_view base = baseDir;
    while (!base.empty() && base.back() == '/')
    {
        base.remove_suffix(1);
    }

    std::size_t lineStart = 0;
    while (lineStart <= text.size())
    {
        const std::size_t newline = text.find('\n', lineStart);
        const std::size_t lineEnd = (newline == std::string_view::npos) ? text.size() : newline;
        std::string_view line     = text.substr(lineStart, lineEnd - lineStart);
        lineStart                 = lineEnd + 1;

        // Trailing whitespace is not part of a pattern, and a CRLF checkout must
        // not leave a '\r' glued to every rule.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
        {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        if (line.find_first_of(kUnsupportedPatternChars) != std::string_view::npos)
        {
            Log::Warn("AssetIgnore: '{}' uses a character class or an escape, which are not supported; "
                      "the rule is dropped.",
                      line);
            continue;
        }

        Rule rule;
        rule.baseDir      = std::string(base);
        rule.baseSegments = SegmentCount(base);

        if (line.front() == '!')
        {
            rule.negate = true;
            line.remove_prefix(1);
        }
        if (line.back() == '/')
        {
            rule.dirOnly = true;
            while (!line.empty() && line.back() == '/')
            {
                line.remove_suffix(1);
            }
        }
        // A separator anywhere inside pins the pattern to this ignore file's own
        // directory; without one it matches a name at any depth below it.
        rule.anchored = line.find('/') != std::string_view::npos;

        for (std::string_view segment : SplitSegments(line))
        {
            rule.segments.emplace_back(segment);
        }
        if (rule.segments.empty())
        {
            continue;
        }

        _rules.push_back(std::move(rule));
    }
}

bool AssetIgnoreList::IsFileIgnored(std::string_view vpath) const
{
    // The list describes the pipeline rather than feeding it, so it is never
    // content — whatever it does or does not say about itself.
    if (BaseName(vpath) == kAssetIgnoreFileName)
    {
        return true;
    }
    return Matches(vpath, false);
}

bool AssetIgnoreList::IsDirectoryIgnored(std::string_view vdir) const
{
    return Matches(vdir, true);
}

std::size_t AssetIgnoreList::RuleCount() const noexcept
{
    return _rules.size();
}

bool AssetIgnoreList::Matches(std::string_view vpath, bool isDirectory) const
{
    const std::vector<std::string_view> parts = SplitSegments(vpath);
    if (parts.empty())
    {
        return false;
    }

    bool ignored = false;
    for (const Rule &rule : _rules)
    {
        // A rule reaches only what is strictly under its own directory.
        if (!rule.baseDir.empty() &&
            !(vpath.size() > rule.baseDir.size() && vpath.starts_with(rule.baseDir) &&
              vpath[rule.baseDir.size()] == '/'))
        {
            continue;
        }

        // The candidates are every ancestor directory of the path and then the
        // path itself: testing the ancestors is how a directory rule reaches the
        // files beneath it without the walk having to prune anything.
        for (std::size_t last = rule.baseSegments; last < parts.size(); ++last)
        {
            const bool candidateIsDirectory = (last + 1 < parts.size()) || isDirectory;
            if (rule.dirOnly && !candidateIsDirectory)
            {
                continue;
            }

            const std::span<const std::string_view> relative(parts.data() + rule.baseSegments,
                                                             last + 1 - rule.baseSegments);
            const bool hit = rule.anchored ? MatchSegments(rule.segments, relative)
                                           : MatchSegment(rule.segments.front(), relative.back());
            if (hit)
            {
                ignored = !rule.negate;
                break;
            }
        }
    }
    return ignored;
}

} // namespace Assisi::Core
