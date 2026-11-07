#pragma once

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace azl {

// A sorted set of ids in one vector. clear() keeps the capacity, so a member IdSet that is rebuilt every frame stops
// allocating once it has held its largest frame. Not thread safe.
class IdSet {
public:
    IdSet() = default;
    IdSet(std::initializer_list<uint64_t> ids) { insert(ids); }

    bool contains(uint64_t id) const { return std::binary_search(ids_.begin(), ids_.end(), id); }

    void insert(uint64_t id)
    {
        const auto at = std::lower_bound(ids_.begin(), ids_.end(), id);
        if (at == ids_.end() || *at != id)
            ids_.insert(at, id);
    }

    void insert(std::initializer_list<uint64_t> ids)
    {
        for (const uint64_t id : ids)
            insert(id);
    }

    void clear() { ids_.clear(); }
    bool empty() const { return ids_.empty(); }
    auto begin() const { return ids_.begin(); }
    auto end() const { return ids_.end(); }

private:
    std::vector<uint64_t> ids_;
};

}  // namespace azl
