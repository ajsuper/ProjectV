# Networking: server-authoritative, on the deterministic simulation

Status: **approved**; **N0-N7 done** (2026-10-17): the plan is built; P0's second compiler waits on Clang being
installed (`--preset clang` is ready). Open: the debug overlay (N7), encryption, items under each milestone. Drafted and revised 2026-10-16 after the user's answers: ENet, a dedicated-server demo first,
hundreds of players as the eventual target, and clients that load and simulate only what is near them,
decided by one rule a game writes (`world-loading.md`). Builds on the physics plan (`physics-jolt.md`:
D6, D8, D9, A3, A7) and on the world-loading plan.

## Goals

1. **The world stays the same on every machine that can see it.** Voxel edits, breaks and splits
   reach every client that has that part of the world. A client that loads a part later gets it as it
   is now.
2. **A client loads and simulates only what is near its players.** The server has the whole world.
3. **Your own character responds at once.** It is predicted locally and corrected by the server.
4. **Everything else looks right, not exact.** Bodies drift a little between snapshots and are
   corrected without visible pops (A3: the sim pose snaps, the presentation glides).
5. **Scales toward hundreds of players.** A client's cost is set by what is near it, not by the map
   or the player count. The server's cost is set by what is awake.
6. **Mechanism in the engine, policy in systems (D8).** What a client loads and simulates is one
   rule function a game writes (the world-loading plan), run by the server for each client. The
   engine supplies the defaults, the loading, the transitions and the wire.
7. **Testable without a network:** server and clients in one process, over a loopback transport with
   seeded latency, loss, reordering and duplication.

Not goals: lockstep, rolling the world back (D9), peer-to-peer, matchmaking, NAT traversal, voice,
splitting one world across several server processes (kept possible, not built: see "Scale").

## Decisions

| # | Decision | Why |
|---|---|---|
| N1 | **Client-server, server-authoritative, dedicated server first.** A listen server (host plays) is a server plus a local client over loopback, later | D6. The demo is a server process and two separate client processes |
| N2 | **The server owns everything but each client's input.** Clients send input and game requests, never edits, breaks or body states | One owner for cheating and conflicts; D8's policy systems run on the server |
| N3 | **What a client has is the world-loading rule's answer, run by the server per client:** Active (simulated), Kept (a proxy following the server), Absent | One rule for single-player and online; clients do not simulate the rest of the map; the server decides, so a client cannot ask for what it should not see |
| N4 | **World changes are sent as results, not causes.** A break goes out as which voxels went to which new body (run-length coded), not as a strike to re-run | A client may have only part of a component loaded, and support and island passes need all of it. Results need no determinism to be right, and a client applies only the chunks it has |
| N5 | **Only the client's own character is predicted**, by re-running its input from the last acknowledged tick against the current world (character-only rewind) | D9: no world rollback. The character is `CharacterVirtual`, outside Jolt's body state, so it rewinds alone |
| N6 | **ENet, behind a transport interface**; only `src/net/transport_enet.cpp` includes it (layering ctest) | User's call. Up to 4,095 peers per host, reliable and unreliable channels. Same isolation rule as Jolt |
| N7 | **One clock: the physics tick.** Everything on the wire carries a tick; commands keep (tick, issuer, sequence) with issuer = client id | Already how PhysicsWorld orders commands (`PhysicsCommand`) |

## Loading per client (N3)

What a client has is decided by the world-loading rule (`world-loading.md`), the same rule a
single-player game uses. Online, **the server runs it for each client**, with that client's players
as the viewers:

```cpp
// On the server: one rule for every client, or one per client.
setClientLoadRule(world, loadNear(32.0f, 128.0f), LoadHint{140.0f});
setClientLoadRule(world, client, [](const LoadCandidate& c, std::span<const Viewer> v) { ... });
```

