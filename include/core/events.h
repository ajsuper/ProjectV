#ifndef PROJECTV_EVENTS_H
#define PROJECTV_EVENTS_H

#include <cstdint>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace projv {
    // Typed event queues, delivered once per frame at one point: the end of PostUpdate. An engine
    // resource -- `app.events()`, or `world.ctx().get<projv::Events>()`.
    //
    //     struct DoorOpened { Entity door; };
    //     events.on<DoorOpened>([](const DoorOpened& e) { ... });
    //     events.send(DoorOpened{door});          // delivered at the next pump
    //
    // The rules, and why:
    //  - send() queues. A system never has handlers run in the middle of its own body, and every
    //    handler sees every event sent anywhere in the frame.
    //  - Everything sent *during* a pump -- by a handler, of any type -- is delivered at the next
    //    pump, not this one. That bounds a pump, makes the order independent of which types were
    //    registered first, and turns an event cycle into one event per frame instead of a hang.
    //  - trigger() delivers immediately. It is for the rare engine signal that must not lag a frame;
    //    each use should say why.
    //  - Handlers run in the order they were connected.
    //
    // ProjectV's own rather than entt::dispatcher, which the plan first named. The dispatcher
    // delivers an event raised inside a handler in the *same* pump whenever that event's type is
    // published after the handler's own, so whether it lags a frame depends on registration order.
    // Its sinks also take only free or member functions, not lambdas.
    //
    // Main thread only.
    class Events {
    public:
        using Connection = uint64_t;

        Events() = default;
        Events(const Events&) = delete;
        Events& operator=(const Events&) = delete;
        Events(Events&&) = default;
        Events& operator=(Events&&) = default;

        template<typename T>
        Connection on(std::function<void(const T&)> handler) {
            Connection id = ++lastConnection;
            queue<T>().handlers.push_back({id, std::move(handler)});
            return id;
        }

        // Removes a handler wherever it is. Safe to call from inside a handler, including its own.
        void disconnect(Connection id);

        template<typename T>
        void send(T event) {
            queue<T>().pending.push_back(std::move(event));
        }

        template<typename T>
        void trigger(const T& event) {
            queue<T>().deliverOne(event);
        }

        // Delivers everything that was queued before the pump began.
        void pump();

        template<typename T>
        size_t pendingCount() const {
            auto it = queues.find(std::type_index(typeid(T)));
            return it == queues.end() ? 0 : it->second->pendingCount();
        }

    private:
        struct QueueBase {
            virtual ~QueueBase() = default;
            virtual void takePending() = 0;   // pending -> delivering
            virtual void deliver() = 0;       // deliver what was taken
            virtual bool disconnect(Connection id) = 0;
            virtual size_t pendingCount() const = 0;
        };

        template<typename T>
        struct Queue final : QueueBase {
            struct Handler { Connection id; std::function<void(const T&)> fn; };
            std::vector<Handler> handlers;
            std::vector<T> pending;
            std::vector<T> delivering;

            void takePending() override {
                delivering.clear();
                delivering.swap(pending);
            }
            void deliver() override {
                std::vector<T> batch;
                batch.swap(delivering);
                for (const T& event : batch) deliverOne(event);
            }
            void deliverOne(const T& event) {
                // A copy of the list: a handler may connect or disconnect handlers. One connected
                // now hears from the next event on; one disconnected now is skipped from here on.
                std::vector<Connection> ids;
                ids.reserve(handlers.size());
                for (const Handler& handler : handlers) ids.push_back(handler.id);
                for (Connection id : ids) {
                    for (const Handler& handler : handlers) {
                        if (handler.id != id) continue;
                        auto fn = handler.fn;   // the vector may grow under the call
                        fn(event);
                        break;
                    }
                }
            }
            bool disconnect(Connection id) override {
                for (auto it = handlers.begin(); it != handlers.end(); ++it) {
                    if (it->id == id) { handlers.erase(it); return true; }
                }
                return false;
            }
            size_t pendingCount() const override { return pending.size(); }
        };

        template<typename T>
        Queue<T>& queue() {
            auto key = std::type_index(typeid(T));
            auto it = queues.find(key);
            if (it == queues.end()) {
                it = queues.emplace(key, std::make_unique<Queue<T>>()).first;
                order.push_back(it->second.get());
            }
            return static_cast<Queue<T>&>(*it->second);
        }

        std::unordered_map<std::type_index, std::unique_ptr<QueueBase>> queues;
        std::vector<QueueBase*> order;   // first-use order, so a pump is deterministic
        Connection lastConnection = 0;
    };
}

#endif
