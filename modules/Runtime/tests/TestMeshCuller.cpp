/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <span>

#include <Assisi/Render/MeshCuller.hpp>

using Assisi::Render::CullTableBuilder;
using Assisi::Render::CullTables;
using Assisi::Render::GpuSubMesh;
using Assisi::Render::EncodeCullMaterial;
using Assisi::Render::MeshPipeline;
using Assisi::Render::kNoMaterial;
using Assisi::Render::MeshGeometry;

namespace
{
// A distinct dedup key per mesh — AddInstanceRaw only uses it for identity, so a
// plain address works. Two arbitrary stand-ins for "mesh A" and "mesh B".
const int32_t kMeshA = 0;
const int32_t kMeshB = 0;

// Builds a MeshGeometry over caller-owned submesh storage (the span is only read
// during AddInstanceRaw). vertexBase/indexBase distinguish arena placement.
MeshGeometry MakeGeometry(std::span<const GpuSubMesh> submeshes, uint32_t vertexBase, uint32_t indexBase)
{
    MeshGeometry geometry;
    geometry.sphere        = glm::vec4(1.f, 2.f, 3.f, 4.f);
    geometry.aabbMin       = glm::vec4(-1.f, -2.f, -3.f, 0.f);
    geometry.aabbMax       = glm::vec4(1.f, 2.f, 3.f, 0.f);
    geometry.vertexBase    = vertexBase;
    geometry.indexBase     = indexBase;
    geometry.lod0Submeshes = submeshes;
    return geometry;
}
} // namespace

TEST_CASE("AddInstanceRaw packs one object, its mesh descriptor, submeshes, and materials")
{
    CullTableBuilder builder;

    const std::array<GpuSubMesh, 2> submeshes{
        GpuSubMesh{ /*indexOffset=*/ 0, /*indexCount=*/ 6, /*materialSlot=*/ 0, 0},
        GpuSubMesh{ /*indexOffset=*/ 6, /*indexCount=*/ 12, /*materialSlot=*/ 1, 0},
    };
    const std::array<uint32_t, 2> materials{10u, 20u};

    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, /*vertexBase=*/ 100, /*indexBase=*/ 200),
                           glm::mat4(1.f), materials);

    const CullTables &tables = builder.Tables();
    REQUIRE(tables.objects.size() == 1);
    REQUIRE(tables.meshDescs.size() == 1);
    REQUIRE(tables.submeshes.size() == 2);
    REQUIRE(tables.objectMaterials.size() == 2);

    // The mesh descriptor: LOD0 range, arena bases, and bounds carried verbatim.
    const auto &desc = tables.meshDescs.front();
    CHECK(desc.firstSubmesh == 0);
    CHECK(desc.submeshCount == 2);
    CHECK(desc.vertexBase == 100);
    CHECK(desc.indexBase == 200);
    CHECK(desc.sphere.w == doctest::Approx(4.f));

    // Submeshes copied in draw order.
    CHECK(tables.submeshes[0].indexCount == 6);
    CHECK(tables.submeshes[1].indexOffset == 6);
    CHECK(tables.submeshes[1].materialSlot == 1);

    // The object references the descriptor and owns a contiguous material slice.
    const auto &obj = tables.objects.front();
    CHECK(obj.meshDescIndex == 0);
    CHECK(obj.materialBase == 0);
    CHECK(obj.materialCount == 2);
    CHECK(tables.objectMaterials[0] == 10u);
    CHECK(tables.objectMaterials[1] == 20u);

    // drawCapacity is the per-object LOD0 submesh sum (the max draws the pass emits).
    CHECK(tables.drawCapacity == 2);
}

TEST_CASE("AddInstanceRaw dedups a repeated mesh but appends a fresh object + material slice")
{
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};

    const std::array<uint32_t, 1> first{7u};
    const std::array<uint32_t, 1> second{8u};
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), first);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), second);

    const CullTables &tables = builder.Tables();
    // Same key → one descriptor and one submesh copy, but two objects.
    CHECK(tables.meshDescs.size() == 1);
    CHECK(tables.submeshes.size() == 1);
    REQUIRE(tables.objects.size() == 2);

    CHECK(tables.objects[0].meshDescIndex == 0);
    CHECK(tables.objects[1].meshDescIndex == 0);
    // Each object's material slice is its own — the second starts after the first.
    CHECK(tables.objects[0].materialBase == 0);
    CHECK(tables.objects[1].materialBase == 1);
    CHECK(tables.objectMaterials[1] == 8u);
    CHECK(tables.drawCapacity == 2);
}

