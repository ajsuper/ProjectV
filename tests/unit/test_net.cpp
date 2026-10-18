// The network layer (net/, docs/plans/networking.md N1): bits on the wire, and transports. The
// loopback network runs in this process on a clock the test owns; one test uses real sockets on
// localhost and is skipped here -- ctest runs it as `net_sockets`.

#include "doctest/doctest.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>
#include <vector>

#include <glm/gtc/quaternion.hpp>

#include "net/transport.h"
#include "net/wire.h"

using namespace projv::net;
using projv::core::quat;
using projv::core::vec3;

// ---- Wire ----------------------------------------------------------------------------------------

TEST_CASE("wire: what is written is read back, field for field") {
    BitWriter w;
    w.bits(5, 3);
    w.boolean(true);
    w.varint(0);
    w.varint(300);
    w.varint(~uint64_t(0));
    w.signedVarint(-1);
    w.signedVarint(-123456789);
    w.f32(-2.5f);
    std::vector<uint8_t> blob{1, 2, 3, 250};
    w.bytes(blob);
    w.bits(0x1234, 13);

    BitReader r(w.data());
    CHECK(r.bits(3) == 5);
    CHECK(r.boolean());
    CHECK(r.varint() == 0);
    CHECK(r.varint() == 300);
    CHECK(r.varint() == ~uint64_t(0));
    CHECK(r.signedVarint() == -1);
    CHECK(r.signedVarint() == -123456789);
    CHECK(r.f32() == -2.5f);
    CHECK(r.bytes(16) == blob);
    CHECK(r.bits(13) == (0x1234 & 0x1FFF));
    CHECK_FALSE(r.failed);
    CHECK(r.bitsLeft() < 8);
}

TEST_CASE("wire: a reader past the end, or past a limit, fails without asserting") {
    BitWriter w;
    w.varint(1000);
    w.bits(7, 3);
    {
        BitReader r(w.data());
        r.varint();
        r.bits(3);
        CHECK(r.bits(16) == 0);   // past the end
        CHECK(r.failed);
        CHECK(r.varint() == 0);   // and nothing after
    }
    {
        BitWriter big;
        big.bytes(std::vector<uint8_t>(100, 9));
        BitReader r(big.data());
        CHECK(r.bytes(10).empty());   // longer than it may be
        CHECK(r.failed);
    }
    {
        // A length that claims more than there is.
        BitWriter lie;
        lie.varint(1u << 30);
        BitReader r(lie.data());
        CHECK(r.bytes(size_t(1) << 31).empty());
        CHECK(r.failed);
    }
    // Garbage through every reader: no crash, no assert, no huge allocation.
    std::mt19937 random(7);
    for (int round = 0; round < 2000; round++) {
        std::vector<uint8_t> junk(random() % 40);
        for (uint8_t& b : junk) b = uint8_t(random());
        BitReader r(junk);
        r.bytes(64);
        readLabels(r, 4096, 600);
        readRotation(r);
        readPosition(r, vec3(0), 0.001f);
        readVector(r, 100.0f, 12);
        r.f32();
    }
}

TEST_CASE("wire: quantised values come back within half a step") {
    std::mt19937 random(3);
    std::uniform_real_distribution<float> any(-50.0f, 50.0f);
    for (uint32_t bits : {4u, 9u, 12u, 16u}) {
        const float step = 100.0f / float((1u << bits) - 1);
        BitWriter w;
        std::vector<float> values;
        for (int i = 0; i < 200; i++) {
            values.push_back(any(random));
            w.quantized(values.back(), -50.0f, 50.0f, bits);
        }
        w.quantized(1e9f, -50.0f, 50.0f, bits);           // clamped
        w.quantized(std::nanf(""), -50.0f, 50.0f, bits);  // a NaN is the minimum, not a crash
        BitReader r(w.data());
        for (float v : values) CHECK(std::abs(r.quantized(-50.0f, 50.0f, bits) - v) <= step * 0.5f + 1e-4f);
        CHECK(r.quantized(-50.0f, 50.0f, bits) == 50.0f);
        CHECK(r.quantized(-50.0f, 50.0f, bits) == -50.0f);
    }
}

