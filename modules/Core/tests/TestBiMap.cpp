/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestBiMap.cpp
/// @brief The bijection invariant, which is the only reason the container exists.
///
/// Every case here is ultimately the same question asked from a different angle:
/// can the two directions be made to disagree? A plain lookup test would pass
/// against a BiMap that writes one side and drops the other, so the cases below
/// are written to fail against exactly that — they check the side that was *not*
/// asked about, and they check it after operations that were refused as well as
/// after ones that took.

#include <doctest/doctest.h>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/BiMap.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace Assisi::Core;

namespace
{

/// Left and right values are drawn from disjoint numeric ranges throughout, so a
/// container that crossed its two maps would resolve to a value that cannot
/// belong to the side it came from, rather than to a plausible one.
constexpr int32_t kRightBase = 1000;

using IntMap = BiMap<int32_t, int32_t>;

/// A left-hand key that is not an integer, exercising the custom-hash template
/// parameter that the NetSync use will not.
struct Handle
{
    std::uint32_t index      = 0;
    std::uint32_t generation = 0;

    bool operator==(const Handle &) const = default;
};

struct HandleHash
{
    std::size_t operator()(const Handle &handle) const
    {
        return std::hash<std::uint64_t>{}(static_cast<std::uint64_t>(handle.index) |
                                          (static_cast<std::uint64_t>(handle.generation) << 32));
    }
};

/// Bind a pair that the case under test is not itself about. Spelling the
/// expected out on every setup line buries the assertions that are the subject.
template <typename Map, typename L, typename R> void RequireInserted(Map &map, const L &left, const R &right)
{
    REQUIRE(map.Insert(left, right).has_value());
}

/// Deterministic, so a failure reproduces exactly rather than "sometimes".
std::uint32_t NextRandom(std::uint32_t &state)
{
    state = (state * 1664525u) + 1013904223u;
    return state >> 16u;
}

} // namespace

TEST_CASE("BiMap: an inserted pair resolves from both sides")
{
    IntMap map;
    REQUIRE(map.Insert(1, kRightBase + 1).has_value());

    REQUIRE(map.FindRight(1) != nullptr);
    REQUIRE(map.FindLeft(kRightBase + 1) != nullptr);
    CHECK(*map.FindRight(1) == kRightBase + 1);
    CHECK(*map.FindLeft(kRightBase + 1) == 1);
    CHECK(map.ContainsLeft(1));
    CHECK(map.ContainsRight(kRightBase + 1));
    CHECK(map.Size() == 1);
}

TEST_CASE("BiMap: a missing key resolves to nullptr on both sides")
{
    IntMap map;
    CHECK(map.FindRight(7) == nullptr);
    CHECK(map.FindLeft(kRightBase + 7) == nullptr);
    CHECK_FALSE(map.ContainsLeft(7));
    CHECK_FALSE(map.ContainsRight(kRightBase + 7));
    CHECK(map.Empty());
}

TEST_CASE("BiMap: a taken right is refused, and the refusal writes nothing")
{
    // The shape that cost NetSync an entity's replication: the *right* is the
    // side already spoken for, so a container that writes the left first and
    // then discovers the collision leaves a left row pointing at an id that does
    // not point back. Checking only the return value would pass against that.
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);

    const auto refused = map.Insert(2, kRightBase + 1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == BiMapError::RightTaken);

    CHECK(map.FindRight(2) == nullptr);
    CHECK_FALSE(map.ContainsLeft(2));
    CHECK(map.Size() == 1);

    // ...and the incumbent is untouched, rather than half-overwritten.
    REQUIRE(map.FindLeft(kRightBase + 1) != nullptr);
    CHECK(*map.FindLeft(kRightBase + 1) == 1);
    CHECK(*map.FindRight(1) == kRightBase + 1);
}

TEST_CASE("BiMap: a taken left is refused, and the refusal writes nothing")
{
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);

    const auto refused = map.Insert(1, kRightBase + 2);
    REQUIRE_FALSE(refused.has_value());
    // Which side collided, not merely that one did — the two are different
    // problems for the caller and the error names them apart.
    CHECK(refused.error() == BiMapError::LeftTaken);

    CHECK(map.FindLeft(kRightBase + 2) == nullptr);
    CHECK_FALSE(map.ContainsRight(kRightBase + 2));
    CHECK(map.Size() == 1);
    CHECK(*map.FindRight(1) == kRightBase + 1);
}