| The rule's answer for a client | The server sends | The client has |
|---|---|---|
| **Absent** | nothing; a despawn or block unload when it changes | nothing |
| **Kept** | the block (files' version + overlay), entities, snapshots | a frozen body following interpolated snapshots: a proxy |
| **Active** | the same, at higher snapshot priority | a simulated body, corrected by snapshots |

- **The client does not choose.** A client that picked its own blocks could ask for the whole map and
  see through walls. Its loader runs in "follow the server" mode: load what it is told, from its own
  copy of the scene files, plus the overlay the server sends.
- **The Kept ring is the boundary.** A simulated body near the edge hits proxies, which are where the
  server says. It never meets an empty space where the server has a wall of crates.
- **The server's own loading** is a separate rule, `loadEverything` by default. A server that unloads
  empty parts of the world is a later choice (see Scale).
- **Support and island passes run only on the server**, which has whole components. Clients get
  their results (N4), so a partly loaded component on a client never decides what falls.
- One rule replaces what the first draft had as three (zones, a relevance policy, region loading).
  Visibility rules a game might want -- teams, an owner-only inventory, line of sight -- are written
  in it, from tags and the game's own data.

## Time

```
server tick S ─────────────────────────────────────────▶
client world      S - buffer          (plays snapshots and world changes at their tick)
client character  S + lead            (inputs stamped for when the server will run them)
                  lead = RTT/2 + jitter margin, adapted from lateCommands / early-arrival counts
```

- **The client's world runs behind the server** by a jitter buffer (start at 3 ticks at 60 Hz, adapt
  to measured jitter). Snapshots and world changes apply when the client's world reaches their tick.
  Ones that arrive late apply at the next step and are counted.
- **The character runs ahead** by `lead`. The server tells each client how early its commands arrive,
  and the client speeds up or slows down its tick rate by up to 5% to stay in the window.
- **The cost (accepted, D9):** the character touches a world slightly in the past. A box you push
  responds when the server's answer comes back.

## What crosses the wire

| What | Channel | Client does |
|---|---|---|
| Handshake: protocol version, scene id and content hash | reliable | refuses to join on a mismatch |
| Block load: the overlay for that block if it has one (compressed), then its entities | reliable ordered | loads the block from its own files, applies the overlay, creates the entities |
| Entities: spawn, despawn, Presence change, component changes | reliable ordered | creates, updates or switches them by NetId, from component JSON (`ComponentTraits`) |
| World changes: edits, breaks, splits (N4) | reliable ordered | applies them to the chunks it has, at their tick |
| Body states | unreliable, latest wins | simulated: overwrites the sim state, the presentation eases; proxied: adds to the interpolation buffer |
| Notices: impacts, fractures (for sound, particles, camera shake) | unreliable | the game's systems; nothing in the world depends on them |
| Own character input | unreliable, redundant (last 8 ticks in every packet) | predicts; reconciles on each ack (N5) |
| Game messages (fire, use, build) | the game's choice | the game's systems; server-side validation (D8) |

### World changes as results (N4)

```
server: strike at tick 1200 on NetId 57 -> pieces 9000..9031, voxels removed from 57
  -> change { tick 1200, target 57,
              per changed chunk of 57: run-length labels (0 stays, k: went to piece k), or new content,
              pieces: [ {NetId 9000, components...}, ... ] }
client: at its tick 1200, for each chunk of 57 it has loaded:
          moves the labelled voxels out, into the pieces; skips the chunks it has not loaded
        pieces in its interest spawn; the rest are absent until they become relevant
```

- **The client runs no fracture patterns, support or island passes.** It has nothing to get wrong, and
  partial loading cannot change the answer. Determinism still matters for the bodies' motion, replays
  and tests.
- **Size:** labels run-length code well. A break is mostly long runs of "stays", and each shard is
  one run per row. It is sent only to clients with the chunks loaded. The plan measures demo 2's
  thirty blasts (N3 milestone) before choosing any further compression.
- **A plain edit** (`editVoxels`) goes as its ops, which are already results.
- **A voxel hash per changed component** goes too, in dev builds. A client that has the whole
  component checks it: a cheap desync test.
- **The debris budget** runs on the server; what it removes reaches clients as despawns.
- **Policy systems** (`ImpactFracture` and the like) run on the server only. Clients get notices for
  effects.

### Body snapshots

- **Quantised state:** position (relative to a 64 m cell, ~1 mm), rotation (smallest three, ~30 bits),
  linear and angular velocity. About 20 B per body.