TEST_CASE("wire: positions to the millimetre, rotations in 29 bits") {
    std::mt19937 random(11);
    std::uniform_real_distribution<float> spread(-2000.0f, 2000.0f), unit(-1.0f, 1.0f);
    BitWriter w;
    std::vector<vec3> positions;
    std::vector<quat> rotations;
    const vec3 origin(1024.0f, 0.0f, -512.0f);
    for (int i = 0; i < 500; i++) {
        positions.push_back(vec3(spread(random), spread(random), spread(random)));
        quat q = glm::normalize(quat(unit(random), unit(random), unit(random), unit(random)));
        rotations.push_back(q);
        writePosition(w, positions.back(), origin, 0.001f);
        writeRotation(w, q);
    }
    BitReader r(w.data());
    float worstAngle = 0.0f;
    for (int i = 0; i < 500; i++) {
        vec3 p = readPosition(r, origin, 0.001f);
        CHECK(glm::length(p - positions[size_t(i)]) < 0.001f);
        quat q = readRotation(r);
        float d = std::min(1.0f, std::abs(glm::dot(q, rotations[size_t(i)])));
        worstAngle = std::max(worstAngle, 2.0f * std::acos(d));
    }
    CHECK_FALSE(r.failed);
    CHECK(worstAngle < 0.01f);   // under 0.6 degrees
    // About 20 bytes a body for a pose near its cell's corner.
    BitWriter one;
    writePosition(one, origin + vec3(12.3f, 4.5f, 30.1f), origin, 0.001f);
    writeRotation(one, rotations[0]);
    writeVector(one, vec3(1, 2, 3), 64.0f, 12);
    writeVector(one, vec3(0.1f, 0, 0), 32.0f, 12);
    CHECK(one.data().size() <= 22);
}

TEST_CASE("wire: labels go as runs, and a reader refuses more than it was allowed") {
    std::vector<uint32_t> labels(5000, 0);
    for (size_t i = 100; i < 140; i++) labels[i] = 1;
    for (size_t i = 3000; i < 3010; i++) labels[i] = 7;
    labels[4999] = 2;
    BitWriter w;
    writeLabels(w, labels);
    CHECK(w.data().size() < 24);
    {
        BitReader r(w.data());
        CHECK(readLabels(r, 5000, 7) == labels);
        CHECK_FALSE(r.failed);
    }
    {
        BitReader r(w.data());
        CHECK(readLabels(r, 4999, 7).empty());   // one cell too many
        CHECK(r.failed);
    }
    {
        BitReader r(w.data());
        CHECK(readLabels(r, 5000, 6).empty());   // a label too high
        CHECK(r.failed);
    }
}

// ---- Loopback --------------------------------------------------------------------------------------

namespace {
    struct Pair {
        LoopbackNetwork net;
        std::unique_ptr<Transport> server, client;
        PeerId atServer = NO_PEER, atClient = NO_PEER;
        std::vector<NetEvent> serverEvents, clientEvents;

        explicit Pair(LinkConditions conditions, uint64_t seed = 1) : net(seed) {
            net.setConditions(conditions);
            server = net.endpoint();
            client = net.endpoint();
            REQUIRE(server->listen(27015, 8));
            atClient = client->connect("loopback", 27015);
            REQUIRE(atClient != NO_PEER);
        }
        // Advances the clock in frames, gathering what each end hears.
        void run(double seconds, double frame = 1.0 / 60.0) {
            for (double t = 0; t < seconds; t += frame) {
                net.advance(frame);
                size_t s = serverEvents.size();
                server->poll(serverEvents);
                client->poll(clientEvents);
                for (size_t i = s; i < serverEvents.size(); i++)
                    if (serverEvents[i].kind == NetEvent::Kind::Connected) atServer = serverEvents[i].peer;
            }
        }
        static std::vector<uint32_t> numbers(const std::vector<NetEvent>& events, Channel channel) {
            std::vector<uint32_t> out;
            for (const NetEvent& e : events)
                if (e.kind == NetEvent::Kind::Received && e.channel == channel) {
                    BitReader r(e.data);
                    out.push_back(uint32_t(r.varint()));
                }
            return out;
        }
        static std::vector<uint8_t> number(uint32_t n) {
            BitWriter w;
            w.varint(n);
            return w.take();
        }
    };
}

