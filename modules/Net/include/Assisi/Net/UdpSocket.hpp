/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file UdpSocket.hpp
/// @brief A plain IPv4 UDP socket that can broadcast, for finding sessions on a LAN.
///
/// GameNetworkingSockets carries connections and cannot broadcast, so LAN
/// discovery needs a socket of its own. This is that socket and nothing more: it
/// moves datagrams and knows nothing of what they mean (NetSync's
/// SessionDiscovery does). Non-blocking, so a game polls it once a frame.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Assisi::Net
{

/// @brief An IPv4 address, as dotted text, and a port.
struct UdpEndpoint
{
    std::string address;
    std::uint16_t port = 0;
};

/// @brief One datagram that arrived, and who sent it.
struct UdpDatagram
{
    std::vector<std::byte> bytes;
    UdpEndpoint from;
};

/// @brief How the socket's port is shared.
enum class UdpPortUse : std::uint8_t
{
    Own,    ///< This socket alone, as a joiner's ephemeral one is.
    Shared, ///< Every socket on this machine bound shared to the port hears its broadcasts, so two hosts on one machine both answer.
    Count_
};

class UdpSocket
{
public:
    /// @brief Binds to @p port on every interface, or to any free port when it
    /// is 0, with broadcasting allowed.
    /// @return the socket, or why it could not be opened.
    [[nodiscard]] static std::expected<UdpSocket, std::string> Open(std::uint16_t port, UdpPortUse use);

    ~UdpSocket();
    UdpSocket(UdpSocket &&other) noexcept;
    UdpSocket &operator=(UdpSocket &&other) noexcept;
    UdpSocket(const UdpSocket &) = delete;
    UdpSocket &operator=(const UdpSocket &) = delete;

    /// @brief Sends @p bytes to @p to, which may be a broadcast address.
    /// @return false when the address does not parse or the send failed.
    bool SendTo(const UdpEndpoint &to, std::span<const std::byte> bytes);

    /// @brief The next datagram that arrived, or nothing when none is waiting.
    /// Never blocks.
    [[nodiscard]] std::optional<UdpDatagram> Receive();

    /// @brief The port this socket is bound to.
    [[nodiscard]] std::uint16_t Port() const;

    /// @brief Where a query has to go to reach every machine on every LAN this
    /// one is on: the limited broadcast 255.255.255.255, each interface's own
    /// broadcast address, and the loopback network's. Windows sends the limited
    /// broadcast out of only one interface, so the per-interface ones are what
    /// reach the rest; the loopback one reaches hosts on this machine.
    [[nodiscard]] static std::vector<std::string> BroadcastAddresses();

private:
    /// The platform's socket handle, held as the widest of them.
    using Handle = std::uintptr_t;

    explicit UdpSocket(Handle handle) : _handle(handle) {}

    void CloseHandle();

    Handle _handle;
};

} // namespace Assisi::Net