- **Priority accumulator, per client:** each body in a client's interest gains priority each tick
  (simulated more than proxied, faster and nearer more, just hit most). Each packet takes the
  highest until it is full, then resets theirs. A body that falls asleep is sent once, then not
  again until it wakes.
- **Budget per client:** start at 64 kB/s, 30 packets/s (~2 kB, ~100 bodies per packet).
- **Delta against an acknowledged baseline:** after the first version works.
- **Applying one** to a simulated body sets its pose and velocity through a new logged op (a replay
  must see it), and the presentation eases toward the new sim pose over ~100 ms (A3). A proxied body
  interpolates between the two snapshots around `now - buffer`.

### Character prediction (N5)

```
client tick T: run input(T) on the character, keep (T, input, resulting state)
server ack:    "character state at tick A was X"
client:        if X differs from the kept state at A by more than ~1 mm:
                 restore character to X at A, re-run input(A+1 .. T) against the current world
```

- Other players' characters are proxies.
- What the predicted character pushed is the server's: re-running it must not push bodies again.

## Scale: toward hundreds of players

| Cost | Grows with | Kept down by |
|---|---|---|
| Client simulation | the client's Active bodies | the rule; a game shrinks its radius in a crowd |
| Client bandwidth | the client's Active and Kept bodies | the per-client budget and priority |
| Server simulation | awake bodies in the whole world | sleeping (Jolt does not step sleeping bodies); debris budget; a server load rule that keeps (freezes) what no player is near |
| Server snapshot building | players x relevant bodies | spatial grid for interest; bodies serialised once per tick, shared by every client that takes them |
| Server upload | players x budget | 300 players x 64 kB/s ~= 150 Mbit/s; tunable per game |

- **One server process, one PhysicsWorld** is the limit this plan builds to. Hundreds of players
  doing heavy destruction in one place will hit the server's step time first.
- **Not precluded:** NetIds are global and blocks are addressable. A world split across server
  processes by area would hand bodies between them as a client's Active and Kept sets already do.
  Not designed here.
- **ENet for hundreds:** one host, one socket, up to 4,095 peers. Fine for this scale; revisit only if
  profiling says so.

## Layering

```
include/net/transport.h        Connection, Channel {ReliableOrdered, Unreliable}, bytes in and out (no EnTT)
include/net/wire.h             bit writer/reader, quantisation, run-length labels (no EnTT; tested alone)
src/net/transport_enet.cpp     the only file that includes ENet
src/net/transport_loopback.cpp in-process, seeded latency / loss / reorder / duplication
runtime/replication.{h,cpp}    sessions, per-client load rules, snapshots, world changes, entities
```

- `net/` follows the `utils/` rule: no EnTT, no `runtime/`. The `layering` ctest gains it.
- Replication uses the public calls of physics and world loading, plus P-items below. It does not reach into `PhysicsWorld`.
- A server is an `Application` with no render systems. Nothing in the loop needs a window; to be
  checked at N2.
- ENet goes in `external/` as a submodule, pinned at a tagged release, like Jolt.

## Physics work this needs

Static colliders per block of cells, freezing, and support over partly loaded components are in the
world-loading plan (W2-W4). On top of those:

| # | What | Why |
|---|---|---|
| P0 | Golden recording on Clang (and ARM if available) | Clients simulate too; the same code must step the same on their compilers |
| P1 | A logged "set body state" op; a frozen body (W3) that follows given poses as a proxy | Snapshots; Kept bodies on a client |
| P2 | Character-only save, restore and update, without stepping the world | Prediction (N5). `saveState` saves characters today, but only with the whole world, and only `step()` updates them |
| P3 | Apply a world change from labels: move given voxels of a body into new pieces with given NetIds, without running patterns or support | N4 on the client |

## N0, done 2026-10-17

- **P0.** The golden session replays bit for bit in the `release` build (-O3, Jolt asserts off,
  minimal logging) as in `dev`. Clang: not installed on this machine; the `clang` preset
  (`build-clang`, tests only) is the one command once it is. ARM: none available.
