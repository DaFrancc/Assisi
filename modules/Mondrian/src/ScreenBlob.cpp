/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Mondrian/ScreenBlob.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/CookedBlob.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/StructMeta.hpp>

#include <array>
#include <cstddef>

ASSISI_REFLECTED_STRUCT(Assisi::Mondrian::ScreenDocument);

namespace Assisi::Mondrian
{
namespace
{

/// Bits in the layout hash that follows the version.
constexpr uint32_t kLayoutHashBits = 64;

static_assert(kMaxScreenNodes <= Core::Reflect::kMaxVectorElements,
              "a screen the cook allows must be one the codec reads");

const Core::Reflect::StructSpec &DocumentSpec()
{
    const Core::Reflect::StructSpec *spec = Core::Reflect::StructTraits<ScreenDocument>::Spec();
    ASSISI_ASSERT(spec != nullptr, "ScreenDocument's generated table is missing");
    return *spec;
}

/// Whether @p handle names bytes inside @p pool. A handle outside it would read
/// as empty, which hides a blob that disagrees with itself as a blank label.
bool InPool(const Core::StringPool &pool, Core::PooledString handle)
{
    return static_cast<std::size_t>(handle.offset) + handle.length <= pool.Size();
}

/// Whether the table describes a tree this build can walk in one pass.
bool IsConsistent(const ScreenDocument &document)
{
    if (document.nodes.empty())
    {
        return false; // every screen has a root
    }
    if (document.focus != kNoNode && document.focus >= document.nodes.size())
    {
        return false;
    }

    for (std::size_t index = 0; index < document.nodes.size(); ++index)
    {
        const ScreenNode &node = document.nodes[index];

        // Preorder with the parent first is what lets the loader build the tree
        // in one pass. A parent at or past its own child is also how a cycle
        // would arrive, so this is the check that makes the walk terminate.
        const bool rootParent = index == 0 && node.parent == kNoNode;
        const bool childParent = index > 0 && node.parent < index;
        if (!rootParent && !childParent)
        {
            return false;
        }

        // The loader indexes its table of built ids by this, so one past the
        // end is a read out of bounds. Whether the node there can be acted on
        // is the loader's question, asked with the verb in hand.
        if (node.target != kNoNode && node.target >= document.nodes.size())
        {
            return false;
        }

        // An event action with no name is a button that would resolve to
        // nothing and silently do nothing.
        if (node.action == ActionKind::Event && node.eventName.View().empty())
        {
            return false;
        }

        const std::array texts{node.text, node.placeholder, node.pattern};
        for (const Core::PooledString &text : texts)
        {
            if (!InPool(document.pool, text))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

uint64_t ScreenLayoutHash()
{
    return Core::Reflect::StructLayoutHash(DocumentSpec());
}

std::string_view ToString(CookedScreenError error) noexcept
{
    switch (error)
    {
    case CookedScreenError::NotAScreen:
        return "not a cooked screen";
    case CookedScreenError::Truncated:
        return "ended part-way through";
    case CookedScreenError::UnsupportedVersion:
        return "a screen layout this build does not read";
    case CookedScreenError::Invalid:
        return "describes no usable screen";
    case CookedScreenError::Count:
        break;
    }
    return "unknown";
}

void WriteCookedScreen(Core::BitWriter &writer, const ScreenDocument &document)
{
    Core::WriteCookedHeader(writer, Core::kScreenKind);
    writer.WriteUInt8(kScreenPayloadVersion);
    writer.WriteBits64(ScreenLayoutHash(), kLayoutHashBits);
    (void)Core::Reflect::WriteStruct(DocumentSpec(), &document, writer);
}

std::expected<ScreenDocument, CookedScreenError> ReadCookedScreen(std::span<const std::byte> bytes)
{
    Core::BitReader reader{bytes};

    const std::expected<Core::AssetKindId, Core::CookedBlobError> blobKind = Core::ReadCookedHeader(reader);
    if (!blobKind || *blobKind != Core::kScreenKind)
    {
        if (!blobKind && blobKind.error() == Core::CookedBlobError::Truncated)
        {
            return std::unexpected(CookedScreenError::Truncated);
        }
        return std::unexpected(CookedScreenError::NotAScreen);
    }

    const uint8_t version = reader.ReadUInt8();
    const uint64_t layout = reader.ReadBits64(kLayoutHashBits);
    if (reader.Failed())
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    // Either one differing means the fields that follow are not the ones this
    // build would read them into.
    if (version != kScreenPayloadVersion || layout != ScreenLayoutHash())
    {
        return std::unexpected(CookedScreenError::UnsupportedVersion);
    }

    ScreenDocument document;
    const bool read = Core::Reflect::ReadStruct(DocumentSpec(), &document, reader);

    // Framing first: a document read out of bytes that ran out holds whatever
    // zeroes the reader handed back, and calling that inconsistent would name
    // the wrong fault.
    if (reader.Failed())
    {
        return std::unexpected(CookedScreenError::Truncated);
    }
    if (!read || !IsConsistent(document))
    {
        return std::unexpected(CookedScreenError::Invalid);
    }
    return document;
}

} // namespace Assisi::Mondrian
