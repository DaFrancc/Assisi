/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file Reflect/DecodedMessage.hpp
/// @brief One network message read off the wire, waiting to be judged and then
/// delivered to a world's events.
///
/// The network decodes a message the moment it arrives, so its checks can read
/// the value, and delivers it later, at the start of a tick. Between the two it
/// holds the value without knowing its type; this is what holds it.

#include <Assisi/Core/ClientId.hpp>
#include <Assisi/Core/EventQueue.hpp>
#include <Assisi/Core/Received.hpp>
#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/MessageMeta.hpp>

#include <memory>

namespace Assisi::Core::Reflect
{

class DecodedMessage
{
public:
    /// @brief Holds nothing: what a body that could not be read decodes to.
    DecodedMessage() = default;

    /// @brief Reads one body of message type @p T, which @p meta describes,
    /// into a fresh value. Every MessageMeta's `decode` is this, for its type.
    template <typename T>
    static DecodedMessage Decode(const MessageMeta &meta, BitReader &reader, const CodecContext *context)
    {
        // Fresh rather than reused: a message has no baseline, so a field the
        // sender left out reads as its default, not as the last message's.
        std::shared_ptr<T> message = std::make_shared<T>();
        if (!ReadMessage(meta, message.get(), reader, context))
        {
            return {};
        }
        return DecodedMessage(std::move(message), &DeliverAs<T>);
    }

    /// @brief Whether it holds a message.
    explicit operator bool() const { return _message != nullptr; }

    /// @brief The message, as its type's MessageMeta fields describe it, for a
    /// check to read.
    [[nodiscard]] const void *Value() const { return _message.get(); }

    /// @brief Pushes the message into @p queue as a Received from @p sender.
    void Deliver(EventQueue &queue, ClientId sender) const
    {
        ASSISI_ASSERT(_message != nullptr, "DecodedMessage: delivering a message that was never read");
        _deliver(queue, sender, _message.get());
    }

private:
    using DeliverFn = void (*)(EventQueue &queue, ClientId sender, const void *message);

    DecodedMessage(std::shared_ptr<const void> message, DeliverFn deliver)
        : _message(std::move(message)), _deliver(deliver)
    {
    }

    template <typename T>
    static void DeliverAs(EventQueue &queue, ClientId sender, const void *message)
    {
        queue.Push(Received<T>(sender, *static_cast<const T *>(message)));
    }

    std::shared_ptr<const void> _message;
    DeliverFn _deliver = nullptr;
};

} // namespace Assisi::Core::Reflect
