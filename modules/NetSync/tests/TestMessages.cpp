/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestMessages.cpp
/// @brief Messages as reflected structs: the registry, the wire form, the hash,
/// and the generated handler binding.
///
/// The load-bearing claim of the whole design is that declaring a message is a
/// *versioning event* — add one, reorder its fields, or flip it from unreliable
/// to reliable, and two builds that disagree refuse to pair instead of
/// misparsing each other. That falls out of messages being reflected structs, so
/// it is checked here rather than assumed.
///
/// The other claim worth pinning is that nothing about which handler gets called
/// depends on name lookup. The test support declares two handlers with the same
/// name in different namespaces, taking different message types; if the bindings
/// resolved names anywhere but the declaration's own scope, one would silently
/// answer for the other.
///
/// See docs/replication-messaging-relevancy-plan-v1.md M3.

#include <doctest/doctest.h>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/MessageRegistry.hpp>
#include <Assisi/ECS/Scene.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>
#include <Assisi/NetSync/TestMessageHandlers.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Assisi;
using namespace Assisi::Core::Reflect;
using namespace Assisi::NetSync;
using namespace Assisi::NetSync::Test;

namespace
{

/// A message type the registry has never heard of, for the negative cases.
struct NotAMessage
{
    int32_t value = 0;
};

/// The layout text with one message's entry removed, standing in for a build
/// that never declared it. Comparing hashes over doctored descriptions is how
/// "declaring a message moves the hash" is checked without a second build.
std::string DescriptionWithout(std::string_view name)
{
    std::vector<MessageMeta> kept;
    for (const MessageMeta &meta : MessageRegistry::Instance().All())
    {
        if (meta.name != name)
            kept.push_back(meta);
    }
    return MessageLayoutDescription(kept);
}

} // namespace

TEST_CASE("every AMSG in the build is registered, with its grammar intact")
{
    const MessageRegistry &registry = MessageRegistry::Instance();

    const MessageMeta *marker = registry.Find("TestPlaceMarker");
    REQUIRE(marker != nullptr);
    CHECK(marker->direction == MessageDirection::Intent);
    CHECK(marker->reliability == MessageReliability::Reliable);
    CHECK_FALSE(marker->independent);
    CHECK(marker->fields.size() == 2);

    const MessageMeta *ping = registry.Find("TestPing");
    REQUIRE(ping != nullptr);
    CHECK(ping->direction == MessageDirection::Intent);
    CHECK(ping->reliability == MessageReliability::Unreliable);

    const MessageMeta *burst = registry.Find("TestBurst");
    REQUIRE(burst != nullptr);
    CHECK(burst->direction == MessageDirection::Event);
    CHECK(burst->reliability == MessageReliability::Unreliable);
    CHECK_FALSE(burst->independent);

    const MessageMeta *announce = registry.Find("TestAnnounce");
    REQUIRE(announce != nullptr);
    CHECK(announce->direction == MessageDirection::Event);
    CHECK(announce->reliability == MessageReliability::Reliable);
    // Names no entity, so relevancy has nothing to scope it by.
    CHECK(announce->independent);
}

TEST_CASE("message ids are dense, alphabetical, and never zero")
{
    const MessageRegistry &registry = MessageRegistry::Instance();
    REQUIRE(registry.Count() >= 4);

    // A raw counter rather than a MessageId, for the reason MessageId has no
    // arithmetic: what this walks is the sequence 1, 2, 3…, and the claim under
    // test is that the ids happen to be it.
    std::uint32_t expected = 1;
    std::string   previousName;
    for (const MessageMeta &meta : registry.All())
    {
        CHECK(meta.id == MessageId{expected});
        // Zero has to stay invalid so a value-initialized id claims nothing,
        // matching every other id in the engine.
        CHECK(meta.id != kInvalidMessageId);
        if (!previousName.empty())
            CHECK(previousName < meta.name);
        previousName = meta.name;
        ++expected;

        // The round trip both ways, which is what a dispatch depends on.
        CHECK(registry.ById(meta.id) == &meta);
        CHECK(registry.IdOf(meta.name) == meta.id);
    }

    CHECK(registry.ById(kInvalidMessageId) == nullptr);
    CHECK(registry.ById(MessageId{static_cast<std::uint32_t>(registry.Count() + 1)}) == nullptr);
    CHECK(registry.IdOf(typeid(NotAMessage)) == kInvalidMessageId);
}

TEST_CASE("a message round-trips through the binary codec")
{
    const TestPlaceMarker sent{/*target=*/4242, /*slot=*/-7};

    Core::BitWriter writer;
    REQUIRE(WriteMessageValue(sent, writer));

    Core::BitReader   reader(writer.Data());
    const MessageId   id   = ReadMessageId(reader);
    const MessageMeta *meta = MessageRegistry::Instance().ById(id);
    REQUIRE(meta != nullptr);
    CHECK(meta->name == "TestPlaceMarker");

    TestPlaceMarker received;
    REQUIRE(ReadMessage(*meta, &received, reader));
    CHECK(received.target == sent.target);
    CHECK(received.slot == sent.slot);
    CHECK(reader.Ok());
}

