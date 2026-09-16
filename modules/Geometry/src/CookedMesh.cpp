/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Geometry/CookedMesh.hpp>

#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Geometry/MeshValidate.hpp>

namespace Assisi::Geometry
{

namespace
{

constexpr std::size_t kBitsPerByte = 8;

/// The fewest bytes each record can take, used to refuse a count the remaining
/// bytes cannot hold before anything is allocated. A varint is at least one byte.
constexpr std::size_t kFloatBytes          = sizeof(float);
constexpr std::size_t kMinVarIntBytes      = 1;
constexpr std::size_t kVertexFloats        = 12; ///< Position 3, normal 3, UV 2, tangent 4.
constexpr std::size_t kVertexBytes         = kVertexFloats * kFloatBytes;
constexpr std::size_t kBoundsFloats        = 10; ///< Sphere centre 3 + radius 1, AABB min 3 + max 3.
constexpr std::size_t kSubMeshVarInts      = 3;  ///< Index offset, index count, material slot.
constexpr std::size_t kMinSubMeshBytes     = kSubMeshVarInts * kMinVarIntBytes + kBoundsFloats * kFloatBytes;
constexpr std::size_t kLodVarInts          = 2;  ///< First submesh, submesh count.
constexpr std::size_t kMinLodBytes         = kLodVarInts * kMinVarIntBytes + kFloatBytes;
constexpr std::size_t kAssetIdBytes        = sizeof(Core::AssetId::bytes);

void WriteVec3(Core::BitWriter &writer, const glm::vec3 &value)
{
    writer.WriteFloat(value.x);
    writer.WriteFloat(value.y);
    writer.WriteFloat(value.z);
}

glm::vec3 ReadVec3(Core::BitReader &reader)
{
    const float x = reader.ReadFloat();
    const float y = reader.ReadFloat();
    const float z = reader.ReadFloat();
    return {x, y, z};
}

void WriteBounds(Core::BitWriter &writer, const BoundingSphere &sphere, const Aabb &box)
{
    WriteVec3(writer, sphere.center);
    writer.WriteFloat(sphere.radius);
    WriteVec3(writer, box.min);
    WriteVec3(writer, box.max);
}

void ReadBounds(Core::BitReader &reader, BoundingSphere &sphere, Aabb &box)
{
    sphere.center = ReadVec3(reader);
    sphere.radius = reader.ReadFloat();
    box.min       = ReadVec3(reader);
    box.max       = ReadVec3(reader);
}

/// Reads a count and refuses it when @p recordBytes each would not fit in what
/// is left.
bool ReadCount(Core::BitReader &reader, std::size_t recordBytes, std::uint32_t &count)
{
    count = reader.ReadVarUInt32();
    if (reader.Failed())
    {
        return false;
    }
    const std::size_t bytesLeft = reader.BitsRemaining() / kBitsPerByte;
    return static_cast<std::size_t>(count) <= bytesLeft / recordBytes;
}

} // namespace

std::string_view ToString(CookedMeshError error) noexcept
{
    switch (error)
    {
    case CookedMeshError::NotAMesh:
        return "not a cooked mesh";
    case CookedMeshError::UnsupportedVersion:
        return "a mesh layout this build does not read";
    case CookedMeshError::Truncated:
        return "the mesh bytes end part-way through";
    case CookedMeshError::Invalid:
        return "the mesh's tables disagree with each other";
    }
    return "unknown";
}

void WriteCookedMesh(Core::BitWriter &writer, const MeshData &mesh, std::span<const Core::AssetId> slotMaterials)
{
    Core::WriteCookedHeader(writer, Core::CookedKind::Mesh);
    writer.WriteUInt8(kMeshPayloadVersion);

    writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh.Vertices.size()));
    for (const Vertex &vertex : mesh.Vertices)
    {
        WriteVec3(writer, vertex.Position);
        WriteVec3(writer, vertex.Normal);
        writer.WriteFloat(vertex.TextureCoordinates.x);
        writer.WriteFloat(vertex.TextureCoordinates.y);
        writer.WriteFloat(vertex.Tangent.x);
        writer.WriteFloat(vertex.Tangent.y);
        writer.WriteFloat(vertex.Tangent.z);
        writer.WriteFloat(vertex.Tangent.w);
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh.Indices.size()));
    for (const std::uint32_t index : mesh.Indices)
    {
        writer.WriteVarUInt32(index);
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh.SubMeshes.size()));
    for (const SubMesh &submesh : mesh.SubMeshes)
    {
        writer.WriteVarUInt32(submesh.IndexOffset);
        writer.WriteVarUInt32(submesh.IndexCount);
        writer.WriteVarUInt32(submesh.MaterialSlot);
        WriteBounds(writer, submesh.LocalBounds, submesh.LocalAabb);
    }

    writer.WriteVarUInt32(static_cast<std::uint32_t>(mesh.Lods.size()));
    for (const LodRange &lod : mesh.Lods)
    {
        writer.WriteVarUInt32(lod.FirstSubMesh);
        writer.WriteVarUInt32(lod.SubMeshCount);
        writer.WriteFloat(lod.ScreenSizeThreshold);
    }

    WriteBounds(writer, mesh.LocalBounds, mesh.LocalAabb);

    writer.WriteVarUInt32(static_cast<std::uint32_t>(slotMaterials.size()));
    for (const Core::AssetId &id : slotMaterials)
    {
        Core::WriteAssetId(writer, id);
    }
}