- **P1.** `PhysicsWorld::setBodyState(id, position, rotation, linear, angular)`: one logged change
  (Op::SetState), wakes the body; a frozen body takes the pose and stays still. A frozen body
  (`setFrozen`, world-loading L3) follows `moveKinematic` poses and shoves what it meets: the proxy.
- **P2.** `saveCharacter` / `restoreCharacter` (refuses another character's bytes) /
  `stepCharacterAlone(id, dt)`: one character updated against the world as it is, pushing nothing --
  no contact impulses (a contact listener) and no weight on its ground (mass 0 for the update). All
  logged (LOG_VERSION 11). On open ground a re-run lands bit-exact where the first run did.
- **P3.** `runtime::applyVoxelSplit(world, entity, VoxelSplit)`: at the next step, in order with edits
  and strikes, the given voxels are cut into pieces with the given NetIds and motion, and the dust
  removed; no pattern, island or support pass, and the body's sources are taken as they are (so the
  step does not run one either). `voxelSplitOf(world, BodyBroken)` builds one on the server:
  BodyBroken now carries `pieceVoxels`, `pieceMotion` and `dust`. BreakCause::Replicated. The cut is
  `cutIslands`, shared with the island pass.
- Tests: `test_physics_prediction.cpp` (state as one change, through replay; a frozen proxy shoving a
  box; a character re-run near where it ended and the crate it pushed untouched -- checked to fail
  when the re-run pushes; exact on open ground); in `test_physics_fracture.cpp`, a server's strike
  applied to a client's wall makes the same wall and the same pieces by NetId, moving alike, and a
  split naming voxels the client lacks skips them.

## N1, done 2026-10-17

- `include/net/transport.h`: `Transport` (listen, connect, send on Reliable or Unreliable, disconnect,
  poll, stats), `makeEnetTransport()`, and `LoopbackNetwork` -- endpoints in one process on a clock
  the test advances, seeded, with `LinkConditions {latency, jitter, loss, duplicate, reorder}`. A
  lost reliable message is delayed by a resend, never lost or reordered; unreliable ones may be lost,
  duplicated or held back, and one older than the newest delivered is dropped (ENet's sequenced
  unreliable does the same).
- `include/net/wire.h`: `BitWriter`/`BitReader` (bits, varints, zigzag, f32, length-prefixed bytes,
  quantised floats); a reader past the end or past a caller's limit sets `failed` and returns zeros,
  never asserts. Positions as millimetre steps from a cell corner (signed varints), rotations smallest
  three (2 + 3 x 9 bits, under 0.6 degrees), vectors quantised; a body's pose and velocities in
  under 22 bytes near its corner. `writeLabels`/`readLabels`: runs, with cell and label limits.
- ENet v1.3.18 is a submodule (`external/enet`), built by its own CMake, or vcpkg's
  `unofficial-enet` first. Only `src/net/transport_enet.cpp` sees its headers (a source-file include
  path) and includes it (`layering`); `net/` is under the utils rule (no EnTT, no runtime).
- Tests (`test_net.cpp`): round trips field for field; 2,000 random buffers through every reader;
  quantisation within half a step; poses to the millimetre; labels and their limits; loopback
  handshake timing, 600 reliable messages each way in order through 20% loss at 150 ms, unreliable
  newest-only with loss and duplicates, the same seed losing the same messages, disconnects, a full
  server, nobody listening, an endpoint going away. ENet on localhost both ways: `ctest --preset net`
  (`net_sockets`, excluded from the default run like the soak).

## N2, done 2026-10-17

- `runtime/replication.h`: `installServer(app, transport, {port, maxClients, snapshotRate, sceneFolder,
  spawnPlayer, applyInput})`, `installClient(app, transport, {host, port, sceneFolder, name,
  interpolationDelay})`, `setLocalInput`, `sceneFilesHash`, `sessionStats`; events ClientJoined,
  ClientLeft (server), JoinedServer, LeftServer (client, with the reason).
- Handshake: Hello {PROTOCOL_VERSION, scene files hash, name} -> Welcome {client id, player NetId,
  tick length, tick} or Refuse {reason} then disconnect (sent first: ENet `disconnect_later`; the
  loopback delivers what was sent before a close).
