/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file SessionTypes.hpp
/// @brief What a game says to `ctx.net` about sessions, and the events it hears back.
///
/// A session has an identity (a random id and a name), a visibility (listed in
/// a browse or hidden), an access rule (open, a password, or a code), a most
/// players and a level. Visibility and access are independent: a listed session
/// can need a code, and a hidden one can be open.
///
/// @code
/// ctx.net.Host({.name = "Lodge", .visibility = Visibility::Hidden, .access = Access::Code});
/// for (const SessionStarted &started : ctx.events.Read<SessionStarted>())
///     showCode(ctx.net.Session()->code);
/// @endcode

#include <Assisi/Core/Reflect/Annotations.hpp>
#include <Assisi/Core/SessionId.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Assisi::App
{

/// @brief The port a session listens on unless told otherwise, and the one a
/// joiner tries when its address names none.
inline constexpr std::uint16_t kDefaultSessionPort = 27015;

/// @brief The most players a session takes unless told otherwise.
inline constexpr std::uint32_t kDefaultMaxPlayers = 8;

/// @brief Key/value strings the host sets, such as `mode=coop`, carried to every
/// browse entry. The engine never reads them.
using GameData = std::vector<std::pair<std::string, std::string>>;

/// @brief Whether a session shows up when players browse.
enum class Visibility : std::uint8_t
{
    Listed, ///< Answers a browse.
    Hidden, ///< Joined only by address, with its code or password if it has one.
    Count_
};

/// @brief Who a session lets in.
enum class Access : std::uint8_t
{
    Open,     ///< Anyone who finds it.
    Password, ///< Whoever types the password the host chose.
    Code,     ///< Whoever types the short code made when it was hosted.
    Count_
};

/// @brief How to host. Only the name usually needs setting.
///
/// In the order the fields are usually written, so designated initializers read
/// as the decision does, rather than largest first. Every field has a default,
/// so any of them can be left out.
struct HostOptions
{
    std::string name{};                       ///< Up to 64 bytes. Shown in a browse.
    Visibility visibility = Visibility::Listed;
    Access access = Access::Open;
    std::string password{};                   ///< Read only with Access::Password. Up to 64 bytes.
    std::uint32_t maxPlayers = kDefaultMaxPlayers; ///< The host counts, when it plays. 0 for no limit.
    std::uint16_t port = 0;                   ///< 0 is the first free one from 27015.
    GameData gameData{};                      ///< Up to 16 pairs, keys 32 bytes, values 128.
};

/// @brief One session a browse found.
struct SessionEntry
{
    Core::SessionId id{};
    std::string name{};
    std::string address{}; ///< Where it answered from, which is where a join goes.
    std::string level{};   ///< The level it is running.
    GameData gameData{};
    std::int32_t pingMs = 0;     ///< From the search's last query to its answer.
    std::uint32_t players = 0;   ///< Including the host, when it plays.
    std::uint32_t maxPlayers = 0;
    std::uint16_t port = 0;
    bool needsSecret = false;    ///< Ask the player for a password or code before joining.
    bool compatible = true;      ///< Runs this build's protocol; one that does not would be refused.
};

/// @brief The session this machine is in, as `ctx.net.Session()` gives it.
struct SessionInfo
{
    /// The host's; on a client, the browse entry's it joined, or invalid when
    /// it joined by address.
    Core::SessionId id{};
    std::string name{};
    /// The code a host shows its friends, with Access::Code. A plain string:
    /// showing it as `K7M-Q4X` is the game's choice. Empty otherwise.
    std::string code{};
    std::uint32_t players = 0;    ///< Host only: everyone in, the host too when it plays.
    std::uint32_t maxPlayers = 0; ///< Host only.
    std::uint16_t port = 0;       ///< Host only: where it listens, for a game that shows its address.
    Visibility visibility = Visibility::Listed;
    Access access = Access::Open;
    bool hosting = false;         ///< True on the host, false on a client.
};

/// @brief What a search looks for. Empty for now: a backend's filters, such as
/// open places or game data, are added here without changing the call.
struct SearchParams
{
};

/// @brief Why hosting failed.
enum class HostFailure : std::uint8_t
{
    PortUnavailable,            ///< The port was taken, or no port from 27015 was free.
    NoLevel,                    ///< There is no saved level to host; a joiner loads it from disk.
    TooMuchData,                ///< The name or game data is over its limits.
    BadPassword,                ///< Access::Password with no password, or one over 64 bytes.
    AlreadyInSession,           ///< This machine is already hosting or joined; Leave first.
    DirectConnectionsForbidden, ///< `allowDirectConnections` is off in `config/network.json`.
    Count_
};

/// @brief Why a join was refused.
enum class JoinRefusal : std::uint8_t
{
    WrongSecret,                ///< The password or code was wrong, or missing.
    Full,                       ///< The session has its most players.
    TooManyTries,               ///< Too many wrong secrets from this address; wait and try again.
    Version,                    ///< The host runs a different build.
    Content,                    ///< The host has different level or blueprint files.
    Level,                      ///< This machine could not load the host's level.
    NotFound,                   ///< The browse entry is gone, or names no session.
    Unreachable,                ///< Nobody answered at that address.
    AlreadyInSession,           ///< This machine is already hosting or joined; Leave first.
    DirectConnectionsForbidden, ///< `allowDirectConnections` is off in `config/network.json`.
    Count_
};

/// @brief Why a session ended.
enum class EndReason : std::uint8_t
{
    Left,           ///< This machine called Leave.
    HostLeft,       ///< The host ended the session.
    ConnectionLost, ///< The connection to the host dropped.
    Count_
};

/// @brief The enumerator's own name, such as "WrongSecret", for a log or a menu
/// that has no words of its own yet.
[[nodiscard]] std::string_view Name(HostFailure reason);
[[nodiscard]] std::string_view Name(JoinRefusal reason);
[[nodiscard]] std::string_view Name(EndReason reason);

/// @brief Hosting worked; `ctx.net.Session()` has the id, the code and the port.
AEVENT(universal)
struct SessionStarted
{
};

/// @brief Hosting failed, and why.
AEVENT(universal)
struct SessionFailed
{
    HostFailure reason = HostFailure::PortUnavailable;
};

/// @brief This machine joined: the host's world is arriving.
AEVENT(universal)
struct SessionJoined
{
};

/// @brief A join was refused, and why. Refused before the host's level was
/// built, the world is as it was; refused after (Content, or Level because the
/// load failed), the level played before is loaded again from its file.
AEVENT(universal)
struct SessionRefused
{
    JoinRefusal reason = JoinRefusal::Unreachable;
};

/// @brief The session ended, and why. The world plays alone again.
AEVENT(universal)
struct SessionEnded
{
    EndReason reason = EndReason::Left;
};

/// @brief A search heard from a session it did not know, or one whose details
/// changed. Keyed by `entry.id`: update a known row rather than add one.
AEVENT(universal)
struct SessionFound
{
    SessionEntry entry{};
};

/// @brief A search ended without hearing from a session it knew.
AEVENT(universal)
struct SessionLost
{
    Core::SessionId id{};
};

/// @brief A search ended.
AEVENT(universal)
struct SessionSearchDone
{
};

} // namespace Assisi::App
