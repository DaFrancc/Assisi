/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestMessageHandlers.hpp
/// @brief Handler declarations — and, for a test-support header, their bodies.
///
/// The bodies are `inline` here rather than in a .cpp because the generated
/// bindings for a header land in that header's reflection OBJECT library, and
/// every executable in the tree links every such library. A binding that
/// referenced a symbol living in a static library only the tests link would fail
/// to resolve in the sandbox. Inline definitions make the handlers travel with
/// their bindings, which is the arrangement a test-support header wants anyway.
///
/// Two of the handlers are deliberately named the same thing in two different
/// namespaces and take two different message types. Nothing about which one gets
/// bound to which message may depend on lookup, so the pair is the compile-level
/// half of the zero-ambiguity requirement: it is checked by the fact that this
/// builds at all, and by the runtime test that each message reaches its own
/// handler rather than the other's.

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/NetSync/MessageDispatch.hpp>
#include <Assisi/NetSync/TestNetComponents.hpp>

#include <Assisi/ECS/Entity.hpp>

#include <cstdint>

namespace Assisi::NetSync::Test
{

/// What the handlers below record, so a test can ask what actually arrived.
struct HandlerLog
{
    std::uint32_t placeMarkerCalls = 0;
    std::uint32_t pingCalls        = 0;
    std::uint32_t burstCalls       = 0;
    std::uint32_t announceCalls    = 0;

    /// The last payload each handler saw, so a round trip is checked all the way
    /// through dispatch rather than only through the codec.
    std::uint32_t movePawnCalls = 0;

    std::uint32_t tagInstanceCalls   = 0;
    std::uint32_t instanceNamedCalls = 0;
    std::uint32_t knockbackCalls     = 0;
    std::uint32_t reliableHitCalls   = 0;

    TestPlaceMarker   lastPlaceMarker;
    TestPing          lastPing;
    TestBurst         lastBurst;
    TestKnockback     lastKnockback;
    TestReliableHit   lastReliableHit;
    TestAnnounce      lastAnnounce;
    TestMovePawn      lastMovePawn;
    TestTagInstance   lastTagInstance;
    TestInstanceNamed lastInstanceNamed;

    /// The server dispatches its own copy of every event it sends, through the
    /// same codec pair the wire uses. The two copies are kept apart so a test can
    /// say which side it is asking about — they translate through different id
    /// spaces and are not expected to agree.
    TestInstanceNamed lastInstanceNamedOnHost;
    TestInstanceNamed lastInstanceNamedOnClient;

    /// Who the dispatch site said sent the last intent.
    ClientId lastSender;

    static HandlerLog &Instance()
    {
        static HandlerLog log;
        return log;
    }

    void Clear() { *this = HandlerLog{}; }
};

AMSG_HANDLER() void HandlePlaceMarker(NetContext &ctx, const TestPlaceMarker &msg);
AMSG_HANDLER() void HandleTestBurst(NetContext &ctx, const TestBurst &msg);
AMSG_HANDLER() void HandleTestKnockback(NetContext &ctx, const TestKnockback &msg);
AMSG_HANDLER() void HandleTestReliableHit(NetContext &ctx, const TestReliableHit &msg);
AMSG_HANDLER() void HandleTestAnnounce(NetContext &ctx, const TestAnnounce &msg);
AMSG_HANDLER() void HandleMovePawn(NetContext &ctx, const TestMovePawn &msg);
AMSG_HANDLER() void HandleTagInstance(NetContext &ctx, const TestTagInstance &msg);
AMSG_HANDLER() void HandleInstanceNamed(NetContext &ctx, const TestInstanceNamed &msg);

inline void HandlePlaceMarker(NetContext &ctx, const TestPlaceMarker &msg)
{
    HandlerLog &log = HandlerLog::Instance();
    ++log.placeMarkerCalls;
    log.lastPlaceMarker = msg;
    log.lastSender      = ctx.sender;
}

inline void HandleTestBurst(NetContext &ctx, const TestBurst &msg)
{
    (void)ctx;
    HandlerLog &log = HandlerLog::Instance();
    ++log.burstCalls;
    log.lastBurst = msg;
}

inline void HandleTestKnockback(NetContext &ctx, const TestKnockback &msg)
{
    (void)ctx;
    HandlerLog &log = HandlerLog::Instance();
    ++log.knockbackCalls;
    log.lastKnockback = msg;
}

inline void HandleTestReliableHit(NetContext &ctx, const TestReliableHit &msg)
{
    (void)ctx;
    HandlerLog &log = HandlerLog::Instance();
    ++log.reliableHitCalls;
    log.lastReliableHit = msg;
}

inline void HandleTestAnnounce(NetContext &ctx, const TestAnnounce &msg)
{
    (void)ctx;
    HandlerLog &log = HandlerLog::Instance();
    ++log.announceCalls;
    log.lastAnnounce = msg;
}

inline void HandleMovePawn(NetContext &ctx, const TestMovePawn &msg)
{
    HandlerLog &log = HandlerLog::Instance();
    ++log.movePawnCalls;
    log.lastMovePawn = msg;
    log.lastSender   = ctx.sender;
}

inline void HandleTagInstance(NetContext &ctx, const TestTagInstance &msg)
{
    HandlerLog &log = HandlerLog::Instance();
    ++log.tagInstanceCalls;
    log.lastTagInstance = msg;
    log.lastSender      = ctx.sender;
}

inline void HandleInstanceNamed(NetContext &ctx, const TestInstanceNamed &msg)
{
    HandlerLog &log = HandlerLog::Instance();
    ++log.instanceNamedCalls;
    log.lastInstanceNamed = msg;
    if (ctx.sender == HostClientId)
        log.lastInstanceNamedOnHost = msg;
    else
        log.lastInstanceNamedOnClient = msg;
}

/// A second namespace declaring a *same-named* handler for a different message.
/// If the generated bindings resolved names by a lookup somewhere other than the
/// declaration's own scope, one of these two would silently bind to the other's
/// type.
namespace Elsewhere
{

AMSG_HANDLER() void HandlePlaceMarker(NetContext &ctx, const TestPing &msg);

inline void HandlePlaceMarker(NetContext &ctx, const TestPing &msg)
{
    HandlerLog &log = HandlerLog::Instance();
    ++log.pingCalls;
    log.lastPing   = msg;
    log.lastSender = ctx.sender;
}

} // namespace Elsewhere

} // namespace Assisi::NetSync::Test
