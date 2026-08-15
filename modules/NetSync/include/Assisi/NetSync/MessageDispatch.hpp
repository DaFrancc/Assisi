/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */
#pragma once

/// @file MessageDispatch.hpp
/// @brief What a message handler is, and how a declaration becomes one.
///
/// ## Handlers are annotated free functions
///
/// @code
///   // In any reflected header — the declaration IS the registration:
///   AMSG_HANDLER() void HandleChatSend(NetContext &ctx, const ChatSend &msg);
///
///   // In a .cpp — just the body:
///   void HandleChatSend(NetContext &ctx, const ChatSend &msg)
///   {
///       ctx.session->Send(ChatLine{ .from = ctx.sender, .text = msg.text });
///   }
/// @endcode
///
/// Functions rather than lambdas, and not only by preference: reflectgen scans
/// *declarations*, and a lambda has no declaration, no name, and no linkage to
/// reference. Handlers marked `static` or living in an anonymous namespace are
/// rejected for the same reason — the generated table could not name them.
///
/// One fixed signature, `void(NetContext &, const T &)`. The rigidity is what
/// keeps the scan a fixed-shape pattern match instead of a C++ signature parser:
/// extracting one type from one fixed pattern does not reopen the
/// function-parsing problem that messages-as-structs exists to avoid, because
/// the wire form still comes entirely from the struct.
///
/// ## The binding is generated, and unambiguous by construction
///
/// reflectgen emits each handler's binding beside its own header's component
/// registrations, in that module's generated OBJECT library. Three layers keep
/// it from ever calling the wrong function:
///
///  1. every name is spelled fully qualified and anchored at global scope
///     (`::MyGame::Chat::HandleChatSend`), so no `using` directive, ADL, or
///     nearer-scope name in the generated file can redirect the lookup;
///  2. the address is taken through an explicit signature cast, so an overload
///     set resolves to exactly the declared shape at compile time;
///  3. two handlers for one message type is a build error naming both
///     declaration sites — not first-wins, not link-order. Only a whole-tree
///     pass can see that, so it gets one: reflectgen.py --check-handlers.
///
/// Generated rather than hand-written, and that is a linkage argument rather
/// than a taste one: modules are static libraries and linkers strip registrar
/// objects nobody references, but an OBJECT library is always pulled whole into
/// the final link (cmake/AssisiReflect.cmake), so a generated binding cannot
/// vanish by link order.
///
/// ## Handlers are ordinary gameplay code
///
/// Query the world, mutate it, spawn things, send further messages. On the
/// server those mutations *are* the replication — the state channel picks them
/// up on its own. On a client they do the local and cosmetic work. Functions
/// over a context is the same shape ECS systems already use, which is also why
/// no captures are wanted.
///
/// Dispatch never runs from a network thread and never races a system: intents
/// dispatch on the server at a fixed point before the simulation consumes their
/// consequences, and events dispatch on a client after the packet's state has
/// been applied.

#include <Assisi/Core/Reflect/BinaryCodec.hpp>
#include <Assisi/Core/Reflect/MessageRegistry.hpp>
// Generated: every AMSG type's direction and reliability as compile-time
// constants, which is what lets a send call refuse the wrong direction at the
// call site instead of dropping a packet at the far end.
#include <Assisi/Core/Reflect/MessageTraits.hpp>
#include <Assisi/NetSync/NetProtocol.hpp>

#include <typeindex>
#include <vector>

namespace Assisi::ECS
{
// `struct`, matching the definition in ECS/Scene.hpp. The tags have to agree:
// under the Microsoft C++ ABI the class-key is part of the decorated name, so a
// mismatched forward declaration is a link error there and nothing at all here.
struct Scene;
}

namespace Assisi::NetSync
{

class NetSession;

/// @brief What a handler is told about the message it is handling.
///
/// Deliberately small. Everything a handler needs to *act* is reachable from
/// the scene and the session; everything it needs to *judge* is the sender.
struct NetContext
{
    /// @brief Who sent this. Meaningful for intents — it is the identity every
    /// authority check is made against — and for events it is the authority,
    /// which is to say nobody in particular.
    ///
    /// Never guessed and never taken from the payload: it is the connection the
    /// bytes arrived on, or `HostClientId` for the host's own submissions.
    ClientId sender;

    /// @brief The session this arrived through, for sending further messages.
    /// Never null in a dispatch.
    NetSession *session = nullptr;

    /// @brief The world to query and mutate. Never null in a dispatch.
    ECS::Scene *scene = nullptr;
};

/// @brief Where the generated bindings land: message type → the one function
/// that handles it.
///
/// A service locator for the same reason ComponentRegistry is one: binding
/// happens in a static initializer, before main() and before any owner object
/// could exist. Immutable once startup is over.
class MessageDispatch
{
public:
    static MessageDispatch &Instance();

    /// @brief Erased function-pointer storage.
    ///
    /// `void(*)()` rather than `void*`: converting a function pointer to an
    /// object pointer is only conditionally supported, while converting between
    /// function pointer types and back is well defined.
    using ErasedFn = void (*)();

