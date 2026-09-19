/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>

using namespace Assisi::Mondrian;

namespace
{

/// A frame of each shape the UI must handle: a common window, and one narrower
/// than it is tall so a layout that mixes up the axes lands out of view.
constexpr Extent kLandscape{1280, 720};
constexpr Extent kPortrait{480, 960};

constexpr TextureId kPlaceholder{7};

/// Whether @p rect covers some pixels and all of them lie inside @p viewport.
bool VisibleWithin(const Rect &rect, Extent viewport)
{
    return rect.width > 0.f && rect.height > 0.f && rect.x >= 0.f && rect.y >= 0.f &&
           rect.x + rect.width <= static_cast<float>(viewport.width) &&
           rect.y + rect.height <= static_cast<float>(viewport.height);
}

/// Runs one whole frame and returns what it drew.
const DrawList &Frame(Ui &ui, Extent viewport)
{
    ui.ProcessInput();
    ui.Sync(viewport);
    return ui.GetDrawList();
}

/// Whether some quad in @p list has a corner of @p style.
bool HasCornerStyle(const DrawList &list, CornerStyle style)
{
    return std::ranges::any_of(list.Instances(), [style](const QuadInstance &quad)
                               { return UnpackCornerStyle(quad.cornerStyles, Corner::TopLeft) == style; });
}

} // namespace

TEST_CASE("Mondrian: the placeholder strip shows every quad the pass draws, inside the viewport")
{
    Ui ui;
    ui.SetPlaceholderTexture(kPlaceholder);
    const DrawList &drawn = Frame(ui, kLandscape);

    REQUIRE(drawn.IsFinalized());
    REQUIRE_FALSE(drawn.Instances().empty());
    for (const QuadInstance &quad : drawn.Instances())
    {
        CHECK(VisibleWithin(quad.rect, kLandscape));
    }

    CHECK(HasCornerStyle(drawn, CornerStyle::Square));
    CHECK(HasCornerStyle(drawn, CornerStyle::Rounded));
    CHECK(HasCornerStyle(drawn, CornerStyle::Cut));
    CHECK(std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad) { return quad.borderWidth > 0.f; }));

    // Clipped: the clip cuts into the quad rather than containing it.
    CHECK(std::ranges::any_of(drawn.Instances(),
                              [](const QuadInstance &quad)
                              { return quad.clip.width < quad.rect.width && quad.clip.width > 0.f; }));

    // Textured with whatever the engine registered.
    CHECK(std::ranges::any_of(drawn.Entries(), [](const DrawEntry &entry) { return entry.texture == kPlaceholder; }));
    CHECK(std::ranges::any_of(drawn.Instances(), [](const QuadInstance &quad)
                              { return quad.kind == static_cast<uint32_t>(QuadKind::Image); }));
}

TEST_CASE("Mondrian: every sync rebuilds the strip against the viewport it is given")
{
    Ui ui;
    const std::size_t landscapeCount = Frame(ui, kLandscape).Instances().size();
    const Rect landscapeFirst        = Frame(ui, kLandscape).Instances()[0].rect;
    const DrawList &portrait         = Frame(ui, kPortrait);

    // A second sync rebuilds rather than appends.
    REQUIRE(portrait.Instances().size() == landscapeCount);
    for (const QuadInstance &quad : portrait.Instances())
    {
        CHECK(VisibleWithin(quad.rect, kPortrait));
    }
    CHECK(portrait.Instances()[0].rect.width != landscapeFirst.width);
}

TEST_CASE("Mondrian: a zero-sized viewport draws nothing and does not assert")
{
    Ui ui;
    CHECK(Frame(ui, Extent{0, 0}).Instances().empty());
}

#ifndef NDEBUG
TEST_CASE("Mondrian: the two frame steps must alternate, input first")
{
    const Assisi::Testing::ThrowOnContractViolation guard;

    SUBCASE("sync before any input asserts")
    {
        Ui ui;
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("input twice without a sync asserts")
    {
        Ui ui;
        ui.ProcessInput();
        CHECK_THROWS_AS(ui.ProcessInput(), Assisi::Core::ContractViolation);
    }

    SUBCASE("sync twice without input asserts")
    {
        Ui ui;
        ui.ProcessInput();
        ui.Sync(kLandscape);
        CHECK_THROWS_AS(ui.Sync(kLandscape), Assisi::Core::ContractViolation);
    }

    SUBCASE("alternating frames are accepted")
    {
        Ui ui;
        CHECK_NOTHROW(Frame(ui, kLandscape));
        CHECK_NOTHROW(Frame(ui, kPortrait));
    }
}
#endif
