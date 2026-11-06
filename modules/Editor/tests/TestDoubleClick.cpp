/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestDoubleClick.cpp
/// @brief A second press soon enough and near enough completes a double-click;
///        one too late, too far or third in a row does not.

#include <doctest/doctest.h>

#include <Assisi/Editor/DoubleClick.hpp>

using Assisi::Editor::DoubleClick;
using Assisi::Editor::kDoubleClickPixels;
using Assisi::Editor::kDoubleClickSeconds;

namespace
{

constexpr glm::vec2 kHere{100.f, 100.f};

/// Well inside the time a double-click allows.
constexpr double kQuickly = kDoubleClickSeconds * 0.5;

} // namespace

TEST_CASE("DoubleClick: two quick presses in one place complete one")
{
    DoubleClick clicks;
    CHECK_FALSE(clicks.Press(1.0, kHere));
    CHECK(clicks.Press(1.0 + kQuickly, kHere));
}

TEST_CASE("DoubleClick: a second press too late or too far away is a single")
{
    DoubleClick late;
    CHECK_FALSE(late.Press(1.0, kHere));
    CHECK_FALSE(late.Press(1.0 + kDoubleClickSeconds * 2.0, kHere));

    DoubleClick far;
    CHECK_FALSE(far.Press(1.0, kHere));
    CHECK_FALSE(far.Press(1.0 + kQuickly, kHere + glm::vec2(kDoubleClickPixels * 2.f, 0.f)));
}

TEST_CASE("DoubleClick: a third quick press starts afresh, and a fourth completes another")
{
    DoubleClick clicks;
    CHECK_FALSE(clicks.Press(1.0, kHere));
    CHECK(clicks.Press(1.0 + kQuickly, kHere));
    CHECK_FALSE(clicks.Press(1.0 + 2.0 * kQuickly, kHere));
    CHECK(clicks.Press(1.0 + 3.0 * kQuickly, kHere));
}