TEST_CASE("BiMap: re-inserting the same pair is idempotent, not an error")
{
    // A caller that cannot cheaply tell whether it has already bound a pairing
    // must be able to say so again without the answer reading as failure.
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);

    CHECK(map.Insert(1, kRightBase + 1).has_value());

    // Succeeded and created nothing. Size is what says so: the value channel is
    // void, so "it did not double up" has to be observed rather than returned.
    CHECK(map.Size() == 1);
    CHECK(*map.FindRight(1) == kRightBase + 1);
    CHECK(*map.FindLeft(kRightBase + 1) == 1);
}

TEST_CASE("BiMap: erasing by either side removes both directions")
{
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);
    RequireInserted(map, 2, kRightBase + 2);

    CHECK(map.EraseLeft(1));
    CHECK(map.FindRight(1) == nullptr);
    CHECK(map.FindLeft(kRightBase + 1) == nullptr); // the side not named by the call
    CHECK(map.Size() == 1);

    CHECK(map.EraseRight(kRightBase + 2));
    CHECK(map.FindLeft(kRightBase + 2) == nullptr);
    CHECK(map.FindRight(2) == nullptr); // likewise
    CHECK(map.Empty());

    CHECK_FALSE(map.EraseLeft(1));
    CHECK_FALSE(map.EraseRight(kRightBase + 2));
}

TEST_CASE("BiMap: a refused pairing succeeds once the incumbent is erased")
{
    // The recovery sequence a caller is expected to run: refuse, retire the
    // holder, retry. If the refusal had left anything behind, the retry would
    // collide with its own leftovers.
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);
    REQUIRE_FALSE(map.Insert(2, kRightBase + 1).has_value());

    CHECK(map.EraseLeft(1));
    CHECK(map.Insert(2, kRightBase + 1).has_value());

    CHECK(*map.FindRight(2) == kRightBase + 1);
    CHECK(*map.FindLeft(kRightBase + 1) == 2);
    CHECK(map.Size() == 1);
}

TEST_CASE("BiMap: erasing while iterating removes both directions")
{
    IntMap map;
    for (int32_t i = 0; i < 8; ++i)
        RequireInserted(map, i, kRightBase + i);

    std::vector<int32_t> dropped;
    for (auto it = map.begin(); it != map.end();)
    {
        if (it->first % 2 == 0)
        {
            dropped.push_back(it->second);
            it = map.Erase(it);
        }
        else
        {
            ++it;
        }
    }

    CHECK(map.Size() == 4);
    CHECK(dropped.size() == 4);
    for (const int32_t right : dropped)
    {
        CHECK(map.FindLeft(right) == nullptr); // the reverse row went with it
        CHECK_FALSE(map.ContainsRight(right));
    }
    for (int32_t i = 1; i < 8; i += 2)
    {
        REQUIRE(map.FindRight(i) != nullptr);
        CHECK(*map.FindLeft(kRightBase + i) == i);
    }
}

TEST_CASE("BiMap: Clear empties both directions")
{
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);
    RequireInserted(map, 2, kRightBase + 2);

    map.Clear();

    CHECK(map.Empty());
    CHECK(map.Size() == 0);
    CHECK(map.FindLeft(kRightBase + 1) == nullptr);
    CHECK(map.FindRight(1) == nullptr);
    // Nothing lingering on either side to collide with a fresh pairing.
    RequireInserted(map, 1, kRightBase + 2);
}

TEST_CASE("BiMap: a non-integral key works through a supplied hash")
{
    BiMap<Handle, int32_t, HandleHash> map;
    const Handle first{.index = 4, .generation = 1};
    const Handle reused{.index = 4, .generation = 2};                       // same slot, later life

    RequireInserted(map, first, kRightBase + 1);

    // Distinct keys: the generation is part of the identity, which is the whole
    // reason a handle is not just its index.
    CHECK(map.FindRight(reused) == nullptr);
    RequireInserted(map, reused, kRightBase + 2);
    CHECK(*map.FindLeft(kRightBase + 1) == first);
    CHECK(*map.FindLeft(kRightBase + 2) == reused);
}