- `Replicated {prefab, omit}` on the server: spawned to every welcomed client in NetId order (prefab
  path + pose + entityComponents less `omit`), and to a joining client all that exists; destroying
  it (or removing Replicated) despawns everywhere. A client instantiates the prefab from its own
  scene folder (paths with `..` refused), applies the components, and marks it `NetProxy`.
- Snapshots: every replicated entity's simulation pose (character, body, else Transform) at 30 Hz,
  unreliable, split under 1,100 bytes; raw floats and 12-bit-a-component rotations for now
  (quantisation and budgets are N4). Clients interpolate 100 ms behind their estimate of the
  server's tick.
- Input: each fixed step, the last 4 inputs (sequence, move, jump) unreliably; the server keeps the
  newest move and any jump not seen before; default `applyInput` is setCharacterInput.
- Example `70-multiplayer`: `mp_server` (no window), `mp_client` (window, or `--headless` with a
  scripted walk and checks), `selftest.sh` (three processes, localhost, random port; also
  `ctest --preset net` as `multiplayer_selftest`): both join, both see two players, Bo sees Ann walk
  9 m.
- Tests (`test_replication.cpp`, one process over the loopback network, no window): two clients
  join and see both players as prefabs without the controller; a walk on the server shows on both
  clients within 0.6 m while moving and exactly when stopped, voxels and all; a client that vanishes
  is despawned everywhere and a late joiner gets everyone; other scene files are refused with the
  reason; at 150 ms and 5% loss a walk still arrives.

## N3, done 2026-10-17

