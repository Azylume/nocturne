#pragma once

#include "document/composition.hpp"
#include "protocol/messages.hpp"

#include <cstdint>
#include <optional>

namespace azl {

// A client's copy of the composition, kept current from a snapshot plus the ordered changes after it.
class DocumentMirror {
public:
    // Returns a snapshot request to send when a change is missing; changes are ignored until the snapshot arrives.
    // A change it cannot apply throws and leaves it unsynced, waiting for a snapshot the same way.
    std::optional<Message> receive(const Message& message);

    bool synced() const { return synced_; }
    uint64_t change() const { return change_; }
    const Composition& composition() const { return composition_; }

private:
    Composition composition_;
    uint64_t change_ = 0;
    bool synced_ = false;
};

} // namespace azl
