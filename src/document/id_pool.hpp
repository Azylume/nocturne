#pragma once

#include "document/composition.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>

namespace azl {

// Hands out ids: a given one must be free, a zero one becomes the next free one.
class IdPool {
public:
    void reserve(uint64_t id)
    {
        used_.insert(id);
        next_ = std::max(next_, id + 1);
    }

    uint64_t take(uint64_t id, const char* what)
    {
        if (id == 0)
            id = next_;
        else if (used_.contains(id))
            throw ProtocolError(std::string(what) + " id " + std::to_string(id) + " is in use");
        reserve(id);
        return id;
    }

private:
    std::set<uint64_t> used_;
    uint64_t next_ = 1;
};

} // namespace azl
