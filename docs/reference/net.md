# net

Moving bytes between machines. Namespace `projv::net`. **No EnTT and nothing from the runtime**, and
only `src/net/transport_enet.cpp` includes ENet (the `layering` ctest). The session that uses it is
[`runtime/replication.h`](runtime.md#runtimereplicationh); the design is
[networking](../plans/networking.md).

Anything read off the wire is checked, never asserted on: a reader that runs out of bytes sets
`failed` and returns zeros, and the caller drops the message.

## `net/transport.h`

[`include/net/transport.h`](../../include/net/transport.h): one interface, two implementations.

| Name | What |
|---|---|
| `Transport` | `listen(port, maxPeers)`, `connect(host, port)`, `send(peer, channel, bytes)`, `disconnect`, `poll(events)`, `stats(peer)` |
| `makeEnetTransport()` | ENet over UDP; one `Transport` is one host |
| `LoopbackNetwork` | A network inside one process, on a clock it owns (`advance(seconds)`), seeded so a test's losses repeat |
| `LoopbackNetwork::endpoint()`, `setConditions(LinkConditions)` | A host on it; latency, jitter, loss, duplication, reordering |
| `Channel` | `Reliable` (everything, once, in order), `Unreliable` (the newest only: snapshots) |
| `NetEvent`, `PeerId`, `LinkStats` | `Connected`/`Disconnected`/`Received`; a connection; round trip, loss, bytes |

```cpp
auto server = net::makeEnetTransport();
server->listen(27015, 64);
std::vector<net::NetEvent> events;
server->poll(events);          // each frame: sends what was queued, gathers what arrived
```

## `net/wire.h`

[`include/net/wire.h`](../../include/net/wire.h): what goes on the wire, bit by bit.

| Name | What |
|---|---|
| `BitWriter`, `BitReader` | Bits, varints, floats, quantised values, byte runs; the reader's `failed` |
| `writePosition`, `writeRotation`/`readRotation`, `writeVector` | The quantisations bodies are sent with (millimetres, smallest-three rotations, limited velocities) |
| `writeLabels` | Run-length labels: a voxel split over a box |