TEST_CASE("a message round-trips through JSON")
{
    const MessageMeta *meta = MessageRegistry::Instance().Find("TestBurst");
    REQUIRE(meta != nullptr);
    REQUIRE(meta->serialize != nullptr);
    REQUIRE(meta->deserialize != nullptr);

    const TestBurst sent{ECS::Entity{9, 1}, /*intensity=*/33};
    const nlohmann::json json = meta->serialize(&sent);

    TestBurst received;
    meta->deserialize(json, &received);
    CHECK(received.source == sent.source);
    CHECK(received.intensity == sent.intensity);
}

TEST_CASE("an unknown message id can be stepped over, not merely failed on")
{
    // Two messages back to back. A reader that does not recognise the first must
    // still find the second — which is the entire purpose of the length prefix.
    Core::BitWriter writer;
    REQUIRE(WriteMessageValue(TestPing{1.5f, -2.5f}, writer));
    REQUIRE(WriteMessageValue(TestAnnounce{/*round=*/12}, writer));

    Core::BitReader reader(writer.Data());
    (void)ReadMessageId(reader);       // pretend this build does not know it
    REQUIRE(SkipMessageBody(reader));

    const MessageId    secondId = ReadMessageId(reader);
    const MessageMeta *meta     = MessageRegistry::Instance().ById(secondId);
    REQUIRE(meta != nullptr);
    CHECK(meta->name == "TestAnnounce");

    TestAnnounce received;
    REQUIRE(ReadMessage(*meta, &received, reader));
    CHECK(received.round == 12);
}

TEST_CASE("a truncated message body fails rather than reading past the buffer")
{
    Core::BitWriter writer;
    REQUIRE(WriteMessageValue(TestPlaceMarker{7, 7}, writer));

    // Every prefix of a legitimate packet, which is what a cut connection or a
    // hostile sender produces. None of them may read outside the buffer, and
    // none may claim success.
    const std::span<const std::byte> whole = writer.Data();
    for (std::size_t length = 0; length < whole.size(); ++length)
    {
        Core::BitReader reader(whole.subspan(0, length));
        const MessageId id = ReadMessageId(reader);
        if (id == kInvalidMessageId)
            continue; // the id itself did not survive; nothing further to try

        const MessageMeta *meta = MessageRegistry::Instance().ById(id);
        if (meta == nullptr)
            continue;

        TestPlaceMarker received;
        const bool      ok = ReadMessage(*meta, &received, reader);
        CHECK_FALSE(ok); // a short buffer cannot yield a whole message
    }
}

TEST_CASE("a message body whose declared length lies is not trusted")
{
    // The length prefix is the authority on where the next message starts. A
    // sender claiming a body longer than the buffer must be refused outright,
    // not left to walk off the end.
    Core::BitWriter writer;
    writer.WriteVarUInt32(1);        // some message id
    writer.WriteVarUInt32(100000);   // ...and a body length far past the packet

    Core::BitReader reader(writer.Data());
    (void)ReadMessageId(reader);
    CHECK_FALSE(SkipMessageBody(reader));
    CHECK(reader.Failed());
}

TEST_CASE("declaring a message moves the protocol hash")
{
    // The versioning story, and the reason messages are reflected structs at
    // all. A build without TestBurst has a different message table, so it has a
    // different hash, so the two refuse to pair rather than misdispatching each
    // other's dense ids.
    const std::string whole   = MessageLayoutDescription(MessageRegistry::Instance().All());
    const std::string without = DescriptionWithout("TestBurst");

    CHECK(whole != without);
    CHECK(whole.find("TestBurst") != std::string::npos);
    CHECK(without.find("TestBurst") == std::string::npos);

    // ...and the whole-protocol hash is taken over a text that contains it.
    CHECK(ProtocolLayoutDescription().find("TestBurst") != std::string::npos);
}

TEST_CASE("direction and reliability are in the hash, not only the field layout")
{
    // Two builds that disagree about which way a message travels, or about
    // whether it must arrive, have identical field descriptions and completely
    // different behaviour. Reclassifying is a protocol change, and this is what
    // makes it one.
    std::vector<MessageMeta> flipped;
    for (const MessageMeta &meta : MessageRegistry::Instance().All())
        flipped.push_back(meta);

    const std::string original = MessageLayoutDescription(flipped);

    const auto burst = std::find_if(flipped.begin(), flipped.end(),
                                    [](const MessageMeta &meta) { return meta.name == "TestBurst"; });
    REQUIRE(burst != flipped.end());

    burst->reliability = MessageReliability::Reliable;
    CHECK(MessageLayoutDescription(flipped) != original);

    burst->reliability = MessageReliability::Unreliable;
    CHECK(MessageLayoutDescription(flipped) == original);

    burst->direction = MessageDirection::Intent;
    CHECK(MessageLayoutDescription(flipped) != original);
}