- **Physics.** `VoxelsEdited {entity, ops}`, triggered as an editVoxels is applied (in order with
  BodyBroken, which can follow from it). `applyVoxelEdit` applies a server's edit with no island or
  support pass (the body's sources are taken as they are). A split whose target has no body yet
  waits for it (up to 120 steps). `utils::voxelContentHash` (positions and slots, in order).
- **Server.** A BodyBroken of an entity with a NetId goes out as a Split: the pieces' NetIds,
  shard flags and motion, and labels over the box of every voxel cut (0 stays, k piece k, pieces +
  1 dust). VoxelsEdited goes as an Edit (its ops). Both are kept in the session's history, which a
  client joining later gets after its spawns. Pieces are `Replicated` and posed, never spawned:
  every client makes them from the split. A piece cleared (or destroyed) despawns everywhere, and
  the despawn joins the history. After each step's changes, a Check: each changed entity's voxel
  hash.
- **Client.** Splits, edits, despawns and checks queue in order and are applied by "net: world"
  (FixedUpdate, before "physics"); one naming what does not exist yet (a piece an earlier split
  makes) waits for it; a check waits for the step that applied what came before it, then counts
  `SessionStats::worldMismatches`. The client needs physics installed, and no voxel policy of its
  own (impact fracture).
- Tests (`test_replication.cpp`): a wall broken on the server leaves every client with the same wall
  (hash) and the same pieces by NetId and voxels, through 10% loss, and an edit after it likewise;
  a client joining after the break gets the rubble less a piece the server destroyed; the debris
  budget's clearing reaches clients; a client whose wall differs is caught by the check.
- Not here: the history only grows (compacting it into an overlay per block is for N6, with the
  world-loading overlay); the example does not break anything yet (game messages are N6).

## N4, done 2026-10-17

- **Physics.** `runtime::setBodyState(world, e, position, rotation, linear, angular, asleep)`: a
  command (PhysicsCommand::Kind::SetState, `c` the angular velocity), applied at the next step by
  `PhysicsWorld::setBodyState` (now with `asleep`: put to sleep there; LOG_VERSION 12, golden
  regenerated, replay unchanged). The simulation snaps; `PhysicsBody::easePosition/easeRotation`
  hold where it was drawn, fading with a 0.1 s time constant in "physics: present".
- **Server.** A snapshot may carry every replicated entity and every dynamic body with a NetId
  (authored ones clients have from files; pieces): NetId, moving and asleep bits, position in mm
  steps (signed varints), rotation smallest three at 10 bits, and for moving bodies velocities
  (12 bits each, 64 / 32 limits). Per client, priority accumulates per NetId until sent: 10 a
  snapshot for a player or anything only snapshots pose, 1 + speed / 2 for an awake body, 5 for one
  asleep and not yet told so; asleep and told, nothing. Packets fill in priority order under
  `ServerConfig::bytesPerSecond / snapshotRate` (64,000 / 30) and 1,100 bytes each.
- **Client.** A NetProxy keeps interpolating; a body it simulates (a dynamic RigidBody, including a
  spawned one, which now gets no NetProxy) takes the newest snapshot's state through setBodyState.
  `SessionStats::corrections`, server `snapshotBytes`.
- Tests: a ten-crate pyramid knocked over only on the server settles within 1 cm (and 0.01 rad)
  on both clients through 150 ms and 5% loss; twenty-one crates under a 6 kB/s budget stay within
  it, settle the same, and once asleep cost nothing but the player's pose; a 1 m correction snaps
  the body and eases the drawing over a few frames.
- Found: a test's player spawned inside the pyramid kept the whole stack awake -- a character
  standing in a pile wakes it every step. Expected Jolt behaviour, worth knowing for games.

## N5, done 2026-10-17

- **Physics.** `PhysicsWorld::setCharacterVelocity` (logged; LOG_VERSION 13, golden regenerated,
  replay unchanged). `runtime::hasPhysics(world)`.
- **Server.** A client's inputs wait by sequence (at most 60); one is applied a step, in order, once
  two have come; a gap is skipped; none waiting repeats the last move. Each snapshot to the owner
  starts with an ack: the last input applied, the character's position (0.1 mm steps) and velocity
  (20 bits a component) after it, and how many inputs wait. A client that said in its Hello it
  predicts is not sent its own player's pose (the ack is it), and is sent that entity whole when it
  spawns (Replicated::omit is for the others).
- **Client.** With physics, its own player (spawned with its CharacterController) is predicted:
  "net: input" (before "physics") reconciles with the newest ack, sets the step's input on the
  character and sends the inputs; "net: predicted" (after "physics") keeps the character's state
  after each input. A prediction off by 1 mm or 1 cm/s at the acked input is restored to the server's
  there (restoreCharacter, setCharacterPosition, setCharacterVelocity) and every input since run again
  with stepCharacterAlone; the drawn position moves with it. `Time::scale` runs 0.95..1.05 to keep
  two inputs waiting at the server. Without physics, the own player stays a proxy.
- Stats: `predicting`, `reconciliations`, `largestCorrection`.
- Example: mp_client installs physics and the arena's entities, so it predicts; the self-test reports
  both clients predicting, Ann's largest correction 1 mm over a 9 m walk.
- Tests: at 150 ms and 5% loss the own player moves two frames after the key, a walk with turns and
  a jump is never put right by 1 cm or more, ends within 1 mm of the server, where its inputs say;
  a 3 m move by the server is taken within a round trip, to the millimetre.
- Not here: other players are proxies, so walking into one is mispredicted until the correction
  (N4/N5 trade-off, D9). `Time::scale` also scales Update's delta by up to 5%.

## N6, done 2026-10-17

- **Server.** `ServerConfig::loadRule` (default loadEverything) and `setClientLoadRule(world[,
  client], policy)`: the world-loading rule, run per client with its player as the viewer. Blocks
  are numbered as a lazy open of the same files numbers them (a scratch SceneBlocks at install,
  with their bounds and content stamps). Every 10 frames (and at once on joining) each client's
  block presences are judged and the changes sent (BlockPresence); a block turning present whose
  stamp moved since the start is sent first, whole (BlockContent). Replicated entities (players,
  pieces) are judged at their position each frame and spawned or despawned per client; a client's
  own player always. Authored entities count as a client's while any block under their component
  is present for it. Snapshots, edits, splits and checks go only to the clients that have their
  entity; a split's pieces are counted as had by the clients told of it, and spawned with their
  voxels (palette and voxel list) to any other when it comes near. N3's history is gone: block
  contents and piece spawns put a late joiner right.
- **Client.** With world loading (`hasWorldLoading`), its loader's rule is the server's table (no
  block until told); without, the scene is whole and only block contents apply. Block contents queue
  with the other world changes and are applied in order -- in place, or kept for the block's next
  load (`SceneBlocks::replaceContent`) -- and taken by physics without an island pass
  (`runtime::acceptVoxelChanges`). A piece spawned with voxels is built as a chunk of its own, linked,
  with its components and Piece.
- `utils::componentVoxels` (and the hash on it).
- Test: two players 200 m apart on a 320 m field of 400 blocks under loadNear(24, 48): each client has
  about 40 blocks, all within 60 m of its player, neither has the other, and only Bo has the crate
  by him, simulated; walking together at 8 m/s, Ann's floor loads ahead of her (she never drops
  below it), Bo appears to her within 1.5 m of where the server has him, they meet, and Bo's
  client has let the crate go. Every earlier session test passes on the new path (late joiners).
