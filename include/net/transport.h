#ifndef PROJV_NET_TRANSPORT_H
#define PROJV_NET_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Moving bytes between machines (docs/plans/networking.md). Two implementations of one interface:
// ENet over UDP (makeEnetTransport), and a network inside one process for tests (LoopbackNetwork),
// with latency, jitter, loss, duplication and reordering it is told to have.
//
//     auto server = net::makeEnetTransport();
//     server->listen(27015, 64);
//     ...each frame:
//     events.clear();
//     server->poll(events);                      // sends what was queued, gathers what arrived
//     for (const net::NetEvent& e : events) ...
//
// Two channels per connection. Reliable: everything arrives, once, in the order sent. Unreliable:
// may not arrive, and what arrives late after something newer is dropped -- for state where only the
// newest matters (body snapshots). Nothing here knows what the bytes mean (net/wire.h writes them).
//
// No EnTT, no runtime, and only src/net/transport_enet.cpp includes ENet (the `layering` test).
namespace projv::net {
    using PeerId = uint32_t;   // a connection, numbered by this end; 0 is none
    constexpr PeerId NO_PEER = 0;

    enum class Channel : uint8_t { Reliable = 0, Unreliable = 1 };

    struct NetEvent {
        enum class Kind : uint8_t { Connected, Disconnected, Received };
        Kind                 kind = Kind::Received;
        PeerId               peer = NO_PEER;
        Channel              channel = Channel::Reliable;
        std::vector<uint8_t> data;   // Received only
    };

    struct LinkStats {
        float    roundTripMs = 0.0f;
        float    loss = 0.0f;          // 0..1, as the transport measures it
        uint64_t bytesSent = 0, bytesReceived = 0;
    };

    class Transport {
    public:
        virtual ~Transport() = default;
        // Accepts up to `maxPeers` connections on `port`. False if the port could not be taken.
        virtual bool listen(uint16_t port, uint32_t maxPeers) = 0;
        // Starts connecting; Connected (or Disconnected, if it fails) arrives from poll. NO_PEER if
        // the address could not even be tried.
        virtual PeerId connect(const std::string& host, uint16_t port) = 0;
        // Queues bytes for the peer; sent at the next poll. False for a peer not connected.
        virtual bool send(PeerId peer, Channel channel, std::span<const uint8_t> data) = 0;
        // Closes the connection, gracefully; the peer is told.
        virtual void disconnect(PeerId peer) = 0;
        // Sends what is queued and appends what has happened since. Never blocks.
        virtual void poll(std::vector<NetEvent>& out) = 0;
        virtual LinkStats stats(PeerId peer) const = 0;
    };

    // ENet over UDP. Each Transport is one ENet host: one that listens is a server; one that connects
    // first is a client of one connection.
    std::unique_ptr<Transport> makeEnetTransport();

    // ---- A network in one process ----------------------------------------------------------------

    // Applied to every message, each way. Reliable messages are never lost, duplicated or reordered:
    // a loss delays one by a resend (a round trip) instead, as ENet's would be.
    struct LinkConditions {
        double latency = 0.0;     // s, one way
        double jitter = 0.0;      // s, added at random, 0..jitter
        double loss = 0.0;        // 0..1
        double duplicate = 0.0;   // 0..1, unreliable only
        double reorder = 0.0;     // 0..1: that share of unreliable messages is held back a further latency
    };

    // Endpoints that reach each other by port, on a clock this owns: nothing is delivered until
    // advance() passes the time it is due. Seeded, so a test's losses are the same every run.
    class LoopbackNetwork {
    public:
        explicit LoopbackNetwork(uint64_t seed = 1);
        ~LoopbackNetwork();
        LoopbackNetwork(const LoopbackNetwork&) = delete;
        LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;

        // A new host on this network. It must not outlive the network.
        std::unique_ptr<Transport> endpoint();
        void   setConditions(const LinkConditions& conditions);
        void   advance(double seconds);
        double now() const;

        struct Impl;
    private:
        std::shared_ptr<Impl> impl;
    };
}

#endif
