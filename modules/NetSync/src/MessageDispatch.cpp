/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/MessageDispatch.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>

namespace Assisi::NetSync
{

MessageDispatch &MessageDispatch::Instance()
{
    static MessageDispatch instance;
    return instance;
}

void MessageDispatch::BindErased(std::type_index type, InvokeFn invoke, ErasedFn handler)
{
    // The generated table is produced by a pass that already refuses to emit two
    // handlers for one type, naming both declaration sites. Reaching this is
    // therefore either a hand-written second binding or a table linked twice,
    // and both are worth saying out loud rather than resolving by first-wins.
    if (Find(type) != nullptr)
    {
        Core::Log::Error("NetSync: a second handler was bound for message type '{}'. One message type has "
                         "exactly one handler; keeping the first.",
                         type.name());
        ASSISI_ASSERT(false, "duplicate message handler binding");
        return;
    }

    _bindings.push_back(Binding{type, invoke, handler});
}

const MessageDispatch::Binding *MessageDispatch::Find(std::type_index type) const
{
    const auto it = std::find_if(_bindings.begin(), _bindings.end(),
                                 [type](const Binding &binding) { return binding.type == type; });
    return it == _bindings.end() ? nullptr : &*it;
}

bool MessageDispatch::HasHandler(const Core::Reflect::MessageMeta &meta) const
{
    return Find(meta.typeIndex) != nullptr;
}

bool MessageDispatch::Dispatch(const Core::Reflect::MessageMeta &meta, NetContext &context,
                               Core::BitReader &reader, const Core::Reflect::CodecContext *codec,
                               ValidateFn validate, void *userData) const
{
    const Binding *binding = Find(meta.typeIndex);
    if (binding == nullptr)
        return false;

    binding->invoke(context, reader, meta, codec, validate, userData, binding->handler);
    return true;
}

} // namespace Assisi::NetSync