TEST_CASE("BiMap: random operations never desynchronise the two directions")
{
    // The cases above each pin one operation. This one asks whether any *sequence*
    // of them can drift, by replaying every operation against a plain model and
    // then sweeping the whole key universe — both sides — after each step. The
    // sweep is what catches an orphan: a row on one side with no counterpart is
    // invisible to a test that only ever looks up keys it expects to find.
    constexpr int32_t kLefts = 12;

    IntMap map;
    std::unordered_map<int32_t, int32_t> leftToRight; // the model
    std::unordered_map<int32_t, int32_t> rightToLeft;
    std::uint32_t random = 0x5eed1234u;

    for (int32_t step = 0; step < 3000; ++step)
    {
        const std::uint32_t action = NextRandom(random) % 4u;
        const int32_t left   = static_cast<int32_t>(NextRandom(random) % static_cast<std::uint32_t>(kLefts));
        const int32_t right =
            kRightBase + static_cast<int32_t>(NextRandom(random) % static_cast<std::uint32_t>(kLefts));

        if (action == 0u)
        {
            const bool leftFree  = !leftToRight.contains(left);
            const bool rightFree = !rightToLeft.contains(right);
            const bool paired    = !leftFree && !rightFree && leftToRight[left] == right;

            const auto result = map.Insert(left, right);
            if (paired)
            {
                // Idempotent. The Size check below is what proves it created
                // nothing, since success carries no value to inspect.
                REQUIRE(result.has_value());
            }
            else if (leftFree && rightFree)
            {
                REQUIRE(result.has_value());
                leftToRight[left]  = right;
                rightToLeft[right] = left;
            }
            else
            {
                REQUIRE_FALSE(result.has_value());
                // Left is checked first, so a double collision reports LeftTaken.
                REQUIRE(result.error() == (!leftFree ? BiMapError::LeftTaken : BiMapError::RightTaken));
            }
        }
        else if (action == 1u)
        {
            const auto row = leftToRight.find(left);
            REQUIRE(map.EraseLeft(left) == (row != leftToRight.end()));
            if (row != leftToRight.end())
            {
                rightToLeft.erase(row->second);
                leftToRight.erase(row);
            }
        }
        else if (action == 2u)
        {
            const auto row = rightToLeft.find(right);
            REQUIRE(map.EraseRight(right) == (row != rightToLeft.end()));
            if (row != rightToLeft.end())
            {
                leftToRight.erase(row->second);
                rightToLeft.erase(row);
            }
        }
        else
        {
            // Iterating and erasing one arbitrary pair, which is the access
            // pattern a sweep uses and the one most able to leave an orphan.
            if (!map.Empty())
            {
                const auto it          = map.begin();
                const int32_t erasedLeft  = it->first;
                const int32_t erasedRight = it->second;
                (void)map.Erase(it);
                leftToRight.erase(erasedLeft);
                rightToLeft.erase(erasedRight);
            }
        }

        REQUIRE(map.Size() == leftToRight.size());

        // Both directions, over the entire universe of keys — present and absent
        // alike, so a row that should have gone is as visible as one that should
        // have stayed.
        for (int32_t key = 0; key < kLefts; ++key)
        {
            const auto expected = leftToRight.find(key);
            const int32_t *actual   = map.FindRight(key);
            if (expected == leftToRight.end())
            {
                REQUIRE(actual == nullptr);
            }
            else
            {
                REQUIRE(actual != nullptr);
                REQUIRE(*actual == expected->second);
            }

            const int32_t value        = kRightBase + key;
            const auto expectedLeft = rightToLeft.find(value);
            const int32_t *actualLeft   = map.FindLeft(value);
            if (expectedLeft == rightToLeft.end())
            {
                REQUIRE(actualLeft == nullptr);
            }
            else
            {
                REQUIRE(actualLeft != nullptr);
                REQUIRE(*actualLeft == expectedLeft->second);
            }
        }
    }
}

// The container's own invariant check, which only exists in debug. See
// Assert.hpp: the test handler turns a fired ASSISI_ASSERT into a throw.
#ifndef NDEBUG
TEST_CASE("BiMap guard: erasing the end iterator is caught")
{
    Assisi::Testing::ThrowOnContractViolation guard;
    IntMap map;
    RequireInserted(map, 1, kRightBase + 1);

    CHECK_THROWS_AS((void)map.Erase(map.end()), ContractViolation);
}
#endif
