#include "net/transport.h"

#include <algorithm>
#include <map>
#include <random>
#include <set>

namespace projv::net {
    // ---- The network's state ----------------------------------------------------------------------

    struct LoopbackNetwork::Impl {
        struct Host;
        // One connection, from both ends. Side 0 connected to side 1.
        struct Link {
            Host*    host[2] = {nullptr, nullptr};
            PeerId   peer[2] = {NO_PEER, NO_PEER};   // the id each end knows it by
            bool     open[2] = {false, false};       // that end has seen Connected
            bool     closed = false;
            uint64_t sent[2] = {0, 0};               // unreliable sequence, by sending side
            uint64_t newest[2] = {0, 0};             // newest unreliable delivered, by receiving side
            double   reliableAt[2] = {0.0, 0.0};     // when the last reliable message to that side lands
            uint64_t bytesSent[2] = {0, 0}, bytesReceived[2] = {0, 0};
        };
        struct Host {
            Impl*                          net = nullptr;
            bool                           listening = false;
            uint16_t                       port = 0;
            uint32_t                       maxPeers = 0;
            PeerId                         nextPeer = 1;
            std::map<PeerId, Link*>        links;
            std::vector<NetEvent>          inbox;
        };
        // A message on its way.
        struct Flight {
            double   at;
            uint64_t order;     // ties broken by when sent
            Link*    link;
            int      to;        // the side it is for
            NetEvent event;     // peer filled in on arrival
            uint64_t sequence;  // unreliable only
        };

        double                              now = 0.0;
        std::mt19937_64                     random;
        LinkConditions                      conditions;
        std::set<Host*>                     hosts;
        std::vector<std::unique_ptr<Link>>  links;
        std::vector<Flight>                 flights;
        uint64_t                            order = 0;

        double uniform() { return std::uniform_real_distribution<double>(0.0, 1.0)(random); }
        double delay() { return conditions.latency + conditions.jitter * uniform(); }

        void post(Link* link, int to, NetEvent event, double at, uint64_t sequence = 0) {
            flights.push_back({at, order++, link, to, std::move(event), sequence});
        }

        void message(Link* link, int from, Channel channel, std::span<const uint8_t> data) {
            const int to = 1 - from;
            NetEvent e;
            e.kind = NetEvent::Kind::Received;
            e.channel = channel;
            e.data.assign(data.begin(), data.end());
            link->bytesSent[from] += data.size();
            if (channel == Channel::Reliable) {
                double at = now + delay();
                while (uniform() < conditions.loss) at += 2.0 * conditions.latency + conditions.jitter;   // resent
                at = std::max(at, link->reliableAt[to]);   // in order
                link->reliableAt[to] = at;
                post(link, to, std::move(e), at);
                return;
            }
            const uint64_t sequence = ++link->sent[from];
            if (uniform() < conditions.loss) return;
            double at = now + delay();
            if (uniform() < conditions.reorder) at += conditions.latency + conditions.jitter;
            if (uniform() < conditions.duplicate) post(link, to, e, at + conditions.jitter * uniform(), sequence);
            post(link, to, std::move(e), at, sequence);
        }

        // Ends a link: each end that had it open hears Disconnected (now for this end, after the
        // latency for the other).
        void close(Link* link, int by) {
            if (link->closed) return;
            link->closed = true;
            for (int side = 0; side < 2; side++) {
                Host* h = link->host[side];
                if (!h) continue;
                h->links.erase(link->peer[side]);
                NetEvent e;
                e.kind = NetEvent::Kind::Disconnected;
                e.peer = link->peer[side];
                if (side == by) h->inbox.push_back(e);
                else post(link, side, e, now + delay());
            }
        }

        void advance(double seconds) {
            now += std::max(seconds, 0.0);
            std::stable_sort(flights.begin(), flights.end(),
                             [](const Flight& a, const Flight& b) { return std::tie(a.at, a.order) < std::tie(b.at, b.order); });
            size_t done = 0;
            for (; done < flights.size() && flights[done].at <= now; done++) {
                Flight& f = flights[done];
                Host* h = f.link->host[f.to];
                if (!h) continue;   // that end is gone
                NetEvent e = std::move(f.event);
                e.peer = f.link->peer[f.to];
                if (e.kind == NetEvent::Kind::Connected) {
                    if (f.link->closed) continue;
                    f.link->open[f.to] = true;
                } else if (e.kind == NetEvent::Kind::Received) {
                    if (f.link->closed || !f.link->open[f.to]) continue;
                    if (e.channel == Channel::Unreliable) {
                        if (f.sequence <= f.link->newest[f.to]) continue;   // older than what came already
                        f.link->newest[f.to] = f.sequence;
                    }
                    f.link->bytesReceived[f.to] += e.data.size();
                } else if (e.kind == NetEvent::Kind::Disconnected) {
                    if (!f.link->open[f.to] && f.to == 1) continue;   // a server never saw it open
                }
                h->inbox.push_back(std::move(e));
            }
            flights.erase(flights.begin(), flights.begin() + long(done));
        }

