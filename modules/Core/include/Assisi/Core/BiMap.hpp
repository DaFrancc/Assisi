/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file BiMap.hpp
/// @brief A one-to-one pairing that can be looked up from either side in O(1).
///
/// The point is not lookup speed; it is that two hash maps kept as each other's
/// inverse have to be *written* as a pair, and nothing in the type system says
/// so. Half-installed pairings are invisible — each direction is only consulted
/// through its own map, so the half that exists looks correct from one side and
/// absent from the other. Here every mutation writes both directions or neither,
/// and both are checked before either is touched.
///
/// **Insert refuses; it never evicts.** A value already paired with somebody
/// else is a disagreement only the caller can resolve, and overwriting would
/// orphan the incumbent's row on the *other* side. Replacement is erase then
/// insert: two visible operations instead of one silent one.
///
/// **Neither side is mutable through this API.** Lookups hand back `const`
/// pointers and iteration is const-only — rewriting half of a pairing in place
/// is indistinguishable from the corruption this type prevents.
///
/// Both keys are stored twice, so this is for small, cheap-to-copy keys — ids,
/// handles, indices — not for pairing two large strings. The alternative is one
/// map plus a linear scan for the reverse direction: equally unbreakable, O(n)
/// to read. Prefer this when the reverse lookup runs per-entity per-frame.

#include <Assisi/Core/Assert.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <unordered_map>

namespace Assisi::Core
{

/// @brief Why a pairing was refused.
///
/// Which side collided is the useful half of the answer: it tells the caller
/// whether the value it is trying to bind is the problem, or the key it wanted
/// to bind it to. A single "conflict" would make both look the same.
enum class BiMapError : std::uint8_t
{
    LeftTaken,  ///< The left value is already paired with a different right.
    RightTaken, ///< The right value is already paired with a different left.
};

/// @brief A bijection between @p Left and @p Right, indexed both ways.
///
/// Values of both types must be unique across the container: it maps one Left to
/// exactly one Right and back. See the file comment for why Insert refuses a
/// collision rather than resolving it.
template <typename Left, typename Right, typename LeftHash = std::hash<Left>,
          typename RightHash = std::hash<Right>>
class BiMap
{
public:
    using LeftType   = Left;
    using RightType  = Right;
    using ForwardMap = std::unordered_map<Left, Right, LeftHash>;

    /// Const throughout: see the file comment. Yields `pair<const Left, Right>`
    /// in whatever order the underlying hash map holds them.
    using Iterator = typename ForwardMap::const_iterator;

    /// @brief Pair @p left with @p right, unless either is already spoken for.
    ///
    /// Both sides are checked before either is written, so there is no path on
    /// which this returns having modified one map and not the other.
    ///
    /// Idempotent: re-binding a pairing that already holds succeeds and changes
    /// nothing. That is the ordinary case for a caller that cannot cheaply tell
    /// whether it has bound this pair before, and it must not read as failure.
    /// An error means nothing changed either — the container is untouched on
    /// every path but the one that pairs.
    [[nodiscard]] std::expected<void, BiMapError> Insert(const Left &left, const Right &right)
    {
        const auto leftRow   = _byLeft.find(left);
        const auto rightRow  = _byRight.find(right);
        const bool leftFree  = leftRow == _byLeft.end();
        const bool rightFree = rightRow == _byRight.end();

        if (!leftFree && !rightFree && leftRow->second == right)
        {
            ASSISI_ASSERT(rightRow->second == left, "BiMap: the two directions disagree about an existing pairing");
            return {};
        }

        if (!leftFree)
            return std::unexpected(BiMapError::LeftTaken);
        if (!rightFree)
            return std::unexpected(BiMapError::RightTaken);

        // Both sides free, so neither insert below can fail. That is what makes a
        // half-applied pairing impossible here rather than merely unlikely.
        _byLeft.emplace(left, right);
        _byRight.emplace(right, left);
        return {};
    }

    /// @brief The Right paired with @p left, or nullptr.
    ///
    /// The pointer is into the container and is invalidated by the next erase or
    /// rehash, like any node's address in an unordered_map. Read it, do not keep
    /// it across a mutation.
    [[nodiscard]] const Right *FindRight(const Left &left) const
    {
        const auto row = _byLeft.find(left);
        return row == _byLeft.end() ? nullptr : &row->second;
    }

    /// @brief The Left paired with @p right, or nullptr. Same lifetime caveat.
    [[nodiscard]] const Left *FindLeft(const Right &right) const
    {
        const auto row = _byRight.find(right);
        return row == _byRight.end() ? nullptr : &row->second;
    }

    [[nodiscard]] bool ContainsLeft(const Left &left) const { return _byLeft.contains(left); }

    [[nodiscard]] bool ContainsRight(const Right &right) const { return _byRight.contains(right); }

    /// @brief Drop the pair @p left belongs to. False if it had none.
    bool EraseLeft(const Left &left)
    {
        const auto row = _byLeft.find(left);
        if (row == _byLeft.end())
            return false;

        _byRight.erase(row->second); // before the row that names it goes away
        _byLeft.erase(row);
        return true;
    }

    /// @brief Drop the pair @p right belongs to. False if it had none.
    bool EraseRight(const Right &right)
    {
        const auto row = _byRight.find(right);
        if (row == _byRight.end())
            return false;

        _byLeft.erase(row->second);
        _byRight.erase(row);
        return true;
    }

    /// @brief Drop the pair @p it names and return the iterator after it.
    ///
    /// The erase-while-iterating form: `it = map.Erase(it)` in the branch that
    /// removes, `++it` in the branch that keeps.
    Iterator Erase(Iterator it)
    {
        ASSISI_ASSERT(it != _byLeft.end(), "BiMap: Erase called with the end iterator");

        _byRight.erase(it->second);
        return _byLeft.erase(it);
    }

    void Clear()
    {
        _byLeft.clear();
        _byRight.clear();
    }

    [[nodiscard]] std::size_t Size() const { return _byLeft.size(); }

    [[nodiscard]] bool Empty() const { return _byLeft.empty(); }

    [[nodiscard]] Iterator begin() const { return _byLeft.begin(); }

    [[nodiscard]] Iterator end() const { return _byLeft.end(); }

private:
    ForwardMap _byLeft;
    std::unordered_map<Right, Left, RightHash> _byRight;
};

} // namespace Assisi::Core