- Not here: the example's arena is small enough to send whole; per-client loading is a setting on
  the server (`loadRule`). The server judges every block for every client every 10 frames -- fine to
  a few thousand blocks and tens of clients; N7 measures it.

## N7, done 2026-10-17

- `runtime::clientStats(world)` (server): per client, round trip, loss, bytes both ways, snapshot
  bytes, inputs waiting, entities and blocks it has.
- `mp_server` logs each second its frame work time (average, worst; the pacing sleep excluded), bytes
  out and the worst round trip; `--max-clients` (default 128); players spawn on a sunflower.
- `mp_bots --count N`: N clients in one process, each its own Application and ENet connection, no
  physics or window, walking at random. `scale.sh` (ctest `multiplayer_scale`, `--preset net`): a
  server and 100 bots over localhost for 22 s.
- Measured (this machine, 100 clients, loadEverything on the small arena): server frames 1.1-1.3 ms
  average, 5.5-7.6 ms worst (under a 16.7 ms step), 3.9 MB/s out (about 39 kB/s a client, under the
  64 kB/s budget: each is sent all 100 players), worst round trip 11 ms; every bot ~49 snapshot
  packets a second.
- Test (`test_replication.cpp`): forty clients in one process each have all forty players, are sent
  between 5 and 21 kB/s against a 20 kB/s budget, and see forty players.
- Not here: **the debug overlay.** The engine draws no text (only the editor has ImGui), so stats are
  logged and in `clientStats`/`sessionStats`; an overlay waits on physics R13 or a text pass.
  Snapshot entries are built once per snapshot and shared by every client (as planned); per-client
  relevance judges every block for every client every 10 frames -- 400 blocks x 100 clients cost
  nothing measurable here, a spatial index is the step when worlds are larger.

## The demo made a game (2026-10-18)

- `runtime::sendToServer(world, bytes, reliable)` and `ServerConfig::onMessage`: game messages from a
  client, whose meaning and checking are the game's (the one channel N6's draft named).
- `70-multiplayer` became something to play: an arena of models (tower, wall, house, trees, crates)
  of breakable materials, iron balls thrown on a left click, blasts on a right, impact fracture on
  the server, and a renderer with sun shadows (two rays through one march call). The self-test now
  throws at the tower: both clients apply the same 12 changes, none differing from the server's.
- Found: snapshot entries were copied into each client's packet a bit at a time. With 100 clients
  in the busier arena that pushed server frames to 19 ms; entries are now byte-aligned and copied
  whole (`BitWriter::align`/`raw`, `BitReader::align`), and frames' worst is 12 ms. mp_server now
  logs simulation steps apart from frames (steps: 1.3 ms average, 2.7 ms worst at 100 clients).
- Found, in the example: a hash helper multiplying signed ints overflowed, and the optimiser turned
  the loop calling it into one that never ended (then ran out of memory). Unsigned now.

## Corrections that settle (2026-10-18, from playing the demo)

Played: too many corrections were visible, and a piece could loop -- fall on a client but not on the
server, be corrected, fall again -- before settling. Two causes, both reproduced in tests first:

