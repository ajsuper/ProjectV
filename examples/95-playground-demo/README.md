# 95 — Playground (demo)

A field of things to break, for up to four friends at once: a castle, brick chimneys, a timber house,
a glass greenhouse, a stone bridge, trees, a pyramid of crates and a stack of TNT. No rounds and no
score -- eight tools, shared gravity, and a button that puts it all back.

```bash
cd build/examples/playground_demo && ./playground_demo                    # host one here and play in it
cd build/examples/playground_demo && ./playground_demo --host 10.0.0.5    # join someone's, or a server
cd build/examples/playground_demo_server && ./playground_demo_server                    # a dedicated server, no window
```

`--port` on both sides (27018 by default); `--name` on a client.

## Tools

| | | |
|---|---|---|
| **1** | Cannon | a 0.6 m iron ball at 48 m/s where you aim; what it hits breaks on impact |
| **2** | Blast | an explosion where you aim (up to 120 m): every breakable surface a ray from it reaches is struck, then everything near is thrown |
| **3** | TNT | a barrel, thrown; it goes off when jolted hard, caught in a blast, or after six seconds |
| **4** | Crate | thrown; it splinters where it lands hard |
| **5** | Ball | a 1.2 m rubber ball, thrown |
| **6** | Build | a metre of stone against what you aim at, on the metre grid (120 each; the oldest goes) |
| **7** | Dig | takes out everything within 1.3 m of where you aim; what it held up falls |
| **8** | Push | throws what is in a cone in front of you, players too |

## Controls

| | |
|---|---|
| **W/A/S/D**, **Space** | walk, jump |
| **trackpad** or **arrows** | look (Esc frees the cursor, a click takes it back) |
| **click** or **F** | use the tool in hand; held, it goes on as fast as the tool allows |
| **1–8**, **wheel**, **Q/E** | pick the tool |
| **V** | first person or third (over the shoulder: the crosshair is where the tool goes either way) |
| **G** | the Earth's gravity or the Moon's, for everyone |
| **Backspace** (hold) | put the world back: everything broken, built and thrown |
| **H** | the help |

## How it works

- **Everything breakable is a splitting static collider with a NetId** (`world.h` writes them so in
  `entities.json`), standing on its own y = 0 (`anchorBase`). A cannonball breaks what it hits through
  the server's impact fracture; a blast strikes what its rays reach (`runtime::fracture`); a dig
  removes voxels (`editVoxels`). Whatever the cause, the server's support pass decides what falls,
  and the replication sends the edit and the pieces to every client as they were made -- clients
  never break anything themselves.
- **Aiming is the client's camera, checked.** A client sends the ray it looks along (its camera's eye
  and direction); the server casts it again, skipping the player, if the eye is within 9 m of the
  player's head. Building, digging and pushing reach 10–12 m from the player, not the camera.
- **A knock is carried by the knocked player's own walking**, as in the party (example 80): the server
  tells that player's client, which adds it to the input it predicts with.
- **Gravity is everyone's.** The server sets it and says so in every state message; each client sets
  its own physics' to match, so its prediction of its own player stays right.
- **Putting it back** destroys every piece, thing and block, and edits each structure back to the
  voxels it was loaded with (palette slots and all), then makes the crates and TNT again.
- **The pieces are budgeted**: 1,500 at most (the oldest go), islands of four voxels or fewer are dust,
  a strike's shards share a collision family for their first quarter second, and small debris is
  walked through (the "small debris" group). Thrown things are capped at 160.

## Checking it

`selftest.sh` runs a server and two headless clients over localhost; each walks to the castle's gate
and uses every tool on the south wall in turn, one switches the gravity and the other resets the
world part way through. Each checks that it joined, that the world changed, and that every change it
was sent matches the server's. `ctest --preset net` runs it.

The scene folder (`playground/`) is generated: `playground_demo_server --write-assets <dir>` writes it from
`world.h`, and the server and every client must have the same files.
