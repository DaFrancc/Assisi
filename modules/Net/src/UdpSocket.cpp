/* Copyright (c) 2026 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Net/UdpSocket.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>

#ifdef _WIN32
#    include <winsock2.h>
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <cerrno>
#    include <fcntl.h>
#    include <ifaddrs.h>
#    include <net/if.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace Assisi::Net
{
namespace
{

/// No socket: INVALID_SOCKET on Windows and -1 elsewhere, both all ones.
constexpr std::uintptr_t kInvalidHandle = std::numeric_limits<std::uintptr_t>::max();

/// The largest datagram read. A discovery datagram is far smaller, and anything
/// larger is not one.
constexpr std::size_t kReceiveBufferBytes = 1500;

constexpr const char *kLimitedBroadcast = "255.255.255.255";
constexpr const char *kLoopbackBroadcast = "127.255.255.255";

#ifdef _WIN32
using NativeSocket = SOCKET;
using AddressLength = int;
using BufferLength = int;

/// Winsock is started for as long as any socket of ours is open.
std::mutex g_winsockMutex;
std::int32_t g_winsockRefs = 0;

bool AcquireWinsock()
{
    const std::lock_guard<std::mutex> lock(g_winsockMutex);
    if (g_winsockRefs == 0)
    {
        constexpr WORD kVersion = MAKEWORD(2, 2);
        WSADATA data{};
        if (WSAStartup(kVersion, &data) != 0)
        {
            return false;
        }
    }
    ++g_winsockRefs;
    return true;
}

void ReleaseWinsock()
{
    const std::lock_guard<std::mutex> lock(g_winsockMutex);
    if (--g_winsockRefs == 0)
    {
        WSACleanup();
    }
}

void CloseNative(NativeSocket socket) { ::closesocket(socket); }

bool SetNonBlocking(NativeSocket socket)
{
    u_long nonBlocking = 1;
    return ::ioctlsocket(socket, FIONBIO, &nonBlocking) == 0;
}
#else
using NativeSocket = int;
using AddressLength = socklen_t;
using BufferLength = std::size_t;

bool AcquireWinsock() { return true; }
void ReleaseWinsock() {}

void CloseNative(NativeSocket socket) { ::close(socket); }

bool SetNonBlocking(NativeSocket socket)
{
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

NativeSocket Native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }

bool SetOption(NativeSocket socket, int level, int option)
{
    const int enabled = 1;
    return ::setsockopt(socket, level, option, reinterpret_cast<const char *>(&enabled), sizeof(enabled)) == 0;
}

std::string AddressText(const in_addr &address)
{
    std::array<char, INET_ADDRSTRLEN> text{};
    if (::inet_ntop(AF_INET, &address, text.data(), text.size()) == nullptr)
    {
        return {};
    }
    return text.data();
}

void AddUnique(std::vector<std::string> &addresses, const std::string &address)
{
    if (!address.empty() && std::find(addresses.begin(), addresses.end(), address) == addresses.end())
    {
        addresses.push_back(address);
    }
}

} // namespace

std::expected<UdpSocket, std::string> UdpSocket::Open(std::uint16_t port, UdpPortUse use)
{
    if (!AcquireWinsock())
    {
        return std::unexpected(std::string("could not start Winsock"));
    }
    const NativeSocket native = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (static_cast<std::uintptr_t>(native) == kInvalidHandle)
    {
        ReleaseWinsock();
        return std::unexpected(std::string("could not create a UDP socket"));
    }
    // From here the socket's destructor closes it and releases Winsock.
    UdpSocket socket(static_cast<std::uintptr_t>(native));

    // Before the bind, which is when sharing is decided.
    if (use == UdpPortUse::Shared && !SetOption(native, SOL_SOCKET, SO_REUSEADDR))
    {
        return std::unexpected(std::string("could not share the port"));
    }
    if (!SetOption(native, SOL_SOCKET, SO_BROADCAST) || !SetNonBlocking(native))
    {
        return std::unexpected(std::string("could not set up the socket"));
    }

    sockaddr_in bound{};
    bound.sin_family = AF_INET;
    bound.sin_addr.s_addr = htonl(INADDR_ANY);
    bound.sin_port = htons(port);
    if (::bind(native, reinterpret_cast<const sockaddr *>(&bound), sizeof(bound)) != 0)
    {
        return std::unexpected("could not bind UDP port " + std::to_string(port));
    }
    return socket;
}

UdpSocket::~UdpSocket() { CloseHandle(); }

UdpSocket::UdpSocket(UdpSocket &&other) noexcept : _handle(std::exchange(other._handle, kInvalidHandle)) {}

UdpSocket &UdpSocket::operator=(UdpSocket &&other) noexcept
{
    if (this != &other)
    {
        CloseHandle();
        _handle = std::exchange(other._handle, kInvalidHandle);
    }
    return *this;
}

void UdpSocket::CloseHandle()
{
    if (_handle == kInvalidHandle)
    {
        return;
    }
    CloseNative(Native(_handle));
    _handle = kInvalidHandle;
    ReleaseWinsock();
}

bool UdpSocket::SendTo(const UdpEndpoint &to, std::span<const std::byte> bytes)
{
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(to.port);
    if (::inet_pton(AF_INET, to.address.c_str(), &target.sin_addr) != 1)
    {
        return false;
    }
    const std::int64_t sent = static_cast<std::int64_t>(
        ::sendto(Native(_handle), reinterpret_cast<const char *>(bytes.data()), static_cast<BufferLength>(bytes.size()),
                 0, reinterpret_cast<const sockaddr *>(&target), sizeof(target)));
    return sent == static_cast<std::int64_t>(bytes.size());
}

std::optional<UdpDatagram> UdpSocket::Receive()
{
    std::array<std::byte, kReceiveBufferBytes> buffer{};
    sockaddr_in from{};
    AddressLength fromLength = sizeof(from);
    // A datagram too big for the buffer is not one of ours; skip it and look at
    // the next, rather than stop listening.
    for (;;)
    {
        const std::int64_t received = static_cast<std::int64_t>(
            ::recvfrom(Native(_handle), reinterpret_cast<char *>(buffer.data()),
                       static_cast<BufferLength>(buffer.size()), 0, reinterpret_cast<sockaddr *>(&from), &fromLength));
        if (received >= 0)
        {
            UdpDatagram datagram;
            datagram.bytes.assign(buffer.begin(), buffer.begin() + received);
            datagram.from = UdpEndpoint{.address = AddressText(from.sin_addr), .port = ntohs(from.sin_port)};
            return datagram;
        }
#ifdef _WIN32
        // Windows reports a datagram too big, and a port an earlier send of
        // ours found closed, as errors on the next receive.
        const int error = WSAGetLastError();
        if (error == WSAEMSGSIZE || error == WSAECONNRESET)
        {
            continue;
        }
#endif
        return std::nullopt;
    }
}

std::uint16_t UdpSocket::Port() const
{
    sockaddr_in bound{};
    AddressLength length = sizeof(bound);
    if (::getsockname(Native(_handle), reinterpret_cast<sockaddr *>(&bound), &length) != 0)
    {
        return 0;
    }
    return ntohs(bound.sin_port);
}

std::vector<std::string> UdpSocket::BroadcastAddresses()
{
    std::vector<std::string> addresses{kLimitedBroadcast, kLoopbackBroadcast};
#ifdef _WIN32
    if (!AcquireWinsock())
    {
        return addresses;
    }
    const SOCKET probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != INVALID_SOCKET)
    {
        // Each interface's address and mask; its broadcast is the address with
        // every host bit set. Windows' own broadcast field is always the limited one.
        constexpr std::size_t kMaxInterfaces = 32;
        std::array<INTERFACE_INFO, kMaxInterfaces> interfaces{};
        DWORD bytes = 0;
        if (::WSAIoctl(probe, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces.data(),
                       static_cast<DWORD>(sizeof(interfaces)), &bytes, nullptr, nullptr) == 0)
        {
            const std::size_t count = bytes / sizeof(INTERFACE_INFO);
            for (std::size_t i = 0; i < count; ++i)
            {
                const INTERFACE_INFO &info = interfaces[i];
                if ((info.iiFlags & IFF_UP) == 0 || (info.iiFlags & IFF_BROADCAST) == 0 ||
                    (info.iiFlags & IFF_LOOPBACK) != 0)
                {
                    continue;
                }
                in_addr broadcast{};
                broadcast.s_addr = info.iiAddress.AddressIn.sin_addr.s_addr | ~info.iiNetmask.AddressIn.sin_addr.s_addr;
                AddUnique(addresses, AddressText(broadcast));
            }
        }
        ::closesocket(probe);
    }
    ReleaseWinsock();
#else
    ifaddrs *list = nullptr;
    if (::getifaddrs(&list) == 0)
    {
        for (const ifaddrs *entry = list; entry != nullptr; entry = entry->ifa_next)
        {
            if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
                (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_BROADCAST) == 0 ||
                entry->ifa_broadaddr == nullptr)
            {
                continue;
            }
            AddUnique(addresses, AddressText(reinterpret_cast<const sockaddr_in *>(entry->ifa_broadaddr)->sin_addr));
        }
        ::freeifaddrs(list);
    }
#endif
    return addresses;
}

} // namespace Assisi::Net
