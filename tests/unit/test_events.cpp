// Events: queued delivery at one point per frame (core/events.h).

#include "doctest/doctest.h"

#include <string>
#include <vector>

#include "core/events.h"

namespace {
    struct Ping { int value = 0; };
    struct Pong { int value = 0; };
}

TEST_CASE("send queues until the pump; the pump delivers in send order") {
    projv::Events events;
    std::vector<int> seen;
    events.on<Ping>([&seen](const Ping& p) { seen.push_back(p.value); });
    events.send(Ping{1});
    events.send(Ping{2});
    CHECK(seen.empty());
    CHECK(events.pendingCount<Ping>() == 2);
    events.pump();
    CHECK(seen == std::vector<int>{1, 2});
    CHECK(events.pendingCount<Ping>() == 0);
}

TEST_CASE("handlers run in the order they were connected") {
    projv::Events events;
    std::string order;
    events.on<Ping>([&order](const Ping&) { order += "a"; });
    events.on<Ping>([&order](const Ping&) { order += "b"; });
    events.send(Ping{});
    events.pump();
    CHECK(order == "ab");
}

TEST_CASE("anything sent during a pump waits for the next one, whatever its type") {
    // Both directions of type order, so the result cannot depend on which queue was created first.
    projv::Events events;
    std::vector<std::string> log;
    events.on<Ping>([&](const Ping& p) {
        log.push_back("ping" + std::to_string(p.value));
        events.send(Pong{p.value});
        events.send(Ping{p.value + 10});
    });
    events.on<Pong>([&](const Pong& p) {
        log.push_back("pong" + std::to_string(p.value));
    });

    events.send(Ping{1});
    events.pump();
    CHECK(log == std::vector<std::string>{"ping1"});
    events.pump();
    CHECK(log == std::vector<std::string>{"ping1", "ping11", "pong1"});
    CHECK(events.pendingCount<Ping>() == 1);   // ping21, waiting: a cycle advances once per pump
}

TEST_CASE("a type first sent from inside a handler is delivered at the next pump") {
    projv::Events events;
    struct Late { int value = 0; };
    std::vector<int> late;
    events.on<Ping>([&events](const Ping&) { events.send(Late{5}); });
    events.on<Late>([&late](const Late& l) { late.push_back(l.value); });
    events.send(Ping{});
    events.pump();
    CHECK(late.empty());
    events.pump();
    CHECK(late == std::vector<int>{5});
}

TEST_CASE("trigger delivers immediately") {
    projv::Events events;
    int seen = 0;
    events.on<Ping>([&seen](const Ping& p) { seen = p.value; });
    events.trigger(Ping{4});
    CHECK(seen == 4);
}

TEST_CASE("disconnect stops delivery, including from inside a handler") {
    projv::Events events;
    int a = 0, b = 0;
    projv::Events::Connection second = 0;
    events.on<Ping>([&](const Ping&) { a++; events.disconnect(second); });
    second = events.on<Ping>([&](const Ping&) { b++; });
    events.send(Ping{});
    events.send(Ping{});
    events.pump();
    CHECK(a == 2);
    CHECK(b == 0);   // disconnected by the handler before it, on the first event
}