TEST_CASE("A second distinct mesh appends its submeshes after the first's")
{
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 2> a{GpuSubMesh{0, 6, 0, 0}, GpuSubMesh{6, 6, 0, 0}};
    const std::array<GpuSubMesh, 1> b{GpuSubMesh{0, 9, 0, 0}};
    const std::array<uint32_t, 1>   mat{0u};

    builder.AddInstanceRaw(&kMeshA, MakeGeometry(a, 0, 0), glm::mat4(1.f), mat);
    builder.AddInstanceRaw(&kMeshB, MakeGeometry(b, 50, 60), glm::mat4(1.f), mat);

    const CullTables &tables = builder.Tables();
    REQUIRE(tables.meshDescs.size() == 2);
    REQUIRE(tables.submeshes.size() == 3);
    // Mesh B's descriptor points past mesh A's two submeshes.
    CHECK(tables.meshDescs[1].firstSubmesh == 2);
    CHECK(tables.meshDescs[1].submeshCount == 1);
    CHECK(tables.meshDescs[1].vertexBase == 50);
    CHECK(tables.drawCapacity == 3);
}

TEST_CASE("An unresolved material slot packs the skip sentinel")
{
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 2> submeshes{GpuSubMesh{0, 3, 0, 0}, GpuSubMesh{3, 3, 1, 0}};
    const std::array<uint32_t, 2>   materials{5u, kNoMaterial};

    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), materials);

    const CullTables &tables = builder.Tables();
    CHECK(tables.objectMaterials[0] == 5u);
    CHECK(tables.objectMaterials[1] == kNoMaterial);
    // Capacity still counts every LOD0 submesh — the shader skips the sentinel one
    // at draw time, but the buffer is sized for the upper bound.
    CHECK(tables.drawCapacity == 2);
}

TEST_CASE("Geometry with no LOD0 submeshes is a no-op")
{
    CullTableBuilder builder;
    const std::array<uint32_t, 1> mat{0u};
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(std::span<const GpuSubMesh>{}, 0, 0), glm::mat4(1.f), mat);

    CHECK(builder.Tables().Empty());
    CHECK(builder.Tables().objects.empty());
    CHECK(builder.Tables().meshDescs.empty());
}

TEST_CASE("Finalize builds one draw template per batch with reserved instance bases")
{
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 2> submeshes{
        GpuSubMesh{ /*indexOffset=*/ 0, /*indexCount=*/ 6, 0, 0},
        GpuSubMesh{ /*indexOffset=*/ 6, /*indexCount=*/ 12, 1, 0},
    };
    const std::array<uint32_t, 2> materials{10u, 20u};
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, /*vertexBase=*/ 100, /*indexBase=*/ 200), glm::mat4(1.f),
                           materials);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    REQUIRE(tables.batchTemplates.size() == 2);
    REQUIRE(tables.BatchCount() == 2);

    // Batch 0: geometry from submesh 0, arena-absolute firstIndex, instanceCount 0.
    const auto &b0 = tables.batchTemplates[0];
    CHECK(b0.indexCount == 6);
    CHECK(b0.firstIndex == 200);      // indexBase 200 + indexOffset 0
    CHECK(b0.vertexOffset == 100);    // vertexBase
    CHECK(b0.instanceCount == 0);
    CHECK(b0.firstInstance == 0);

    // Batch 1: firstIndex offset by the submesh, base advanced by mesh's object
    // count (1) — each batch reserves objectCount instance slots.
    const auto &b1 = tables.batchTemplates[1];
    CHECK(b1.firstIndex == 206);      // 200 + 6
    CHECK(b1.firstInstance == 1);
}

TEST_CASE("Finalize reserves per-mesh instance regions and advances bases across meshes")
{
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 2> a{GpuSubMesh{0, 6, 0, 0}, GpuSubMesh{6, 6, 0, 0}};
    const std::array<GpuSubMesh, 1> b{GpuSubMesh{0, 9, 0, 0}};
    const std::array<uint32_t, 1>   mat{0u};

    // Mesh A drawn by two objects (deduped), mesh B by one.
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(a, 0, 0), glm::mat4(1.f), mat);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(a, 0, 0), glm::mat4(1.f), mat);
    builder.AddInstanceRaw(&kMeshB, MakeGeometry(b, 50, 60), glm::mat4(1.f), mat);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    REQUIRE(tables.batchTemplates.size() == 3);
    // Mesh A (2 objects): submesh 0 base 0, submesh 1 base 2 (each reserves 2 slots).
    CHECK(tables.batchTemplates[0].firstInstance == 0);
    CHECK(tables.batchTemplates[1].firstInstance == 2);
    // Mesh B (1 object) follows mesh A's 4 reserved slots.
    CHECK(tables.batchTemplates[2].firstInstance == 4);
    CHECK(tables.batchTemplates[2].firstIndex == 60);
    CHECK(tables.batchTemplates[2].vertexOffset == 50);
    // Total reserved == drawCapacity (2×2 + 1×1).
    CHECK(tables.drawCapacity == 5);
}

