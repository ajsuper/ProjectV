#ifndef PROJECTV_RUNTIME_REPLICATION_H
#define PROJECTV_RUNTIME_REPLICATION_H

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <memory>
#include <string>
#include <vector>

#include "core/application.h"
#include "core/math.h"
#include "core/world.h"
#include "net/transport.h"
#include "runtime/net.h"
#include "utils/scene_loading.h"

// Server and clients (docs/plans/networking.md, N2): a session over a net::Transport, entities the
// server makes reaching every client by NetId, and the clients' input reaching the server.
//
//     // the server: a headless Application with the scene, physics, and
//     runtime::installServer(app, net::makeEnetTransport(), {.port = 27015, .sceneFolder = "arena",
//                            .spawnPlayer = [](World& w, ClientId id) { ... return entity; }});
//     // a client:
//     runtime::installClient(app, net::makeEnetTransport(), {.host = "127.0.0.1", .port = 27015,
//                            .sceneFolder = "arena"});
//     runtime::setLocalInput(app.world, move, jump);   // each frame
//
// - **Handshake.** A client sends the protocol version and a hash of its scene folder's files; a
//   server with another version or other files refuses it, with the reason, and drops it.
// - **What replicates.** An entity on the server with `Replicated` is sent to every client: its
//   registered components as entities.json writes them (entityComponents, less `omit`), and the prefab
//   folder it was made from, which a client instantiates from its own copy of the scene. By NetId:
//   one is given if it has none. Destroying it, or taking Replicated off, despawns it everywhere.
// - **Where things are** (N4). At `snapshotRate`, unreliably, within `bytesPerSecond` per client:
//   replicated entities' poses and every moving body with a NetId's pose and velocities, quantised
//   (millimetres, 10-bit smallest-three rotations, 12-bit velocities). Each client's entities gather
//   priority until sent -- players most, moving bodies by speed; a body asleep is sent once, asleep,
//   then left alone until it wakes. A client poses its copies (NetProxy) between the two snapshots
//   around `interpolationDelay` behind the newest, and sets the bodies it simulates to the server's
//   state (setBodyState: the simulation snaps, the drawing eases).
// - **Input.** A client's `setLocalInput` goes to the server every fixed step, unreliably, the last
//   few repeated in each message; the server hands each client's newest to `applyInput` (by default:
//   setCharacterInput on that client's player).
// - **Policy is the game's** (physics plan D8): who a player is (`spawnPlayer`), what input means
//   (`applyInput`), and what is sent (Replicated::omit).
//
// - **World changes** (N3). A voxel edit (VoxelsEdited) or a break (BodyBroken) of an entity with a
//   NetId goes to every client as what happened -- the edit's ops; the split's voxels per piece
//   (applyVoxelSplit), as labels over a box -- in the order it happened, and is kept: a client that
//   joins later gets the lot after its spawns. Clients apply them at fixed steps, in order, without
//   island or support passes of their own; pieces come with the server's NetIds, so they are posed
//   by snapshots and despawned like anything else, never spawned. After each step's changes the
//   server sends a hash of each changed entity's voxels; a client that differs counts it
//   (SessionStats::worldMismatches). A client needs physics installed to apply them, and should not
//   install policy that changes voxels itself (impact fracture): that is the server's.
//
// - **Prediction** (N5). A client with physics walks its own player itself: it is sent that entity
//   whole (Replicated::omit is for everyone else), applies each fixed step's input at once, and keeps
//   what each input left the character as. The server applies one input a step, in order (one lost
//   is skipped; none waiting repeats the last), and in each snapshot to the owner acks the last input
//   applied with the character's position and velocity after it. A client whose prediction differs
//   (1 mm, 1 cm/s) goes back to the server's at that input and runs every input since again, the
//   character alone (PhysicsWorld::stepCharacterAlone: it pushes nothing twice). It runs its clock up
//   to 5% fast or slow to keep about two inputs waiting at the server. A client without physics
//   shows its own player as a proxy, a round trip late.
//
// Main thread only.
namespace projv {
    // On the server: send this entity to every client.
    struct Replicated {
        std::string prefab;                 // folder, relative to the scene folder; empty: none
        std::vector<std::string> omit;      // component keys not sent (a character's controller)
    };

    // On a client: an entity the server made, posed by its snapshots. Made and removed by the
    // client; never saved.
    struct NetProxy {
        struct Pose {
            uint64_t   tick;
            core::vec3 position;
            core::quat rotation;
        };
        std::vector<Pose> poses;   // newest last, a second's worth at most
    };
}

