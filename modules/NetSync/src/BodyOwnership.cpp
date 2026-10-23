/* Copyright (c) 2025 Francisco Vivas Puerto (aka "DaFrancc"). */

#include <Assisi/NetSync/BodyOwnership.hpp>

#include <algorithm>
#include <unordered_set>

namespace Assisi::NetSync
{
namespace
{

/// One group's player for this tick, before the most bodies a player may own
/// is counted.
struct Choice
{
    const BodyGroup *group = nullptr;
    float distance = 0.f;
    std::uint32_t client = 0;
};

bool Nearer(const Choice &a, const Choice &b)
{
    return a.distance < b.distance;
}

/// How many players touching a body at once contest it.
constexpr std::size_t kContestants = 2;

} // namespace

void BodyOwnership::Assign(std::span<const BodyGroup> groups, std::uint64_t tick)
{
    std::vector<Choice> choices;
    choices.reserve(groups.size());
    std::unordered_set<NetId> present;
    for (const BodyGroup &group : groups)
    {
        present.insert(group.bodies.begin(), group.bodies.end());
        const std::uint32_t client = Choose(group, tick);
        const PlayerReach *reach = ReachOf(group, client);
        choices.push_back(Choice{.group = &group, .distance = reach != nullptr ? reach->distance : 0.f, .client = client});
    }

    // Nearest first, so a player over the limit keeps the bodies it is
    // nearest; the rest stay with the host. The host's own player has no
    // limit: what it holds the host simulates anyway.
    std::stable_sort(choices.begin(), choices.end(), Nearer);
    std::unordered_map<std::uint32_t, std::size_t> owned;
    for (Choice &choice : choices)
    {
        if (choice.client != 0 && choice.client != HostClientId.value)
        {
            std::size_t &count = owned[choice.client];
            if (count + choice.group->bodies.size() > _config.maxBodiesPerClient)
            {
                choice.client = 0;
            }
            else
            {
                count += choice.group->bodies.size();
            }
        }
        Give(*choice.group, choice.client, tick);
    }

    std::erase_if(_holdings, [&present](const std::pair<const NetId, Holding> &held)
                  { return !present.contains(held.first); });
}

std::uint32_t BodyOwnership::OwnerOf(NetId netId) const
{
    const std::unordered_map<NetId, Holding>::const_iterator found = _holdings.find(netId);
    return found == _holdings.end() ? 0u : found->second.client;
}

void BodyOwnership::TakeBack(NetId netId, std::uint64_t tick)
{
    Holding &holding = _holdings[netId];
    holding = Holding{.changedTick = tick, .takenBack = true};
}

void BodyOwnership::ForgetPlayer(std::uint32_t client)
{
    for (std::pair<const NetId, Holding> &held : _holdings)
    {
        if (held.second.client == client)
        {
            held.second = Holding{};
        }
        else if (held.second.challenger == client)
        {
            held.second.challenger = 0;
            held.second.challengedSince = 0;
        }
    }
}

std::uint32_t BodyOwnership::Choose(const BodyGroup &group, std::uint64_t tick)
{
    switch (Contest(group, tick))
    {
    case ContestState::Contested:
        return 0;
    case ContestState::Ended:
        // The hold the contest began is not waited out: the body has been the
        // host's for the dwell already.
        return NewHolderOf(group);
    case ContestState::None:
        break;
    }

    const std::uint32_t holder = HolderOf(group);
    const PlayerReach *reach = holder != 0 ? ReachOf(group, holder) : nullptr;
    const bool kept = reach != nullptr && reach->distance <= _config.leaveDistance;
    const std::uint32_t challenger = kept ? ChallengerOf(group, holder) : 0u;

    // The challenge goes on from when it began, on whichever body of the group
    // first saw it, and is forgotten the moment it stops.
    std::uint64_t since = tick;
    for (const NetId netId : group.bodies)
    {
        const std::unordered_map<NetId, Holding>::const_iterator found = _holdings.find(netId);
        if (challenger != 0 && found != _holdings.end() && found->second.challenger == challenger)
        {
            since = std::min(since, found->second.challengedSince);
        }
    }
    for (const NetId netId : group.bodies)
    {
        Holding &holding = _holdings[netId];
        holding.challenger = challenger;
        holding.challengedSince = challenger != 0 ? since : 0u;
    }

    if (kept)
    {
        const bool dwelt = challenger != 0 && tick - since >= _config.touchDwellTicks;
        return dwelt && !Held(group, tick) ? challenger : holder;
    }
    const std::uint32_t next = NewHolderOf(group);
    return next != 0 && Held(group, tick) ? 0u : next;
}

BodyOwnership::ContestState BodyOwnership::Contest(const BodyGroup &group, std::uint64_t tick)
{
    const std::size_t touching = static_cast<std::size_t>(
        std::count_if(group.players.begin(), group.players.end(), [](const PlayerReach &player) { return player.touching; }));

    // Whichever of its bodies was contested last speaks for the group: a body
    // a contested one comes to touch joins the contest.
    std::uint64_t contestedTick = 0;
    if (touching >= kContestants)
    {
        contestedTick = tick;
    }
    else
    {
        for (const NetId netId : group.bodies)
        {
            const std::unordered_map<NetId, Holding>::const_iterator found = _holdings.find(netId);
            if (found != _holdings.end())
            {
                contestedTick = std::max(contestedTick, found->second.contestedTick);
            }
        }
        if (contestedTick == 0)
        {
            return ContestState::None;
        }
    }

    // Kept for the dwell after the last but one player lets go, or two players
    // leaning on a body by turns would hand it back and forth.
    const bool over = touching < kContestants && tick - contestedTick >= _config.touchDwellTicks;
    for (const NetId netId : group.bodies)
    {
        Holding &holding = _holdings[netId];
        holding.contestedTick = over ? 0u : contestedTick;
        holding.challenger = 0;
        holding.challengedSince = 0;
    }
    return over ? ContestState::Ended : ContestState::Contested;
}

std::uint32_t BodyOwnership::HolderOf(const BodyGroup &group) const
{
    std::uint32_t holder = 0;
    float nearest = 0.f;
    for (const NetId netId : group.bodies)
    {
        const std::uint32_t client = OwnerOf(netId);
        const PlayerReach *reach = client != 0 ? ReachOf(group, client) : nullptr;
        const float distance = reach != nullptr ? reach->distance : _config.leaveDistance;
        if (client != 0 && (holder == 0 || distance < nearest))
        {
            holder = client;
            nearest = distance;
        }
    }
    return holder;
}

std::uint32_t BodyOwnership::ChallengerOf(const BodyGroup &group, std::uint32_t holder)
{
    const PlayerReach *held = ReachOf(group, holder);
    if (held != nullptr && held->touching)
    {
        return 0;
    }
    const PlayerReach *nearest = nullptr;
    for (const PlayerReach &player : group.players)
    {
        if (player.touching && player.client != holder && (nearest == nullptr || player.distance < nearest->distance))
        {
            nearest = &player;
        }
    }
    return nearest != nullptr ? nearest->client : 0u;
}

std::uint32_t BodyOwnership::NewHolderOf(const BodyGroup &group) const
{
    const PlayerReach *best = nullptr;
    for (const PlayerReach &player : group.players)
    {
        if (!player.touching && player.distance > _config.ownershipDistance)
        {
            continue;
        }
        // A touch beats nearness; between two of a kind the nearer wins.
        const bool better = best == nullptr || (player.touching && !best->touching) ||
                            (player.touching == best->touching && player.distance < best->distance);
        if (better)
        {
            best = &player;
        }
    }
    return best != nullptr ? best->client : 0u;
}

bool BodyOwnership::Held(const BodyGroup &group, std::uint64_t tick) const
{
    for (const NetId netId : group.bodies)
    {
        const std::unordered_map<NetId, Holding>::const_iterator found = _holdings.find(netId);
        if (found != _holdings.end() && found->second.changedTick != 0 &&
            tick < found->second.changedTick + _config.holdTicks)
        {
            return true;
        }
    }
    return false;
}

const PlayerReach *BodyOwnership::ReachOf(const BodyGroup &group, std::uint32_t client)
{
    for (const PlayerReach &player : group.players)
    {
        if (player.client == client)
        {
            return &player;
        }
    }
    return nullptr;
}

void BodyOwnership::Give(const BodyGroup &group, std::uint32_t client, std::uint64_t tick)
{
    for (const NetId netId : group.bodies)
    {
        Holding &holding = _holdings[netId];
        if (holding.takenBack && tick < holding.changedTick + _config.holdTicks)
        {
            continue;
        }
        holding.takenBack = false;
        if (holding.client != client)
        {
            holding.client = client;
            holding.changedTick = tick;
        }
    }
}

} // namespace Assisi::NetSync