TEST_CASE("An all-opaque frame keeps one command per batch and no masked half")
{
    // The pay-for-what-you-place case: with no cutout material placed, the command
    // table and the instance reservations must be exactly what they were before
    // masked draws existed.
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 2> submeshes{GpuSubMesh{0, 6, 0, 0}, GpuSubMesh{6, 6, 1, 0}};
    const std::array<uint32_t, 2>   materials{10u, 20u};
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), materials);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    CHECK_FALSE(tables.UsesPipeline(MeshPipeline::Mask));
    CHECK(tables.BatchCount() == 2);
    CHECK(tables.batchTemplates.size() == 2);
    CHECK(tables.CommandCount(MeshPipeline::Mask) == 0);
    CHECK(tables.CommandCount(MeshPipeline::Opaque) == 2);
}

TEST_CASE("A masked material adds a second command half the masked pipeline draws")
{
    // Two objects of one mesh, one opaque and one cutout. A batch is one draw and
    // a draw is one pipeline, so the (mesh, submesh) they share cannot be one
    // batch — the table grows a masked half for the second to land in.
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};
    const std::array<uint32_t, 1>   opaque{EncodeCullMaterial(7u, MeshPipeline::Opaque)};
    const std::array<uint32_t, 1>   masked{EncodeCullMaterial(8u, MeshPipeline::Mask)};

    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), opaque);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), masked);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    CHECK(tables.UsesPipeline(MeshPipeline::Mask));
    REQUIRE(tables.BatchCount() == 1);
    REQUIRE(tables.batchTemplates.size() == 2);
    CHECK(tables.CommandCount(MeshPipeline::Opaque) == 1);
    CHECK(tables.CommandCount(MeshPipeline::Mask) == 1);

    // Both halves draw the same geometry; only the pipeline and the instance
    // region they pack into differ.
    CHECK(tables.batchTemplates[0].indexCount == 3);
    CHECK(tables.batchTemplates[1].indexCount == 3);
    CHECK(tables.batchTemplates[0].firstIndex == tables.batchTemplates[1].firstIndex);

    // One object each, so the two reserved regions are one slot apiece and do not
    // overlap — an overlap would have the two pipelines writing the same record.
    CHECK(tables.batchTemplates[0].firstInstance == 0);
    CHECK(tables.batchTemplates[1].firstInstance == 1);
}

TEST_CASE("Reservations are exact, so a pipeline nothing uses reserves nothing")
{
    // Reserving one slot per object in both halves would double the instance
    // buffer for every scene that has a cutout material anywhere in it. The counts
    // are what each half will actually hold.
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};
    const std::array<uint32_t, 1>   masked{EncodeCullMaterial(4u, MeshPipeline::Mask)};
    const std::array<uint32_t, 1>   opaque{EncodeCullMaterial(5u, MeshPipeline::Opaque)};

    // Three masked objects of mesh A, one opaque object of mesh B.
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), masked);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), masked);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), masked);
    builder.AddInstanceRaw(&kMeshB, MakeGeometry(submeshes, 50, 60), glm::mat4(1.f), opaque);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    REQUIRE(tables.BatchCount() == 2);   // (A,s0) and (B,s0)
    REQUIRE(tables.batchTemplates.size() == 4);

    // Opaque half: A reserves nothing (all three of its objects are cutouts), B one.
    CHECK(tables.batchTemplates[0].firstInstance == 0);
    CHECK(tables.batchTemplates[1].firstInstance == 0);
    // Masked half follows B's single opaque slot: A takes three, B reserves none.
    CHECK(tables.batchTemplates[2].firstInstance == 1);
    CHECK(tables.batchTemplates[3].firstInstance == 4);

    // Every reservation fits inside the instance buffer drawCapacity sizes.
    CHECK(tables.drawCapacity == 4);
}

