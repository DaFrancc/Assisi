/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/Core/Reflect/MessageRegistry.hpp>

#include <Assisi/Core/Assert.hpp>
#include <Assisi/Core/Logger.hpp>

#include <algorithm>

namespace Assisi::Core::Reflect
{

MessageRegistry &MessageRegistry::Instance()
{
    static MessageRegistry instance;
    return instance;
}

void MessageRegistry::Register(MessageMeta meta)
{
    // The same immutability rule ComponentRegistry documents at length: ids are
    // positions in the name-sorted list, and All()/ById() hand out pointers into
    // the vector. A late arrival would both renumber and dangle.
    if (_finalized)
    {
        Core::Log::Error("MessageRegistry: refusing late registration of '{}' — the registry is already "
                         "finalized and its ids are on the wire. The message will not be reflected.",
                         meta.name);
        ASSISI_ASSERT(false, "MessageRegistry::Register after an id was issued — ids are positions in the "
                             "name-sorted list, and they are what the wire carries");
        return;
    }

    _metas.push_back(std::move(meta));
}

void MessageRegistry::EnsureFinalized() const
{
    if (_finalized)
        return;

    std::sort(_metas.begin(), _metas.end(),
              [](const MessageMeta &lhs, const MessageMeta &rhs) { return lhs.name < rhs.name; });

    // Duplicate names are fatal here, where duplicate *components* are merely
    // dropped with an error. The difference is what the id means: two messages
    // sharing a name share a dense id, so one machine encodes type A and the
    // other decodes type B — same bytes, different meaning, no error anywhere.
    // That is precisely the silent corruption the protocol hash exists to
    // prevent, and it would sail straight past the hash because both builds
    // agree on the (identical) name list.
    for (std::size_t i = 1; i < _metas.size(); ++i)
    {
        if (_metas[i].name == _metas[i - 1].name)
        {
            Core::Log::Error("MessageRegistry: two message types are both named '{}'. Message names are the "
                             "wire identity — rename one.",
                             _metas[i].name);
            ASSISI_ASSERT(false, "duplicate AMSG name — the two would share a dense wire id and misdispatch");
        }
    }

    _idByType.clear();
    _idByType.reserve(_metas.size());
    // Ids start at one so that a zero-initialized MessageId is invalid, matching
    // every other id in the engine.
    for (std::size_t i = 0; i < _metas.size(); ++i)
    {
        _metas[i].id = static_cast<MessageId>(i + 1);
        _idByType.emplace(_metas[i].typeIndex, _metas[i].id);
    }

    _finalized = true;
}

const MessageMeta *MessageRegistry::Find(std::string_view name) const
{
    EnsureFinalized();
    const auto it = std::find_if(_metas.begin(), _metas.end(),
                                 [name](const MessageMeta &meta) { return meta.name == name; });
    return it == _metas.end() ? nullptr : &*it;
}

std::span<const MessageMeta> MessageRegistry::All() const
{
    EnsureFinalized();
    return _metas;
}

std::size_t MessageRegistry::Count() const
{
    EnsureFinalized();
    return _metas.size();
}

MessageId MessageRegistry::IdOf(std::type_index type) const
{
    EnsureFinalized();
    const auto it = _idByType.find(type);
    return it == _idByType.end() ? kInvalidMessageId : it->second;
}

MessageId MessageRegistry::IdOf(std::string_view name) const
{
    const MessageMeta *meta = Find(name);
    return meta == nullptr ? kInvalidMessageId : meta->id;
}

const MessageMeta *MessageRegistry::ById(MessageId id) const
{
    EnsureFinalized();
    if (id == kInvalidMessageId || id > _metas.size())
        return nullptr;
    return &_metas[id - 1];
}

} // namespace Assisi::Core::Reflect