std::expected<CookedMesh, CookedMeshError> ReadCookedMesh(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::CookedKind, Core::CookedBlobError> kind = Core::ReadCookedHeader(reader);
    if (!kind || *kind != Core::CookedKind::Mesh)
    {
        if (!kind && kind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedMeshError::Truncated);
        }
        return std::unexpected(CookedMeshError::NotAMesh);
    }

    const std::uint8_t version = reader.ReadUInt8();
    if (reader.Failed())
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    if (version != kMeshPayloadVersion)
    {
        return std::unexpected(CookedMeshError::UnsupportedVersion);
    }

    CookedMesh cooked;
    MeshData &mesh = cooked.mesh;

    std::uint32_t count = 0;
    if (!ReadCount(reader, kVertexBytes, count))
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    mesh.Vertices.resize(count);
    for (Vertex &vertex : mesh.Vertices)
    {
        vertex.Position             = ReadVec3(reader);
        vertex.Normal               = ReadVec3(reader);
        vertex.TextureCoordinates.x = reader.ReadFloat();
        vertex.TextureCoordinates.y = reader.ReadFloat();
        vertex.Tangent.x            = reader.ReadFloat();
        vertex.Tangent.y            = reader.ReadFloat();
        vertex.Tangent.z            = reader.ReadFloat();
        vertex.Tangent.w            = reader.ReadFloat();
    }

    if (reader.Failed() || !ReadCount(reader, kMinVarIntBytes, count))
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    mesh.Indices.resize(count);
    for (std::uint32_t &index : mesh.Indices)
    {
        index = reader.ReadVarUInt32();
    }

    if (reader.Failed() || !ReadCount(reader, kMinSubMeshBytes, count))
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    mesh.SubMeshes.resize(count);
    for (SubMesh &submesh : mesh.SubMeshes)
    {
        submesh.IndexOffset  = reader.ReadVarUInt32();
        submesh.IndexCount   = reader.ReadVarUInt32();
        submesh.MaterialSlot = reader.ReadVarUInt32();
        ReadBounds(reader, submesh.LocalBounds, submesh.LocalAabb);
    }

    if (reader.Failed() || !ReadCount(reader, kMinLodBytes, count))
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    mesh.Lods.resize(count);
    for (LodRange &lod : mesh.Lods)
    {
        lod.FirstSubMesh        = reader.ReadVarUInt32();
        lod.SubMeshCount        = reader.ReadVarUInt32();
        lod.ScreenSizeThreshold = reader.ReadFloat();
    }

    ReadBounds(reader, mesh.LocalBounds, mesh.LocalAabb);
    mesh.BoundsComputed = true;

    if (reader.Failed() || !ReadCount(reader, kAssetIdBytes, count))
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    cooked.slotMaterials.resize(count);
    for (Core::AssetId &id : cooked.slotMaterials)
    {
        id = Core::ReadAssetId(reader);
    }
    if (reader.Failed())
    {
        return std::unexpected(CookedMeshError::Truncated);
    }
    mesh.Materials.resize(count);

    if (!ValidateMesh(mesh))
    {
        return std::unexpected(CookedMeshError::Invalid);
    }
    return cooked;
}

} // namespace Assisi::Geometry