TEST_CASE("The masked bit never reaches the material id the shader indexes with")
{
    // The bit rides in the id's top bit, which the cull shader strips before
    // writing the instance record. Leaving it in would index a million rows past
    // the material table.
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};
    const std::array<uint32_t, 1>   masked{EncodeCullMaterial(9u, MeshPipeline::Mask)};
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), masked);

    // The tables carry it packed — stripping is the shader's job, and it needs the
    // bit to pick its half.
    CHECK(builder.Tables().objectMaterials[0] == EncodeCullMaterial(9u, MeshPipeline::Mask));
    CHECK((builder.Tables().objectMaterials[0] & Assisi::Render::kCullMaterialIdMask) == 9u);
    // The sentinel must keep reading as "no material" rather than as a masked one.
    CHECK((kNoMaterial &Assisi::Render::kCullMaterialIdMask) != 9u);
}

TEST_CASE("Every pipeline a frame places gets its own contiguous command block")
{
    // Three of the four pipelines in one frame. Blocks must follow one another in
    // pipeline order with no gap for the pipeline nobody used, or the offsets the
    // draw calls are issued at would skip past live commands.
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};
    const std::array<uint32_t, 1> opaque{EncodeCullMaterial(1u, MeshPipeline::Opaque)};
    const std::array<uint32_t, 1> mask{EncodeCullMaterial(2u, MeshPipeline::Mask)};
    const std::array<uint32_t, 1> maskBoth{EncodeCullMaterial(3u, MeshPipeline::MaskDoubleSided)};

    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), opaque);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), mask);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), maskBoth);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    REQUIRE(tables.BatchCount() == 1);
    CHECK(tables.UsesPipeline(MeshPipeline::Opaque));
    CHECK(tables.UsesPipeline(MeshPipeline::Mask));
    CHECK(tables.UsesPipeline(MeshPipeline::MaskDoubleSided));
    // Nobody placed a double-sided opaque material, so that pipeline costs nothing.
    CHECK_FALSE(tables.UsesPipeline(MeshPipeline::OpaqueDoubleSided));
    CHECK(tables.CommandCount(MeshPipeline::OpaqueDoubleSided) == 0);

    // Three live blocks of one batch each, packed with the dead one skipped.
    CHECK(tables.TotalCommandCount() == 3);
    REQUIRE(tables.batchTemplates.size() == 3);
    CHECK(tables.CommandBase(MeshPipeline::Opaque) == 0);
    CHECK(tables.CommandBase(MeshPipeline::OpaqueDoubleSided) == 1);
    CHECK(tables.CommandBase(MeshPipeline::Mask) == 1);
    CHECK(tables.CommandBase(MeshPipeline::MaskDoubleSided) == 2);

    // One instance each, so the three reserved regions are one slot apiece and do
    // not overlap.
    CHECK(tables.batchTemplates[0].firstInstance == 0);
    CHECK(tables.batchTemplates[1].firstInstance == 1);
    CHECK(tables.batchTemplates[2].firstInstance == 2);
    CHECK(tables.drawCapacity == 3);
}

TEST_CASE("A double-sided material is a different pipeline from the same material single-sided")
{
    // The cull mode splits batches exactly as the alpha mode does — one draw is
    // one pipeline either way.
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};
    const std::array<uint32_t, 1> single{EncodeCullMaterial(4u, MeshPipeline::Opaque)};
    const std::array<uint32_t, 1> both{EncodeCullMaterial(5u, MeshPipeline::OpaqueDoubleSided)};

    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), single);
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), both);
    builder.Finalize();

    const CullTables &tables = builder.Tables();
    CHECK(tables.TotalCommandCount() == 2);
    CHECK_FALSE(tables.UsesPipeline(MeshPipeline::Mask));
    // The packed id keeps the pipeline out of the material index the shader uses.
    CHECK((tables.objectMaterials[1] & Assisi::Render::kCullMaterialIdMask) == 5u);
}

TEST_CASE("Reset clears the tables and the mesh dedup map")
{
    CullTableBuilder builder;
    const std::array<GpuSubMesh, 1> submeshes{GpuSubMesh{0, 3, 0, 0}};
    const std::array<uint32_t, 1>   mat{1u};
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), mat);
    REQUIRE_FALSE(builder.Tables().Empty());

    builder.Reset();
    CHECK(builder.Tables().Empty());
    CHECK(builder.Tables().drawCapacity == 0);

    // After Reset the same key is treated as new again (fresh descriptor).
    builder.AddInstanceRaw(&kMeshA, MakeGeometry(submeshes, 0, 0), glm::mat4(1.f), mat);
    CHECK(builder.Tables().meshDescs.size() == 1);
    CHECK(builder.Tables().objects.size() == 1);
}
