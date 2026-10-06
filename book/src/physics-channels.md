# Collision channels

By default, everything collides with everything. Channels let you change that,
for example so that bullets fly through trigger zones, or a camera's
line-of-sight check ignores small props.

Every `Collider` has two channel fields:

- `channel`: what kind of object it is, such as `World` or `Character`.
- `collidesWith`: the list of channels it collides with.

Two colliders collide only if **each** one's `collidesWith` includes the other's
`channel`. So either one can opt out on its own: to make a prop that characters
walk through, untick `Character` in the prop's `collidesWith`.

Rays and other queries (see [Contacts and queries](physics-contacts.md)) follow
the same rule: a query has a channel and a `collidesWith` list too.

There are 32 channels. The engine defines six:

| Channel | For |
|---|---|
| `World` | Ordinary matter: floors, walls, props. |
| `Character` | Player and NPC capsules. |
| `Trigger` | Volumes that detect what enters them and block nothing. |
| `Visibility` | Line-of-sight and picking queries. |
| `Camera` | Camera collision queries. |
| `Hitbox` | The parts of a character a shot can hit. A convention only: the engine treats it like any other channel. |

## Naming your game's channels

The other 26 belong to your game. Take them with `GameChannel` and name them
in a header under `apps/game/src/`:

```cpp
#include <Assisi/Physics/CollisionChannelNames.hpp>

inline constexpr Assisi::Physics::CollisionChannel Bullet = Assisi::Physics::GameChannel(0);
inline constexpr Assisi::Physics::CollisionChannel Pickup = Assisi::Physics::GameChannel(1);

inline constexpr Assisi::Physics::ChannelName kChannelNames[] = {
    {"Bullet", Bullet},
    {"Pickup", Pickup},
};
ASSISI_COLLISION_CHANNEL_NAMES(kChannelNames);
```

Code uses the aliases, for example a ray that only hits pickups:

```cpp
using namespace Assisi;

const Physics::CollisionFilter pickupsOnly{Core::Bitmask<Physics::CollisionChannel>::Of(Pickup), Bullet};
std::optional<Physics::QueryHit> hit = world.CastRay(eye, forward * 3.f, pickupsOnly, player);
```

The editor shows your names in every channel dropdown and mask, and hides the
slots you haven't named. A level file saves a mask by those names too, as
`"collidesWith": ["World", "Bullet"]`, or `"collidesWith": "All"` for every
channel. If a mask has some of those hidden slots set and
others not, its heading says how many are set.

These mistakes fail to build:
- a `GameChannel` number past 25;
- two names for one slot, or one name for two slots;
- an empty name;
- a name for one of the engine's own channels.

A new collider collides with every channel, your game's included. So when you
start using a channel, everything already collides with it until you untick it
in the masks that shouldn't.
