#include "core/events.h"

namespace projv {
    void Events::disconnect(Connection id) {
        for (QueueBase* queue : order) {
            if (queue->disconnect(id)) return;
        }
    }

    void Events::pump() {
        // Take every queue's pending events before delivering any, so anything a handler sends --
        // of its own type or another -- lands in a pending list that this pump has already passed.
        // Indexing rather than iterating: a handler may send an event of a type never seen before,
        // which appends to `order`.
        size_t queueCount = order.size();
        for (size_t i = 0; i < queueCount; i++) order[i]->takePending();
        for (size_t i = 0; i < queueCount; i++) order[i]->deliver();
    }
}