- **Rest was nobody's on the client.** A body the server has asleep was told so once and put to sleep
  here, but the client's own physics could wake it (a neighbour a few centimetres off, a correction
  to the piece under it arriving a packet later), and the server, having said "asleep", said
  nothing more: the loop, or a permanent difference (a poked crate stayed 26 cm off). Now a body the
  server reports asleep is held (`runtime::holdBody`: frozen as Kept is, solid; `PhysicsHold`)
  until the server reports it moving, and a frozen body ignores impulses and velocities
  (`PhysicsWorld`): nothing local moves what the server has at rest. A held body is set again only
  if the server's pose differs by 1 mm or so.
- **Snapshots were applied as if current.** A moving body's snapshot is half a round trip old, so it
  always differed by its speed times that: a crate sliding at 4 m/s was set back on 20 of 30
  snapshots a second. Now the snapshot is brought forward by half the transport's round trip
  (velocity, angular velocity) and only acted on past 2 cm, about 2 degrees or 0.3 m/s; under 10
  corrections a second for the same slide.

Tests (`test_replication.cpp`, "corrections:"): after the server's pile settles, no client is
corrected for 3 s, and a local poke moves nothing; a steady slide is corrected fewer than 10 times
in a second at 150 ms.

## Milestones

| # | What | Done when |
|---|---|---|
| N0 | **P0-P3.** The physics groundwork | Golden passes on two compilers; unit tests for each |
| N1 | **Transport and wire.** The interface, loopback, ENet, framing, two channels, connect, disconnect, timeouts; bit packing, quantisation, run-length labels | Loopback at 20% loss and 150 ms delivers reliable messages in order; quantisation round-trips within its error |
| N2 | **The demo, whole world.** A headless server process and two client processes (`examples/70-multiplayer`: `mp_server`, `mp_client --connect`). Handshake, client ids, time sync, entity spawn and despawn, both players walking (proxies for each other), no prediction yet | Both windows show both players; a self-test script starts the server and two clients and checks their logs |
| N3 | **World changes.** Edits and breaks as results, the overlay on join, debris as despawns | Demo 9-style walls broken on the server match on both clients at 10% loss (dev hashes); a client joining after the break sees the rubble |
| N4 | **Body snapshots, Active and Kept.** Priority, budget, jitter buffer, interpolation, presentation easing | A pyramid knocked over settles within 1 cm on clients; bytes per second within budget |
| N5 | **Character prediction.** Input redundancy, lead, tick-rate adjustment, acks, reconciliation | Walking at 150 ms RTT, 5% loss: no visible correction on flat ground |
| N6 | **Per-client loading.** Needs world-loading L3. `setClientLoadRule`, the client loader in follow mode, block loads with overlays, Presence changes on the wire | Two players 500 m apart: each client has only its own blocks and simulates only its own bodies; walking toward each other loads and switches without a pop |
| N7 | **Scale.** Shared serialisation, per-client bandwidth stats, a debug overlay (with physics R13), a bot client | 100 headless bot clients on one machine, server tick within budget, stats shown |

Order notes: N0-N5 load the whole world on every machine, so the demo exists early and every later
milestone is seen in it. World loading L0-L3 can be built alongside N1-N5 (it touches none of the
same code) and must be done by N6. Physics roadmap items this pulls in: R13 (debug view) at N7, R12
(start-asleep stacks) if loading settled scenes is slow.

## Tests

- **Headless, one process:** a server `Application` and N client `Application`s over loopback,
  stepped together under a fixed seed. No sockets in the unit suite.
- **Network conditions as a parameter:** {0 ms, 0%}, {150 ms, 5%}, {300 ms, 20% with reordering
  and duplication}.
- **Measured divergence:** tests read each client's body error against the server (max and mean, in
  m), voxel hash mismatches, and which blocks and bodies each client has, and assert bounds.
- **Real sockets:** the demo's self-test (server plus two clients, as separate processes) and one
  localhost test behind a ctest label, as the soak is.

## Open questions

1. **Default `loadNear` radii online** (Active 32 m, Kept 128 m proposed). Measured at N6; a game sets
   its own.
2. **Encryption and authentication.** ENet has neither. Needed before anything is public; not before N7.
3. **Scenes the client does not have** (user-made levels): the server sending whole blocks, not only
   overlays. Block loads make it possible later.
