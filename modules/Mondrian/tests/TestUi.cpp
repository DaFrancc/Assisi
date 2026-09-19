/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#include <Assisi/Mondrian/Ui.hpp>
#include <Assisi/Testing/ThrowOnContractViolation.hpp>

#include <doctest/doctest.h>

#include <cstdint>

using namespace Assisi::Mondrian;

namespace
{

/// A frame of each shape the UI must handle: a common window, and one narrower
/// than it is tall so a layout that mixes up the axes lands out of view.
constexpr Extent kLandscape{1280, 720};
constexpr Extent kPortrait{480, 960};

/// Whether @p quad covers some pixels and all of them lie inside @p viewport.
bool VisibleWithin(const DrawQuad &quad, Extent viewport)
{
    const Rect &rect = quad.rect;
    return rect.width > 0.f && rect.height > 0.f && rect.x >= 0.f && rect.y >= 0.f &&
           rect.x + rect.width <= static_cast<float>(viewport.width) &&
           rect.y + rect.height <= static_cast<float>(viewport.height) && quad.color.a > 0.f;
}

/// Runs one whole frame and returns the quads it produced.
DrawList Frame(Ui &ui, Extent viewport)
{
    ui.ProcessInput();
    ui.Sync(viewport);
    return ui.GetDrawList();
}

} // namespace

TEST_CASE("Mondrian: a synced UI draws one visible quad inside the viewport")
{
    Ui ui;
    const DrawList drawn = Frame(ui, kLandscape);

    REQUIRE(drawn.Quads().size() == 1);
    CHECK(VisibleWithin(drawn.Quads()[0], kLandscape));
}

TEST_CASE("Mondrian: every sync lays out against the viewport it is given")
{
    Ui ui;
    const DrawList landscapeList = Frame(ui, kLandscape);
    REQUIRE(landscapeList.Quads().size() == 1);
    const DrawQuad landscape = landscapeList.Quads()[0];
    const DrawList portrait  = Frame(ui, kPortrait);

    // A second sync rebuilds rather than appends.
    REQUIRE(portrait.Quads().size() == 1);
    CHECK(VisibleWithin(portrait.Quads()[0], kPortrait));
    CHECK(portrait.Quads()[0].rect.width != landscape.rect.width);
    CHECK(portrait.Quads()[0].rect.height != landscape.rect.height);
}

TEST_CASE("Mondrian: a zero-sized viewport draws nothing visible and does not assert")
{
    Ui ui;
    const DrawList drawn = Frame(ui, Extent{0, 0});
    for (const DrawQuad &quad : drawn.Quads())
    {
        CHECK(quad.rect.width == 0.f);
        CHECK(quad.rect.height == 0.f);
    }
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
