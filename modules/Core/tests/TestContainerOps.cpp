/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestContainerOps.cpp
/// @brief A list reached only through its ContainerOps can lose a row and have
///        a row moved, as an editor that never knows its type needs; a
///        container whose length is fixed, or that is keyed, offers neither.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include <Assisi/Core/Reflect/ContainerOps.hpp>

using Assisi::Core::Reflect::ContainerOps;
using Assisi::Core::Reflect::ContainerSpecFor;

namespace
{

using Ints = std::vector<int32_t>;

const ContainerOps &IntsOps()
{
    return *ContainerSpecFor<Ints>()->ops;
}

std::byte *Bytes(Ints &values)
{
    return reinterpret_cast<std::byte *>(&values);
}

} // namespace

TEST_CASE("ContainerOps: a vector's row is erased and the rest close up")
{
    Ints values{1, 2, 3};
    IntsOps().erase(Bytes(values), 1);
    CHECK(values == Ints{1, 3});
    IntsOps().erase(Bytes(values), 1);
    CHECK(values == Ints{1});
}

TEST_CASE("ContainerOps: a vector's row moves to another place, the ones between shifting by one")
{
    Ints values{1, 2, 3, 4};
    IntsOps().move(Bytes(values), 3, 0);
    CHECK(values == Ints{4, 1, 2, 3});
    IntsOps().move(Bytes(values), 0, 3);
    CHECK(values == Ints{1, 2, 3, 4});
    IntsOps().move(Bytes(values), 1, 2);
    CHECK(values == Ints{1, 3, 2, 4});
    IntsOps().move(Bytes(values), 2, 2);
    CHECK(values == Ints{1, 3, 2, 4});
}

TEST_CASE("ContainerOps: an array and a map can't erase or move")
{
    const ContainerOps &array = *ContainerSpecFor<std::array<int32_t, 2>>()->ops;
    CHECK(array.erase == nullptr);
    CHECK(array.move == nullptr);
    const ContainerOps &map = *ContainerSpecFor<std::map<int32_t, int32_t>>()->ops;
    CHECK(map.erase == nullptr);
    CHECK(map.move == nullptr);
}
