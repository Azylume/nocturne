#include "engine/id_set.hpp"

#include <doctest.h>

#include <vector>

TEST_CASE("an id set holds each id once, in order, and clear empties it")
{
    azl::IdSet ids{5, 1, 5, 3};
    ids.insert({3, 9, 0});
    CHECK(std::vector<uint64_t>(ids.begin(), ids.end()) == std::vector<uint64_t>{0, 1, 3, 5, 9});
    CHECK(ids.contains(3));
    CHECK_FALSE(ids.contains(4));
    ids.clear();
    CHECK(ids.empty());
    CHECK_FALSE(ids.contains(3));
}
