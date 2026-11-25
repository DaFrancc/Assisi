/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Render/PictureMips.hpp>

using Assisi::Render::PictureMipCount;

TEST_CASE("A picture has levels down to one pixel on its longer side")
{
    CHECK(PictureMipCount(512u, 512u) == 10u);
    CHECK(PictureMipCount(512u, 320u) == 10u);
    CHECK(PictureMipCount(320u, 512u) == 10u);
    CHECK(PictureMipCount(513u, 1u) == 10u);
    CHECK(PictureMipCount(2u, 2u) == 2u);
    CHECK(PictureMipCount(1u, 1u) == 1u);
    CHECK(PictureMipCount(0u, 0u) == 1u);
}