TEST_CASE("loopback: connecting takes a trip to the server and one back") {
    Pair p({0.05});
    CHECK_FALSE(p.client->send(p.atClient, Channel::Reliable, Pair::number(1)));   // not yet
    p.run(0.06);
    CHECK(p.atServer != NO_PEER);
    CHECK(p.clientEvents.empty());
    p.run(0.06);
    REQUIRE(p.clientEvents.size() == 1);
    CHECK(p.clientEvents[0].kind == NetEvent::Kind::Connected);
    CHECK(p.client->send(p.atClient, Channel::Reliable, Pair::number(1)));
}

TEST_CASE("loopback: reliable messages arrive once each, in order, through 20% loss at 150 ms") {
    Pair p({0.15, 0.03, 0.2});
    p.run(0.5);
    REQUIRE(p.atServer != NO_PEER);
    for (uint32_t i = 0; i < 600; i++) {
        REQUIRE(p.client->send(p.atClient, Channel::Reliable, Pair::number(i)));
        REQUIRE(p.server->send(p.atServer, Channel::Reliable, Pair::number(10000 + i)));
        p.run(1.0 / 60.0);
    }
    p.run(3.0);
    std::vector<uint32_t> got = Pair::numbers(p.serverEvents, Channel::Reliable);
    REQUIRE(got.size() == 600);
    for (uint32_t i = 0; i < 600; i++) CHECK(got[i] == i);
    std::vector<uint32_t> back = Pair::numbers(p.clientEvents, Channel::Reliable);
    REQUIRE(back.size() == 600);
    CHECK(back.front() == 10000);
    CHECK(back.back() == 10599);
}

TEST_CASE("loopback: unreliable messages may be lost, never arrive twice, and an older one is dropped") {
    Pair p({0.1, 0.05, 0.2, 0.1, 0.2});
    p.run(0.5);
    for (uint32_t i = 0; i < 1000; i++) {
        p.client->send(p.atClient, Channel::Unreliable, Pair::number(i));
        p.run(1.0 / 60.0);
    }
    p.run(1.0);
    std::vector<uint32_t> got = Pair::numbers(p.serverEvents, Channel::Unreliable);
    for (size_t i = 1; i < got.size(); i++) REQUIRE(got[i] > got[i - 1]);   // newer only, no repeats
    CHECK(got.size() > 500);
    CHECK(got.size() < 820);
}

TEST_CASE("loopback: the same seed loses the same messages") {
    auto arrivals = [](uint64_t seed) {
        Pair p({0.08, 0.04, 0.3, 0.1, 0.3}, seed);
        p.run(0.5);
        for (uint32_t i = 0; i < 200; i++) {
            p.client->send(p.atClient, Channel::Unreliable, Pair::number(i));
            p.run(1.0 / 60.0);
        }
        p.run(1.0);
        return Pair::numbers(p.serverEvents, Channel::Unreliable);
    };
    CHECK(arrivals(5) == arrivals(5));
    CHECK(arrivals(5) != arrivals(6));
}

