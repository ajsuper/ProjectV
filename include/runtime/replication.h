#ifndef PROJECTV_RUNTIME_REPLICATION_H
#define PROJECTV_RUNTIME_REPLICATION_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/application.h"
#include "core/math.h"
#include "core/world.h"
#include "net/transport.h"
#include "runtime/net.h"

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
// - **Where things are.** At `snapshotRate`, every replicated entity's pose (its body's or
//   character's in the simulation, its Transform otherwise), unreliably. A client poses its copies
//   (NetProxy) between the two snapshots around `interpolationDelay` behind the newest.
// - **Input.** A client's `setLocalInput` goes to the server every fixed step, unreliably, the last
//   few repeated in each message; the server hands each client's newest to `applyInput` (by default:
//   setCharacterInput on that client's player).
// - **Policy is the game's** (physics plan D8): who a player is (`spawnPlayer`), what input means
//   (`applyInput`), and what is sent (Replicated::omit).
//
// No prediction yet (N5): a client's own player is a proxy too. Main thread only.
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
        std::string sceneFolder;             // what clients must have the same of
        // Makes the joining client's player; NullEntity for none. It is given Replicated if it has
        // none, and destroyed when the client leaves.
        std::function<Entity(World&, ClientId)> spawnPlayer;
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
    };
    SessionStats sessionStats(const World& world);
}

#endif
