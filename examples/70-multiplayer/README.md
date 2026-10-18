# 70 — Multiplayer

A dedicated server and clients that join it: separate programs, over UDP (ENet). The server has no
window; it simulates the arena and every player. Each client shows everyone and sends what its
player wants to do.

```bash
cd build/examples/mp_server && ./mp_server              # one terminal
cd build/examples/mp_client && ./mp_client --name ann   # another
cd build/examples/mp_client && ./mp_client --name bo    # and another
```

W/S/A/D walk, Space jumps, the mouse turns the camera (Esc releases it, a click recaptures).
**Left click throws an iron ball; right click blasts what the crosshair is on.** Other machines:
`--host <address>`; another port: `--port` on both.

## The arena

A 64 m field of grass and a dirt path, and things to break, each made of what breaks its own way:

| Model | Made of | Breaks |
|---|---|---|
| a stone watchtower, 10 m, battlements and windows | stone | shatters; what loses its support falls |
| a brick wall, 14 m | brick in mortar | the mortar crumbles, bricks come out whole |
| a timber house with a tiled roof | timber posts, plank walls, clay tiles | planks splinter, tiles crumble |
| five trees | timber trunk, leaves | cut the trunk and the crown comes down |
| ten crates, a pyramid and a stack | planks | knocked about, and splinter when hit hard |

Balls break what they hit on impact (the server's `installImpactFracture`); a blast breaks what it
hits and shoves everything near. Everything that breaks reaches every client as it happened, with the
same pieces, and the server moves them. A client simulates only its own player: crates, balls and
rubble are shown where the server has them, a tenth of a second behind, so they never jump or bounce
from corrections.

Two things are this demo's own, not the engine's:
- your ball shows at once as a stand-in, until the server's ball arrives a round trip later;
- each client throws out cosmetic chips where things break, simulated only there and never sent. The
  server makes no body at all of fragments of one or two voxels (`PhysicsConfig::dustVoxels`), so the
  chips stand in for that dust. Keep it under the materials' shard sizes: at 12 every stone shard
  (eight voxels) was dust, and the rubble vanished.

The materials are in `arena/physics_materials.json`, so a client with another library is refused with
the rest of the scene files.

It is drawn with the examples' shared renderer (`examples/shared/voxelRenderer`, first written for
Voxel Mayhem): sun shadows, screen-space AO and a colour bounce, one-bounce ray-traced GI, voxel
reconstruction and TAA, with a crosshair for aiming.

Your own player moves the moment you press a key. The client walks it too, and the server puts it
right when the two differ (prediction).

## How it is built

| | |
|---|---|
| `server.cpp` | loads `arena/`, its materials, physics and impact fracture, `runtime::installServer`; `spawnPlayer` makes each joining client's player: the avatar prefab with a `CharacterController`, `Replicated{"avatar", {"physics.character"}}`; `onMessage` throws a ball (the `ball` prefab, a replicated iron sphere gone after 10 s) or blasts (`castRay`, `fracture`, a shove) |
| `client.cpp` | loads the same `arena/` and materials, physics (it predicts its player and simulates what moves), `runtime::installClient`; each frame `setLocalInput(move, jump)`, and clicks as `sendToServer` messages; draws, the camera behind its own player |
| `arena.h` | the models, built voxel by voxel, each colour naming its physics material; `--write-arena` (on mp_server) regenerates `arena/` |

What crosses the wire is `runtime/replication.h`'s business: the handshake (protocol version and a
hash of the scene files: other files, refused with the reason), spawns by NetId (the prefab path and
the entity's registered components), snapshots at 30 Hz drawn 100 ms behind, and input repeated in
each message. Design: `docs/plans/networking.md`.

## A hundred clients

```bash
cd build/examples/mp_bots && ./mp_bots --count 100 --seconds 20   # against a running mp_server
examples/70-multiplayer/scale.sh                                   # server and bots, checked
```

`mp_bots` runs any number of clients in one process, each with its own connection. They have no
physics and no window and walk at random. The server logs its frame time, bytes out and worst round
trip each second. With 100 clients on one machine, its frames averaged 1.2 ms (worst 7.6 ms) and it
sent about 39 kB/s to each client.

## Self-test

```bash
examples/70-multiplayer/selftest.sh        # or: ctest --preset net
```

It starts a server and two headless clients (`--headless`) as separate processes on a random local
port. Ann walks, throws three balls at the tower and blasts it, and Bo watches. Each client checks
that it joined, sees both players, and (Bo) that the other moved. Both check that something broke
and that every break matched the server's. It prints `MPTEST` lines and exits 0 or 1.
