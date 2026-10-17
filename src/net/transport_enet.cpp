// The only file that includes ENet (the `layering` test enforces it).
#include "net/transport.h"

#include <map>
#include <mutex>

#include <enet/enet.h>

#include "core/log.h"

namespace projv::net {
    namespace {
        constexpr size_t CHANNELS = 2;   // Channel::Reliable, Channel::Unreliable

        // ENet is initialised once per process, while any host is alive.
        std::mutex initLock;
        int        users = 0;
        bool acquire() {
            std::lock_guard lock(initLock);
            if (users == 0 && enet_initialize() != 0) {
                core::error("net: ENet could not be initialised");
                return false;
            }
            users++;
            return true;
        }
        void release() {
            std::lock_guard lock(initLock);
            if (--users == 0) enet_deinitialize();
        }

        class EnetTransport final : public Transport {
        public:
            EnetTransport() : ready(acquire()) {}
            ~EnetTransport() override {
                if (host) {
                    // Tell everyone, and give the goodbyes a chance to leave.
                    for (auto& [id, peer] : peers) enet_peer_disconnect_now(peer, 0);
                    enet_host_flush(host);
                    enet_host_destroy(host);
                }
                if (ready) release();
            }

            bool listen(uint16_t port, uint32_t maxPeers) override {
                if (!ready || host) return false;
                ENetAddress address;
                address.host = ENET_HOST_ANY;
                address.port = port;
                host = enet_host_create(&address, maxPeers, CHANNELS, 0, 0);
                if (!host) core::warn("net: could not listen on port {}", port);
                return host != nullptr;
            }

            PeerId connect(const std::string& name, uint16_t port) override {
                if (!ready) return NO_PEER;
                if (!host) host = enet_host_create(nullptr, 1, CHANNELS, 0, 0);
                if (!host) return NO_PEER;
                ENetAddress address;
                if (enet_address_set_host(&address, name.c_str()) != 0) {
                    core::warn("net: could not resolve '{}'", name);
                    return NO_PEER;
                }
                address.port = port;
                ENetPeer* peer = enet_host_connect(host, &address, CHANNELS, 0);
                if (!peer) return NO_PEER;
                return adopt(peer);
            }

            bool send(PeerId id, Channel channel, std::span<const uint8_t> data) override {
                auto it = peers.find(id);
                if (it == peers.end() || it->second->state != ENET_PEER_STATE_CONNECTED) return false;
                // Unreliable is ENet's sequenced kind: one that arrives after a newer one is dropped.
                ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                                        channel == Channel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
                if (!packet) return false;
                if (enet_peer_send(it->second, uint8_t(channel), packet) != 0) {
                    enet_packet_destroy(packet);
                    return false;
                }
                sent[id] += data.size();
                return true;
            }

            void disconnect(PeerId id) override {
                auto it = peers.find(id);
                if (it != peers.end()) enet_peer_disconnect(it->second, 0);
            }

            void poll(std::vector<NetEvent>& out) override {
                if (!host) return;
                ENetEvent event;
                while (enet_host_service(host, &event, 0) > 0) {
                    NetEvent e;
                    switch (event.type) {
                        case ENET_EVENT_TYPE_CONNECT:
                            e.kind = NetEvent::Kind::Connected;
                            e.peer = adopt(event.peer);
                            out.push_back(std::move(e));
                            break;
                        case ENET_EVENT_TYPE_DISCONNECT:
                            e.kind = NetEvent::Kind::Disconnected;
                            e.peer = idOf(event.peer);
                            if (e.peer != NO_PEER) {
                                peers.erase(e.peer);
                                out.push_back(std::move(e));
                            }
                            event.peer->data = nullptr;
                            break;
                        case ENET_EVENT_TYPE_RECEIVE:
                            e.kind = NetEvent::Kind::Received;
                            e.peer = idOf(event.peer);
                            e.channel = event.channelID == 0 ? Channel::Reliable : Channel::Unreliable;
                            e.data.assign(event.packet->data, event.packet->data + event.packet->dataLength);
                            received[e.peer] += event.packet->dataLength;
                            enet_packet_destroy(event.packet);
                            if (e.peer != NO_PEER) out.push_back(std::move(e));
                            break;
                        case ENET_EVENT_TYPE_NONE:
                            break;
                    }
                }
                enet_host_flush(host);
            }

            LinkStats stats(PeerId id) const override {
                LinkStats s;
                auto it = peers.find(id);
                if (it == peers.end()) return s;
                s.roundTripMs = float(it->second->roundTripTime);
                s.loss = float(it->second->packetLoss) / float(ENET_PEER_PACKET_LOSS_SCALE);
                if (auto b = sent.find(id); b != sent.end()) s.bytesSent = b->second;
                if (auto b = received.find(id); b != received.end()) s.bytesReceived = b->second;
                return s;
            }

        private:
            // Our number for an ENet peer, kept in its user data.
            PeerId adopt(ENetPeer* peer) {
                if (PeerId known = idOf(peer); known != NO_PEER) return known;
                PeerId id = nextPeer++;
                peer->data = reinterpret_cast<void*>(uintptr_t(id));
                peers[id] = peer;
                return id;
            }
            static PeerId idOf(const ENetPeer* peer) { return PeerId(reinterpret_cast<uintptr_t>(peer->data)); }

            bool                         ready = false;
            ENetHost*                    host = nullptr;
            std::map<PeerId, ENetPeer*>  peers;
            std::map<PeerId, uint64_t>   sent, received;
            PeerId                       nextPeer = 1;
        };
    }

    std::unique_ptr<Transport> makeEnetTransport() { return std::make_unique<EnetTransport>(); }
}