TEST_CASE("loopback: disconnects, a full server, nobody listening, and an end that goes away") {
    LoopbackNetwork net;
    net.setConditions({0.02});
    auto server = net.endpoint();
    REQUIRE(server->listen(4000, 1));
    CHECK_FALSE(net.endpoint()->listen(4000, 1));   // the port is taken
    auto a = net.endpoint(), b = net.endpoint(), c = net.endpoint();
    std::vector<NetEvent> sa, ea, eb, ec;
    auto run = [&](double seconds) {
        for (double t = 0; t < seconds; t += 0.01) {
            net.advance(0.01);
            server->poll(sa);
            a->poll(ea);
            if (b) b->poll(eb);
            c->poll(ec);
        }
    };
    PeerId toServer = a->connect("loopback", 4000);
    run(0.1);
    REQUIRE(ea.size() == 1);
    CHECK(ea[0].kind == NetEvent::Kind::Connected);

    b->connect("loopback", 4000);   // one peer only
    c->connect("loopback", 4001);   // nobody there
    run(0.1);
    REQUIRE(eb.size() == 1);
    CHECK(eb[0].kind == NetEvent::Kind::Disconnected);
    REQUIRE(ec.size() == 1);
    CHECK(ec[0].kind == NetEvent::Kind::Disconnected);

    a->disconnect(toServer);
    run(0.1);
    CHECK(ea.back().kind == NetEvent::Kind::Disconnected);
    CHECK(sa.back().kind == NetEvent::Kind::Disconnected);
    CHECK_FALSE(a->send(toServer, Channel::Reliable, Pair::number(1)));

    // A client that goes away is a disconnect at the server.
    b.reset();
    b = net.endpoint();
    eb.clear();
    b->connect("loopback", 4000);
    run(0.1);
    size_t before = sa.size();
    b.reset();
    run(0.1);
    REQUIRE(sa.size() == before + 1);
    CHECK(sa.back().kind == NetEvent::Kind::Disconnected);
}

// ---- ENet, on real sockets --------------------------------------------------------------------------

TEST_CASE("enet: a server and a client on localhost talk both ways" * doctest::skip()) {
    auto server = makeEnetTransport();
    uint16_t port = 0;
    for (uint16_t candidate = 47800; candidate < 47820 && !port; candidate++)
        if (server->listen(candidate, 4)) port = candidate;
    REQUIRE(port != 0);
    auto client = makeEnetTransport();
    PeerId toServer = client->connect("127.0.0.1", port);
    REQUIRE(toServer != NO_PEER);

    std::vector<NetEvent> se, ce;
    PeerId toClient = NO_PEER;
    bool connected = false;
    auto pump = [&](double seconds, auto until) {
        auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
        while (std::chrono::steady_clock::now() < end && !until()) {
            server->poll(se);
            client->poll(ce);
            for (const NetEvent& e : se) if (e.kind == NetEvent::Kind::Connected) toClient = e.peer;
            for (const NetEvent& e : ce) if (e.kind == NetEvent::Kind::Connected) connected = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    pump(3.0, [&] { return connected && toClient != NO_PEER; });
    REQUIRE(connected);
    REQUIRE(toClient != NO_PEER);

    for (uint32_t i = 0; i < 50; i++) {
        REQUIRE(client->send(toServer, Channel::Reliable, Pair::number(i)));
        REQUIRE(server->send(toClient, Channel::Unreliable, Pair::number(i)));
    }
    pump(3.0, [&] { return Pair::numbers(se, Channel::Reliable).size() == 50; });
    std::vector<uint32_t> got = Pair::numbers(se, Channel::Reliable);
    REQUIRE(got.size() == 50);
    for (uint32_t i = 0; i < 50; i++) CHECK(got[i] == i);
    CHECK_FALSE(Pair::numbers(ce, Channel::Unreliable).empty());
    CHECK(client->stats(toServer).bytesSent > 0);

    client->disconnect(toServer);
    bool gone = false;
    pump(3.0, [&] {
        for (const NetEvent& e : se) gone = gone || e.kind == NetEvent::Kind::Disconnected;
        return gone;
    });
    CHECK(gone);
}

TEST_CASE("loopback: what was sent before a disconnect still arrives, before the disconnect") {
    Pair p({0.05, 0.02});
    p.run(0.3);
    REQUIRE(p.atServer != NO_PEER);
    REQUIRE(p.server->send(p.atServer, Channel::Reliable, Pair::number(42)));
    p.server->disconnect(p.atServer);
    p.run(0.5);
    REQUIRE(p.clientEvents.size() >= 3);   // connected, the message, the disconnect
    CHECK(p.clientEvents[1].kind == NetEvent::Kind::Received);
    CHECK(Pair::numbers(p.clientEvents, Channel::Reliable) == std::vector<uint32_t>{42});
    CHECK(p.clientEvents.back().kind == NetEvent::Kind::Disconnected);
}
