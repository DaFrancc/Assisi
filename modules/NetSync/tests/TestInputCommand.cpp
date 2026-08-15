/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

/// @file TestInputCommand.cpp
/// @brief Round trip, untrusted-input clamping, redundant send, and the
/// server-side jitter queue.

#include <doctest/doctest.h>

#include <Assisi/Core/BitStream.hpp>
#include <Assisi/NetSync/InputCommand.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

using namespace Assisi;
using namespace Assisi::NetSync;

namespace
{

InputCommand MakeCommand(std::uint64_t tick)
{
    InputCommand command;
    command.tick            = tick;
    command.actionBits      = 0b1011u;
    command.moveX           = 0.5f;
    command.moveY           = -0.25f;
    command.lookYaw         = 0.125f;
    command.lookPitch       = -0.0625f;
    command.subTickFraction = 200;
    return command;
}

bool Same(const InputCommand &a, const InputCommand &b)
{
    return a.tick == b.tick && a.actionBits == b.actionBits && a.moveX == b.moveX && a.moveY == b.moveY &&
           a.lookYaw == b.lookYaw && a.lookPitch == b.lookPitch && a.subTickFraction == b.subTickFraction;
}

} // namespace

TEST_CASE("InputCommand round-trips every field, including the reserved sub-tick byte")
{
    const InputCommand original = MakeCommand(1234567);

    Core::BitWriter writer;
    WriteInputCommand(original, writer);

    Core::BitReader reader(writer.Data());
    const InputCommand decoded = ReadInputCommand(reader);

    REQUIRE(reader.Ok());
    CHECK(Same(original, decoded));
    // v1 servers ignore it, but it must survive the wire so turning it on later
    // is a server-side change and not a format break.
    CHECK(decoded.subTickFraction == 200);
}

TEST_CASE("a truncated command fails the reader instead of returning garbage")
{
    Core::BitWriter writer;
    WriteInputCommand(MakeCommand(42), writer);

    const std::span<const std::byte> full = writer.Data();
    for (std::size_t length = 0; length < full.size(); ++length)
    {
        Core::BitReader reader(full.subspan(0, length));
        (void)ReadInputCommand(reader);
        CHECK_MESSAGE(reader.Failed(), "truncation to ", length, " bytes should have failed the reader");
    }
}

TEST_CASE("ClampInputCommand bounds a hostile command")
{
    InputLimits limits;

    SUBCASE("an honest command passes through untouched")
    {
        InputCommand command = MakeCommand(1);
        const InputCommand before = command;
        CHECK(ClampInputCommand(command, limits));
        CHECK(Same(before, command));
    }

    SUBCASE("diagonal movement is clamped by magnitude, not per axis")
    {
        // The classic speed hack: 1.0 on both axes is within a per-axis clamp
        // but travels sqrt(2) times as fast as intended.
        InputCommand command;
        command.moveX = 1.f;
        command.moveY = 1.f;
        CHECK_FALSE(ClampInputCommand(command, limits));

        const float magnitude = std::sqrt(command.moveX * command.moveX + command.moveY * command.moveY);
        CHECK(magnitude == doctest::Approx(limits.maxMoveMagnitude));
        // Direction preserved — clamping must not steer the player.
        CHECK(command.moveX == doctest::Approx(command.moveY));
    }

    SUBCASE("look deltas are bounded in both directions")
    {
        InputCommand command;
        command.lookYaw   = 100.f;
        command.lookPitch = -100.f;
        CHECK_FALSE(ClampInputCommand(command, limits));
        CHECK(command.lookYaw == doctest::Approx(limits.maxLookYaw));
        CHECK(command.lookPitch == doctest::Approx(-limits.maxLookPitch));
    }

    SUBCASE("non-finite values become zero rather than slipping past comparisons")
    {
        InputCommand command;
        command.moveX     = std::numeric_limits<float>::quiet_NaN();
        command.lookYaw   = std::numeric_limits<float>::infinity();
        command.lookPitch = -std::numeric_limits<float>::infinity();
        CHECK_FALSE(ClampInputCommand(command, limits));
        CHECK(command.moveX == 0.f);
        CHECK(command.lookYaw == 0.f);
        CHECK(command.lookPitch == 0.f);
    }
}

TEST_CASE("InputCommandBuffer repeats the last N commands in every packet")
{
    InputCommandBuffer buffer(3);
    CHECK(buffer.Empty());
    CHECK(buffer.Latest() == nullptr);

    for (std::uint64_t tick = 1; tick <= 5; ++tick)
        buffer.Push(MakeCommand(tick));

    CHECK(buffer.Size() == 3);
    REQUIRE(buffer.Latest() != nullptr);
    CHECK(buffer.Latest()->tick == 5);

    Core::BitWriter writer;
    buffer.WritePacket(writer);

    std::vector<InputCommand> received;
    Core::BitReader reader(writer.Data());
    REQUIRE(InputCommandBuffer::ReadPacket(reader, received));
    REQUIRE(received.size() == 3);
    // Oldest first: the receiver feeds them to the queue in tick order.
    CHECK(received[0].tick == 3);
    CHECK(received[2].tick == 5);
}

TEST_CASE("an empty buffer still writes a parseable packet")
{
    const InputCommandBuffer buffer;
    Core::BitWriter writer;
    buffer.WritePacket(writer);

    std::vector<InputCommand> received;
    Core::BitReader reader(writer.Data());
    CHECK(InputCommandBuffer::ReadPacket(reader, received));
    CHECK(received.empty());
}

