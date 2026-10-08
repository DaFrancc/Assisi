/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file TestTransport.hpp
/// @brief A transport for a test, or a failed test saying why there is none.

#include <doctest/doctest.h>

#include <Assisi/Net/NetTransport.hpp>

#include <expected>
#include <memory>
#include <string_view>

namespace Assisi::NetSync::Test
{

/// Creates a transport, failing the running test with the reason when the
/// library will not start. Callable from a default member initializer, so a
/// harness's later members can take the transport in their init list.
inline std::unique_ptr<Net::NetTransport> MakeTransport()
{
    std::expected<std::unique_ptr<Net::NetTransport>, Net::NetTransportError> created = Net::NetTransport::Create();
    REQUIRE_MESSAGE(created.has_value(), "NetTransport::Create failed: ",
                    created.has_value() ? std::string_view{} : Net::ToString(created.error()));
    return std::move(*created);
}

} // namespace Assisi::NetSync::Test