    /// @brief Bind @p handler as the handler for `T`. Called by generated code.
    ///
    /// The template parameter is spelled explicitly at every call site, so the
    /// binding cannot be deduced into a different type than the one the
    /// declaration named.
    template <typename T>
    void Bind(void (*handler)(NetContext &, const T &))
    {
        BindErased(typeid(T), &DecodeAndInvoke<T>, reinterpret_cast<ErasedFn>(handler));
    }

    /// @brief Judge a decoded message before its handler sees it.
    ///
    /// The seam that keeps validation at one site instead of one per handler.
    /// Returning false drops the message; the callback owns the counting,
    /// because only it knows *which* rule refused.
    using ValidateFn = bool (*)(const Core::Reflect::MessageMeta &meta, const void *message, void *userData);

    /// @brief Decode one message body and hand it to its handler.
    ///
    /// @param meta     The message type, already resolved from the wire id.
    /// @param reader   Positioned at the message's length prefix.
    /// @param validate Optional gate run on the decoded value, before the
    ///   handler. This is where range and control checks live: they need the
    ///   decoded value, and putting them in each handler would be the
    ///   hand-written-validation pattern that every documented exploit in the
    ///   survey came out of.
    /// @return false when no handler is bound — the body is left unread, so the
    ///   caller can skip it and count the drop. A message nobody handles is a
    ///   normal condition, not an error: the sender's build may simply care
    ///   about something this one does not. A message the validator refuses
    ///   returns *true*: it was dispatched to, and refused, which is a different
    ///   fact from nobody being home.
    bool Dispatch(const Core::Reflect::MessageMeta &meta, NetContext &context, Core::BitReader &reader,
                  const Core::Reflect::CodecContext *codec = nullptr, ValidateFn validate = nullptr,
                  void *userData = nullptr) const;

    /// @brief Whether anything handles @p meta's type.
    [[nodiscard]] bool HasHandler(const Core::Reflect::MessageMeta &meta) const;

    /// @brief How many handlers are bound. For diagnostics and tests.
    [[nodiscard]] std::size_t Count() const { return _bindings.size(); }

    /// @brief Drop every binding. Tests only — a real build binds once, from
    /// generated code, and never unbinds.
    void Clear() { _bindings.clear(); }

private:
    MessageDispatch() = default;

    using InvokeFn = void (*)(NetContext &, Core::BitReader &, const Core::Reflect::MessageMeta &,
                              const Core::Reflect::CodecContext *, ValidateFn, void *, ErasedFn);

    /// The concrete trampoline, instantiated once per message type. It is what
    /// knows the type well enough to make one on the stack, fill it from the
    /// wire, judge it, and pass it by const reference.
    template <typename T>
    static void DecodeAndInvoke(NetContext &context, Core::BitReader &reader,
                                const Core::Reflect::MessageMeta &meta,
                                const Core::Reflect::CodecContext *codec, ValidateFn validate,
                                void *userData, ErasedFn handler)
    {
        // Value-initialized, then filled: a message has no baseline to patch, so
        // any field the sender omitted reads as that field's default rather than
        // as whatever the last message left behind.
        T message{};
        if (!Core::Reflect::ReadMessage(meta, &message, reader, codec))
            return;
        if (validate != nullptr && !validate(meta, &message, userData))
            return;
        reinterpret_cast<void (*)(NetContext &, const T &)>(handler)(context, message);
    }

    struct Binding
    {
        std::type_index type;
        InvokeFn invoke  = nullptr;
        ErasedFn handler = nullptr;
    };

    void BindErased(std::type_index type, InvokeFn invoke, ErasedFn handler);

    [[nodiscard]] const Binding *Find(std::type_index type) const;

    /// A short flat vector, searched linearly. A game with hundreds of message
    /// types would still be searching a vector that fits in a cache line or two,
    /// and the alternative — a hash map — costs more to build at static-init
    /// time than it saves per dispatch at this size.
    std::vector<Binding> _bindings;
};

/// @brief Encode @p message onto @p writer, id and length prefix included.
///
/// @return false if `T` is not a registered `AMSG` type — which is a build
/// mistake rather than a runtime condition, and is logged as one.
template <typename T>
bool WriteMessageValue(const T &message, Core::BitWriter &writer,
                       const Core::Reflect::CodecContext *codec = nullptr)
{
    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    const Core::Reflect::MessageId id       = registry.IdOf(typeid(T));
    const Core::Reflect::MessageMeta *meta     = registry.ById(id);
    if (meta == nullptr)
        return false;
    return Core::Reflect::WriteMessage(*meta, &message, writer, codec);
}

/// @brief The registered descriptor for `T`, or null if it is not an `AMSG`.
template <typename T>
[[nodiscard]] const Core::Reflect::MessageMeta *MessageMetaOf()
{
    const Core::Reflect::MessageRegistry &registry = Core::Reflect::MessageRegistry::Instance();
    return registry.ById(registry.IdOf(typeid(T)));
}

} // namespace Assisi::NetSync
