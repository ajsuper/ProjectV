# 90 — Party (demo)

Five little games for three or four friends, a round each in turn, on arenas floating over a sea of
lava. Every round ranks the players (3, 2, 1 and 0 points); the most points after five rounds takes
the party. Bots fill the seats nobody has, so it plays alone too.

```bash
cd build/examples/party_demo && ./party_demo                    # host a party here and play in it
cd build/examples/party_demo && ./party_demo --host 10.0.0.5    # join someone's party, or a server
cd build/examples/party_demo_server && ./party_demo_server                    # a dedicated server, no window
```

When hosting: `--players 3` (seats, bots filling them; four at most), `--rounds 10`, and
`--mode tag|dash|climb|crumble|blueprint` to play one game only. `--port` on both sides; `--name` on a client.

## The games

They come in this order: Bomb Tag, Sky Dash, Sky Climb, Crumble, Blueprint.

| | |
|---|---|
| **Bomb Tag** | Someone holds a ticking bomb, and passes it by touching someone else (not straight back to who gave it you). When it goes off it takes its holder and the floor around them. Whoever holds it runs a little faster. Last one standing wins. |
| **Sky Dash** | A race over the lava: a strip of tiles that crumble under you (and grow back), a ring with two sweepers turning across it -- jump them -- two gaps too wide to jump, which you bridge with tiles (C lays one in front of you; anyone can use your bridge, and anyone can bomb it), and a narrow way with pushers sliding across it. A fall puts you back at your last checkpoint. First home wins; the round ends when all but one are home. |
| **Sky Climb** | Everyone on one plot of ground, gold marks on the posts at its corners twelve metres up. Build up to them: jump, and lay a cube under you at the top of the jump (in first person, look down and press C). Bomb the others' towers down from under them. The first to stand at the gold wins; otherwise the highest. |
| **Crumble** | Two floors of tiles, eight metres apart. A tile you step on turns orange, then red, and drops. Keep moving; three blocks let you patch a hole to save yourself. The last one standing wins. |
| **Blueprint** | A gold shape appears on the display in the middle; copy it onto your own pad, cube by cube. X takes away a cube you got wrong -- or one someone else got right, on their pad. The first exact copy wins; otherwise the closest. |

## Controls

Made for a laptop: the trackpad or the arrow keys look about, the rest is on the keyboard.

| | |
|---|---|
| **W/A/S/D** | walk, the way the camera faces |
| **Space** | jump |
| **trackpad** or **arrows** | turn the camera (up and down tilt it); Esc frees the cursor, a click takes it back |
| **V** | first person or third |
| **F** or click | shove whoever is in front of you |
| **C** or right click | lay a block: a tile into the gap in front, or a cube -- in front of you, or in first person where the crosshair points (a green ring marks the cell) |
| **X** | take away a cube: the one the crosshair is on, or the one in front of you |
| **R** (hold, let go) | throw a bomb -- the longer you hold, the further; a ring shows where it will land |

## How it works

- **Tiles drop by losing their anchor.** Each floor is one voxel component, a static collider that
  splits. Each tile is an island in it, held up by one voxel of an anchor material
  (`PhysicsMaterial::anchor`) under its middle. Take that voxel away (`editVoxels`) and the support
  pass finds the tile standing on nothing: it falls as a piece of its own, and the server's
  replication sends the edit and the fall to every client. Between rounds the floors are put back
  from the voxels the files have.
- **Laying a tile puts one back.** A gap in Sky Dash is a cell of its floor the files leave empty; laying
  a tile adds its voxels, anchor and all -- the anchor by its palette slot, so it carries the anchor
  material. Tiles that grow back are laid with their voxels' own slots from the files: added by
  colour, each would take a new palette slot, and a palette holds 255.
- **Cubes are built on plots.** Sky Climb's ground and Blueprint's pads and display are each one
  splitting component: an anchored platform and every cube built on it, on a grid of metre cells. A
  bomb removes the voxels within a metre of it (`voxelsInSphere`) and what it cuts loose falls -- a
  tower bombed at its foot comes down. The server reads back which cells still hold a cube.
- **The crosshair is the server's ray.** In first person a client sends where it looks (yaw, pitch);
  the server casts the same ray from the same eye and lays the cube against the face it hits, and the
  client casts it too to ring the cell it will go in.
- **The sweepers and pushers have no bodies.** They are posed by the server and drawn where it says;
  the server throws whoever they touch, the same way a shove does.
- **A shove is carried by the shoved player's own walking.** The engine's characters take their
  velocity from their input each step, so a push is input: the server tells the shoved player's
  client, which adds the push (wearing off) to what it walks with, so its prediction and the server
  agree. A bot's push is added on the server.
- **Everything the server decides reaches clients as messages** (`party.h`): how the round and every
  player stand, fifteen times a second; banners; bursts to draw; shoves.

## Checking it

`selftest.sh` runs a server and three headless clients over localhost, with a bot in the fourth
seat, twice: a match from the start (Bomb Tag, then Sky Dash), where each client wanders about,
shoves, lays blocks and throws bombs; then Blueprint alone, where each lays cubes by aiming, as first
person does. Each checks that it took a seat, saw the rounds begin, that every change to the world it
was sent matches the server's, and in Blueprint that its aimed cubes went in. `ctest --preset net`
runs it.
`PARTY_BOTS=1` on the server logs where each bot is and where it is going.

The scene folder (`party/`) is generated: `party_demo_server --write-assets <dir>` writes it from
`arena.h`, and the server and every client must have the same files.