TEST_CASE("a packet claiming an absurd command count is rejected without allocating for it")
{
    Core::BitWriter writer;
    writer.WriteVarUInt32(1000000u); // far past the per-packet cap

    std::vector<InputCommand> received;
    Core::BitReader reader(writer.Data());
    CHECK_FALSE(InputCommandBuffer::ReadPacket(reader, received));
    CHECK(received.empty());
    // The reader must latch failure so a caller cannot keep parsing past it.
    CHECK(reader.Failed());
}

TEST_CASE("a truncated packet leaves the output vector untouched")
{
    InputCommandBuffer buffer(3);
    for (std::uint64_t tick = 1; tick <= 3; ++tick)
        buffer.Push(MakeCommand(tick));

    Core::BitWriter writer;
    buffer.WritePacket(writer);

    // Keep the count but cut the commands short.
    std::vector<InputCommand> received;
    Core::BitReader reader(writer.Data().subspan(0, 6));
    CHECK_FALSE(InputCommandBuffer::ReadPacket(reader, received));
    CHECK(received.empty());
}

TEST_CASE("InputCommandQueue absorbs jitter and discards the redundant repeats")
{
    InputCommandQueue queue;

    CHECK(queue.Accept(MakeCommand(10)));
    CHECK(queue.Accept(MakeCommand(11)));
    // The same commands arrive again in the next packet's redundancy window.
    CHECK_FALSE(queue.Accept(MakeCommand(10)));
    CHECK_FALSE(queue.Accept(MakeCommand(11)));
    CHECK(queue.Depth() == 2);

    const InputCommand *applied = queue.Consume(10);
    REQUIRE(applied != nullptr);
    CHECK(applied->tick == 10);
    CHECK(queue.LastAppliedTick() == 10);
    CHECK(queue.Depth() == 1);

    // A repeat of an already-simulated tick must never be applied twice.
    CHECK_FALSE(queue.Accept(MakeCommand(10)));

    REQUIRE(queue.Consume(11) != nullptr);
    CHECK(queue.Depth() == 0);
}

TEST_CASE("InputCommandQueue reports starvation and drops commands that arrive too late")
{
    InputCommandQueue queue;

    CHECK(queue.Consume(5) == nullptr);
    CHECK(queue.StarvedTicks() == 1);

    // Arrives after tick 5 was already simulated: too late to be useful.
    CHECK(queue.Accept(MakeCommand(4)));
    CHECK(queue.Accept(MakeCommand(7)));
    CHECK(queue.Consume(6) == nullptr);  // nothing for 6; 4 is dropped on the way past
    CHECK(queue.StarvedTicks() == 2);
    CHECK(queue.Depth() == 1);

    REQUIRE(queue.Consume(7) != nullptr);
    CHECK(queue.StarvedTicks() == 2);
}

TEST_CASE("InputCommandQueue refuses a command flood far in the future")
{
    InputCommandQueue queue;

    CHECK(queue.Accept(MakeCommand(100)));
    REQUIRE(queue.Consume(100) != nullptr);

    // A client cannot buy itself unlimited queue depth by claiming distant ticks.
    CHECK_FALSE(queue.Accept(MakeCommand(100 + 10000)));
    CHECK(queue.Accept(MakeCommand(105)));
    CHECK(queue.Depth() == 1);
}

TEST_CASE("InputCommandQueue refuses a command flood before anything has been applied" *
          doctest::should_fail())
{
    // ENG-117, open. InputCommand.cpp:164 computes the lookahead horizon as
    //   horizon = _hasApplied ? _lastAppliedTick : command.tick
    // so on a fresh connection every command is measured against *itself* and
    // `command.tick > horizon + kMaxQueueLookahead` can never be true. The case
    // above only proves the guard works once something has been consumed, which
    // is what hid this: a client that floods before the server's first Consume
    // buys unlimited queue depth, and the first packet also fixes the queue's
    // tick base at whatever it claims.
    //
    // Note this is not merely "there is no reference point yet" — after the
    // first Accept there plainly is one, and the second command below is still
    // not checked against it.
    //
    // should_fail until the horizon is fixed; the fix removes this decorator.
    InputCommandQueue queue;

    CHECK(queue.Accept(MakeCommand(100)));

    // Same rejection the applied-tick case gets, and against the same base: the
    // queue already holds tick 100, so tick 10100 is far past any lookahead a
    // legitimate client could need.
    CHECK_FALSE(queue.Accept(MakeCommand(100 + 10000)));
    CHECK(queue.Depth() == 1);
}

TEST_CASE("a cleared queue behaves like a fresh one")
{
    InputCommandQueue queue;
    CHECK(queue.Accept(MakeCommand(3)));
    REQUIRE(queue.Consume(3) != nullptr);
    CHECK(queue.LastAppliedTick() == 3);

    queue.Clear();
    CHECK(queue.Depth() == 0);
    CHECK(queue.LastAppliedTick() == 0);
    CHECK(queue.StarvedTicks() == 0);
    // Ticks below the old high-water mark are accepted again — this is the
    // rejoin path, where the session's tick history is gone.
    CHECK(queue.Accept(MakeCommand(1)));
}
