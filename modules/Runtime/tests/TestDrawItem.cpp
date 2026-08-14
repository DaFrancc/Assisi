/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include <Assisi/Render/DrawItem.hpp>

using Assisi::Render::DrawItem;
using Assisi::Render::MakeOpaqueSortKey;
using Assisi::Render::QuantizeDepthFrontToBack;

TEST_CASE("MakeOpaqueSortKey packs the fields into [pipeline|material|mesh|depth]")
{
    // Field widths: pipeline 8, material 20, mesh 20, depth 16 (= 64).
    const uint64_t key = MakeOpaqueSortKey(0, 0, 0, 0);
    CHECK(key == 0);

    // Depth occupies the low 16 bits verbatim.
    CHECK(MakeOpaqueSortKey(0, 0, 0, 0xABCD) == 0xABCDull);

    // Mesh sits just above depth (<< 16).
    CHECK(MakeOpaqueSortKey(0, 0, 1, 0) == (1ull << 16));

    // Material sits above mesh (<< 36).
    CHECK(MakeOpaqueSortKey(0, 1, 0, 0) == (1ull << 36));

    // Pipeline is the top byte (<< 56).
    CHECK(MakeOpaqueSortKey(1, 0, 0, 0) == (1ull << 56));

    // Ids beyond their field width are masked, not allowed to bleed into the next.
    CHECK(MakeOpaqueSortKey(0, (1u << 20), 0, 0) == 0);       // material overflow wraps to 0
    CHECK(MakeOpaqueSortKey(0, 0, (1u << 20), 0) == 0);       // mesh overflow wraps to 0
}

TEST_CASE("MakeOpaqueSortKey orders material-major, then mesh, then depth")
{
    // A higher material always outranks any mesh/depth beneath it.
    CHECK(MakeOpaqueSortKey(0, 1, 0, 0) > MakeOpaqueSortKey(0, 0, 999999, 0xFFFF));
    // Within one material, a higher mesh outranks any depth beneath it.
    CHECK(MakeOpaqueSortKey(0, 5, 2, 0) > MakeOpaqueSortKey(0, 5, 1, 0xFFFF));
    // Within one material+mesh, depth breaks the tie (front-to-back ascending).
    CHECK(MakeOpaqueSortKey(0, 5, 2, 10) < MakeOpaqueSortKey(0, 5, 2, 20));
}

TEST_CASE("QuantizeDepthFrontToBack maps near->0, far->max, and clamps")
{
    CHECK(QuantizeDepthFrontToBack(1.f, 1.f, 100.f) == 0);        // at the near plane
    CHECK(QuantizeDepthFrontToBack(100.f, 1.f, 100.f) == 65535);  // at the far plane
    CHECK(QuantizeDepthFrontToBack(0.5f, 1.f, 100.f) == 0);       // nearer than near -> clamped
    CHECK(QuantizeDepthFrontToBack(1000.f, 1.f, 100.f) == 65535); // beyond far -> clamped

    // Midpoint lands near the middle of the range.
    const uint16_t mid = QuantizeDepthFrontToBack(50.5f, 1.f, 100.f);
    CHECK(mid > 32000);
    CHECK(mid < 33500);

    // A degenerate (zero/negative) range is defined as 0, never a divide-by-zero.
    CHECK(QuantizeDepthFrontToBack(5.f, 10.f, 10.f) == 0);
}

TEST_CASE("Sorting DrawItems by sortKey groups by material then mesh, front-to-back")
{
    // Two materials (1,2), two meshes (10,11), assorted depths, deliberately
    // shuffled. After sorting by sortKey the order must be material-major.
    std::vector<DrawItem> items;
    const auto add = [&items](uint32_t material, uint32_t mesh, uint16_t depth)
                     {
                         DrawItem item;
                         item.sortKey = MakeOpaqueSortKey(0, material, mesh, depth);
                         items.push_back(item);
                     };
    add(2, 11, 100);
    add(1, 10, 200);
    add(1, 10, 50);
    add(2, 10, 10);
    add(1, 11, 5);

    std::sort(items.begin(), items.end(), [](const DrawItem &lhs, const DrawItem &rhs)
              { return lhs.sortKey < rhs.sortKey; });

    // Expected order: (m1,mesh10,d50),(m1,mesh10,d200),(m1,mesh11,d5),(m2,mesh10,d10),(m2,mesh11,d100)
    CHECK(items[0].sortKey == MakeOpaqueSortKey(0, 1, 10, 50));
    CHECK(items[1].sortKey == MakeOpaqueSortKey(0, 1, 10, 200));
    CHECK(items[2].sortKey == MakeOpaqueSortKey(0, 1, 11, 5));
    CHECK(items[3].sortKey == MakeOpaqueSortKey(0, 2, 10, 10));
    CHECK(items[4].sortKey == MakeOpaqueSortKey(0, 2, 11, 100));

    // And the sort is total: strictly ascending keys.
    for (std::size_t i = 1; i < items.size(); ++i)
    {
        CHECK(items[i - 1].sortKey < items[i].sortKey);
    }
}