TEST_CASE("the generated table binds every handler, and binds each to its own type")
{
    // The zero-ambiguity check. Two handlers named HandlePlaceMarker exist in
    // two namespaces, taking two different message types; if either binding had
    // been resolved by a lookup outside the declaration's own scope, one message
    // would arrive at the other's handler.
    const MessageDispatch &dispatch = MessageDispatch::Instance();
    HandlerLog::Instance().Clear();

    ECS::Scene scene;
    NetContext context{ClientId{7}, nullptr, &scene};

    const auto deliver = [&](const auto &message)
    {
        Core::BitWriter writer;
        REQUIRE(WriteMessageValue(message, writer));
        Core::BitReader    reader(writer.Data());
        const MessageMeta *meta = MessageRegistry::Instance().ById(ReadMessageId(reader));
        REQUIRE(meta != nullptr);
        return dispatch.Dispatch(*meta, context, reader);
    };

    CHECK(deliver(TestPlaceMarker{/*target=*/11, /*slot=*/2}));
    CHECK(HandlerLog::Instance().placeMarkerCalls == 1);
    CHECK(HandlerLog::Instance().pingCalls == 0);
    CHECK(HandlerLog::Instance().lastPlaceMarker.target == 11);
    CHECK(HandlerLog::Instance().lastPlaceMarker.slot == 2);
    // The sender travels with the dispatch rather than with the payload — it is
    // the connection the bytes arrived on, which is the only version a handler
    // may trust.
    CHECK(HandlerLog::Instance().lastSender == ClientId{7});

    CHECK(deliver(TestPing{/*x=*/3.5f, /*y=*/-1.25f}));
    CHECK(HandlerLog::Instance().pingCalls == 1);
    CHECK(HandlerLog::Instance().placeMarkerCalls == 1); // the same-named handler did not fire
    CHECK(HandlerLog::Instance().lastPing.x == doctest::Approx(3.5f));
    CHECK(HandlerLog::Instance().lastPing.y == doctest::Approx(-1.25f));

    CHECK(deliver(TestBurst{ECS::NullEntity, /*intensity=*/9}));
    CHECK(HandlerLog::Instance().burstCalls == 1);

    CHECK(deliver(TestAnnounce{/*round=*/3}));
    CHECK(HandlerLog::Instance().announceCalls == 1);
    CHECK(HandlerLog::Instance().lastAnnounce.round == 3);
}

TEST_CASE("a message value fills in from nothing, not from the last one")
{
    // A message has no baseline to patch against — it is not a value with a
    // previous version — so every dispatch starts from a default-constructed
    // instance. Two deliveries in a row must not leak into each other.
    HandlerLog::Instance().Clear();

    ECS::Scene scene;
    NetContext context{ClientId{2}, nullptr, &scene};

    const auto deliver = [&](const auto &message)
    {
        Core::BitWriter writer;
        REQUIRE(WriteMessageValue(message, writer));
        Core::BitReader    reader(writer.Data());
        const MessageMeta *meta = MessageRegistry::Instance().ById(ReadMessageId(reader));
        REQUIRE(meta != nullptr);
        return MessageDispatch::Instance().Dispatch(*meta, context, reader);
    };

    CHECK(deliver(TestBurst{ECS::NullEntity, /*intensity=*/42}));
    CHECK(HandlerLog::Instance().lastBurst.intensity == 42);

    CHECK(deliver(TestBurst{}));
    CHECK(HandlerLog::Instance().lastBurst.intensity == 1); // the field's own default
}

TEST_CASE("an unhandled message is reported rather than silently swallowed")
{
    // Nothing in this build handles the message we are about to invent, and that
    // is a normal state: one side of the wire may simply not care. What must not
    // happen is a silent success.
    MessageMeta orphan{.name        = "Orphan",
                       .typeIndex   = typeid(NotAMessage),
                       .fields      = {},
                       .direction   = MessageDirection::Event,
                       .reliability = MessageReliability::Unreliable,
                       .independent = true,
                       .id          = 999,
                       .serialize   = nullptr,
                       .deserialize = nullptr};

    ECS::Scene scene;
    NetContext context{ClientId{1}, nullptr, &scene};

    Core::BitWriter writer;
    writer.WriteVarUInt32(0); // an empty body

    Core::BitReader reader(writer.Data());
    CHECK_FALSE(MessageDispatch::Instance().HasHandler(orphan));
    CHECK_FALSE(MessageDispatch::Instance().Dispatch(orphan, context, reader));
    // The body is left unread, so the caller can step over it and count the drop.
    CHECK(reader.BitsRead() == 0);
}
