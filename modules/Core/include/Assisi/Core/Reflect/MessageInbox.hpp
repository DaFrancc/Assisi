/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/MessageInbox.hpp
/// @brief Messages that passed their checks, waiting to reach a world's
/// systems.
///
/// A message is decoded and judged the moment it arrives, inside a network
/// poll or a tick, and pushed into the world's events only when the host asks:
/// at the start of a live tick, so the tick that follows and the frame's Update
/// and Ui read it, and a client's rerun of an old tick never does. In Core
/// because a world played alone, in a build without networking too, holds its
/// own messages the same way.

#include <Assisi/Core/ClientId.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Reflect/DecodedMessage.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace Assisi::Core::Reflect
{

class MessageInbox
{
public:
    /// @brief Keeps @p message, from @p sender, for the next Deliver.
    void Hold(ClientId sender, DecodedMessage message)
    {
        _held.push_back(Held{std::move(message), sender});
    }

    /// @brief Pushes every held message into @p events as a Received, in the
    /// order they arrived, and forgets them.
    void Deliver(EventQueue &events)
    {
        for (const Held &held : _held)
        {
            held.message.Deliver(events, held.sender);
        }
        _held.clear();
    }

    /// @brief How many messages wait for the next Deliver.
    [[nodiscard]] std::size_t Size() const { return _held.size(); }

private:
    struct Held
    {
        DecodedMessage message;
        ClientId sender;
    };

    std::vector<Held> _held;
};

} // namespace Assisi::Core::Reflect