        void forget(Host* h) {
            for (auto [peer, link] : std::map<PeerId, Link*>(h->links)) {
                int side = link->host[0] == h ? 0 : 1;
                close(link, side);
            }
            for (auto& link : links)
                for (int side = 0; side < 2; side++)
                    if (link->host[side] == h) link->host[side] = nullptr;
            hosts.erase(h);
        }
    };

    // ---- An endpoint ------------------------------------------------------------------------------

    namespace {
        class LoopbackTransport final : public Transport {
        public:
            LoopbackTransport(std::shared_ptr<LoopbackNetwork::Impl> net) : net(std::move(net)) {
                host.net = this->net.get();
                this->net->hosts.insert(&host);
            }
            ~LoopbackTransport() override { net->forget(&host); }

            bool listen(uint16_t port, uint32_t maxPeers) override {
                for (LoopbackNetwork::Impl::Host* h : net->hosts)
                    if (h != &host && h->listening && h->port == port) return false;
                host.listening = true;
                host.port = port;
                host.maxPeers = maxPeers;
                return true;
            }

            PeerId connect(const std::string& address, uint16_t port) override {
                using Link = LoopbackNetwork::Impl::Link;
                (void)address;   // one network: the port is the address
                net->links.push_back(std::make_unique<Link>());
                Link* link = net->links.back().get();
                link->host[0] = &host;
                link->peer[0] = host.nextPeer++;
                host.links[link->peer[0]] = link;
                LoopbackNetwork::Impl::Host* server = nullptr;
                for (LoopbackNetwork::Impl::Host* h : net->hosts)
                    if (h != &host && h->listening && h->port == port) server = h;
                if (!server || server->links.size() >= server->maxPeers) {
                    // Nobody there, or full: the attempt fails after a while.
                    link->closed = true;
                    host.links.erase(link->peer[0]);
                    NetEvent e;
                    e.kind = NetEvent::Kind::Disconnected;
                    net->post(link, 0, e, net->now + 2.0 * net->conditions.latency + 0.001);
                    return link->peer[0];
                }
                link->host[1] = server;
                link->peer[1] = server->nextPeer++;
                server->links[link->peer[1]] = link;
                // The handshake: the server hears of it after one trip, the client after two.
                NetEvent e;
                e.kind = NetEvent::Kind::Connected;
                const double there = net->now + net->delay();
                net->post(link, 1, e, there);
                net->post(link, 0, e, there + net->delay());
                return link->peer[0];
            }

            bool send(PeerId peer, Channel channel, std::span<const uint8_t> data) override {
                auto it = host.links.find(peer);
                if (it == host.links.end()) return false;
                const int from = it->second->host[0] == &host ? 0 : 1;
                if (!it->second->open[from] || it->second->closed) return false;
                net->message(it->second, from, channel, data);
                return true;
            }

            void disconnect(PeerId peer) override {
                auto it = host.links.find(peer);
                if (it == host.links.end()) return;
                net->close(it->second, it->second->host[0] == &host ? 0 : 1);
            }

            void poll(std::vector<NetEvent>& out) override {
                for (NetEvent& e : host.inbox) out.push_back(std::move(e));
                host.inbox.clear();
            }

            LinkStats stats(PeerId peer) const override {
                LinkStats s;
                auto it = host.links.find(peer);
                if (it == host.links.end()) return s;
                const int side = it->second->host[0] == &host ? 0 : 1;
                s.roundTripMs = float(2000.0 * net->conditions.latency);
                s.loss = float(net->conditions.loss);
                s.bytesSent = it->second->bytesSent[side];
                s.bytesReceived = it->second->bytesReceived[side];
                return s;
            }

        private:
            std::shared_ptr<LoopbackNetwork::Impl> net;
            LoopbackNetwork::Impl::Host host;
        };
    }

    LoopbackNetwork::LoopbackNetwork(uint64_t seed) : impl(std::make_shared<Impl>()) { impl->random.seed(seed); }
    LoopbackNetwork::~LoopbackNetwork() = default;

    std::unique_ptr<Transport> LoopbackNetwork::endpoint() { return std::make_unique<LoopbackTransport>(impl); }
    void LoopbackNetwork::setConditions(const LinkConditions& conditions) { impl->conditions = conditions; }
    void LoopbackNetwork::advance(double seconds) { impl->advance(seconds); }
    double LoopbackNetwork::now() const { return impl->now; }
}
