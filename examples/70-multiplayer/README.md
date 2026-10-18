# 70 — Multiplayer

A dedicated server and clients that join it: separate programs, over UDP (ENet). The server has no
window; it simulates the arena and every player. Each client shows everyone and sends what its
player wants to do.

```bash
cd build/examples/mp_server && ./mp_server              # one terminal
cd build/examples/mp_client && ./mp_client --name ann   # another
cd build/examples/mp_client && ./mp_client --name bo    # and another
```

W/S/A/D walk, Space jumps, the mouse turns the camera (Esc releases it, a click recaptures). Other
machines: `--host <address>`; another port: `--port` on both.

Your own player moves the moment you press a key. The client walks it too, and the server puts it
right when the two differ (prediction).

## How it is built

| | |
|---|---|
| `server.cpp` | loads `arena/`, physics, `runtime::installServer`; `spawnPlayer` makes each joining client's player: the avatar prefab with a `CharacterController`, `Replicated{"avatar", {"physics.character"}}` |
| `client.cpp` | loads the same `arena/`, `runtime::installClient`; each frame `setLocalInput(move, jump)`; draws, the camera behind its own player |
| `arena.h` | the arena both load, and `--write-arena` (on mp_server) that regenerates it |

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
port. Ann walks 9 m and Bo watches. Each client checks that it joined, sees both players, and (Bo)
that the other moved. It prints `MPTEST` lines and exits 0 or 1.