namespace projv::runtime {
    using ClientId = uint16_t;   // a client of this server: 1.. (0 is the server itself)

    constexpr uint32_t PROTOCOL_VERSION = 1;

    struct ServerConfig {
        uint16_t    port = 27015;
        uint32_t    maxClients = 32;
        float       snapshotRate = 30.0f;    // per second
        uint32_t    bytesPerSecond = 64000;  // snapshot budget per client
        std::string sceneFolder;             // what clients must have the same of
        // Makes the joining client's player; NullEntity for none. It is given Replicated if it has
        // none, and destroyed when the client leaves.
        std::function<Entity(World&, ClientId)> spawnPlayer;
        // What each client has (N6): the world-loading rule, run here for each client with its player
        // as the viewer. Default: everything.
        utils::LoadPolicy loadRule;
        // A client's game message (sendToServer): what it means is the game's to say, and to check --
        // it comes from a machine the server does not trust.
        std::function<void(World&, ClientId, Entity player, std::span<const uint8_t> message)> onMessage;
        // What a client's input does; default: setCharacterInput on its player.
        std::function<void(World&, ClientId, Entity player, core::vec3 move, bool jump)> applyInput;
    };

    struct ClientConfig {
        std::string host = "127.0.0.1";
        uint16_t    port = 27015;
        std::string sceneFolder;
        std::string name;
        float       interpolationDelay = 0.1f;   // s behind the newest snapshot
    };

    // Server events.
    struct ClientJoined { ClientId client; Entity player; std::string name; };
    struct ClientLeft   { ClientId client; };
    // Client events.
    struct JoinedServer { ClientId you; NetId player; };
    struct LeftServer   { std::string reason; };

    // Adds "net: receive" (PreUpdate), "net: input" (FixedUpdate, before "physics", when it is
    // installed) and "net: send" (PostUpdate). Listens at once; false if the port is taken.
    bool installServer(Application& app, std::unique_ptr<net::Transport> transport, ServerConfig config);
    // Connects at once; JoinedServer or LeftServer says how it went.
    bool installClient(Application& app, std::unique_ptr<net::Transport> transport, ClientConfig config);

    // The rule every client is judged by, or one client's own (an empty rule: back to everyone's).
    void setClientLoadRule(World& world, utils::LoadPolicy policy);
    void setClientLoadRule(World& world, ClientId client, utils::LoadPolicy policy);

    // A message for the server's game (ServerConfig::onMessage): fire, use, build. Reliable, in order,
    // unless asked otherwise. At most 64 KB. Dropped before the client is welcomed.
    void sendToServer(World& world, std::span<const uint8_t> message, bool reliable = true);

    // The client's input for its player, sent from the next fixed step on, until changed.
    void setLocalInput(World& world, core::vec3 move, bool jump = false);

    // A hash of every file under the folder (paths and contents, in path order): what the
    // handshake compares.
    uint64_t sceneFilesHash(const std::string& folder);

    struct SessionStats {
        bool     connected = false;          // a client: welcomed; a server: listening
        ClientId you = 0;                    // a client's own id
        NetId    player;                     // a client's own player
        size_t   clients = 0;                // a server's
        uint64_t serverTick = 0;             // a client's estimate; a server's own
        uint64_t snapshotsReceived = 0, messagesRefused = 0;
        std::string refusal;                 // why the server refused, or the client was refused
        uint64_t worldChanges = 0;           // a client's: edits and splits applied
        uint64_t worldMismatches = 0;        // a client's: changed voxels not the server's after them
        uint64_t corrections = 0;            // a client's: its bodies set to the server's state
        bool     predicting = false;         // a client's: walking its own player itself
        uint64_t reconciliations = 0;        // a client's: times its prediction was put right
        float    largestCorrection = 0.0f;   // a client's: the furthest it was put right, m
        uint64_t snapshotBytes = 0;          // a server's: the most snapshot bytes sent to one client
    };
    SessionStats sessionStats(const World& world);

    // A server's view of each client (N7), in client id order.
    struct ClientStats {
        ClientId    id = 0;
        std::string name;
        float       roundTripMs = 0.0f, loss = 0.0f;
        uint64_t    bytesSent = 0, bytesReceived = 0;   // all of it, both ways
        uint64_t    snapshotBytes = 0;                  // of what was sent, snapshots
        size_t      inputsWaiting = 0;
        size_t      entities = 0;                       // replicated entities it has
        size_t      blocks = 0;                         // blocks present for it
    };
    std::vector<ClientStats> clientStats(const World& world);
}

#endif
