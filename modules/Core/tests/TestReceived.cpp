/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <doctest/doctest.h>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Received.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/DecodedMessage.hpp>
#include <Assisi/Core/Reflect/MessageRegistry.hpp>
#include <Assisi/Testing/TestMessages.hpp>

#include <cstddef>
#include <type_traits>
#include <vector>

using namespace Assisi::Core;
using Assisi::Core::Test::TestRingBell;

// Only the engine makes one, so what a system reads came through the network's
// checks, from the sender the connection names.
static_assert(!std::is_constructible_v<Received<TestRingBell>, ClientId, TestRingBell>);
static_assert(!std::is_default_constructible_v<Received<TestRingBell>>);
static_assert(!std::is_aggregate_v<Received<TestRingBell>>);

static_assert(Reflect::DeclaredMessage<TestRingBell>);
static_assert(!Reflect::DeclaredMessage<int>);

namespace
{

/// @p ring as the wire carries it, with the reader past its id.
struct OnTheWire
{
    std::vector<std::byte> bytes;
    const Reflect::MessageMeta *meta = nullptr;
};

OnTheWire Encode(const TestRingBell &ring)
{
    const Reflect::MessageRegistry &registry = Reflect::MessageRegistry::Instance();
    const Reflect::MessageMeta *meta = registry.ById(registry.IdOf(typeid(TestRingBell)));
    REQUIRE(meta != nullptr);
    BitWriter writer;
    REQUIRE(Reflect::WriteMessage(*meta, &ring, writer));
    return OnTheWire{std::vector<std::byte>(writer.Data().begin(), writer.Data().end()), meta};
}

} // namespace

TEST_CASE("a received message reaches its world's events with who sent it")
{
    const OnTheWire wire = Encode(TestRingBell{ /*bell=*/ 3, /*loudness=*/ 7});
    BitReader reader(wire.bytes);
    REQUIRE(Reflect::ReadMessageId(reader) == wire.meta->id);

    const Reflect::DecodedMessage decoded = wire.meta->decode(*wire.meta, reader, nullptr);
    REQUIRE(decoded);
    CHECK(static_cast<const TestRingBell *>(decoded.Value())->bell == 3);

    EventQueue queue;
    decoded.Deliver(queue, ClientId{5});

    int count = 0;
    for (const auto &[sender, ring] : queue.Read<Received<TestRingBell>>())
    {
        CHECK(sender == ClientId{5});
        CHECK(ring.bell == 3);
        CHECK(ring.loudness == 7);
        ++count;
    }
    CHECK(count == 1);
}

TEST_CASE("a message whose body cannot be read decodes to nothing")
{
    OnTheWire wire = Encode(TestRingBell{ /*bell=*/ 3, /*loudness=*/ 7});
    wire.bytes.resize(wire.bytes.size() / 2);
    BitReader reader(wire.bytes);
    (void)Reflect::ReadMessageId(reader);

    CHECK_FALSE(wire.meta->decode(*wire.meta, reader, nullptr));
}
