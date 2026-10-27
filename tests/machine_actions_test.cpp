#include "engine/machine_actions.hpp"

#include <doctest.h>

#include <optional>
#include <vector>

using azl::MachineActions;
using azl::Message;

namespace {

// Collects the replies an action sends, so a test can check the op or the error without a live engine.
struct Replies {
    std::vector<Message> messages;
    azl::DocumentServer::Sink sink()
    {
        return [this](const Message& m) { messages.push_back(m); };
    }
    const Message& only() const
    {
        REQUIRE(messages.size() == 1);
        return messages.front();
    }
};

}

TEST_CASE("a supervised engine stops on a machine stop and tells the supervisor")
{
    bool stopped = false;
    MachineActions actions({}, {}, [&] { stopped = true; });
    Replies replies;

    CHECK(actions.handle({{"type", "machine"}, {"op", "stop"}}, replies.sink()));
    CHECK(replies.only().value("op", "") == "stop");
    CHECK(stopped);
}

TEST_CASE("stop without a supervisor answers an error and does not act")
{
    MachineActions actions({}, {}, {}); // No Stop: the engine runs without a supervisor (e.g. spawned by the UI).
    Replies replies;

    CHECK(actions.handle({{"type", "machine"}, {"op", "stop"}}, replies.sink()));
    CHECK(replies.only().value("type", "") == "error");
    CHECK(replies.only().value("message", "").find("supervisor") != std::string::npos);
}

TEST_CASE("the Show lock refuses stop")
{
    bool stopped = false;
    MachineActions actions({}, {}, [&] { stopped = true; });
    Replies lockReply;
    actions.handle({{"type", "machine"}, {"op", "lock"}, {"on", true}}, lockReply.sink());

    Replies replies;
    CHECK(actions.handle({{"type", "machine"}, {"op", "stop"}}, replies.sink()));
    CHECK(replies.only().value("type", "") == "error");
    CHECK(replies.only().value("message", "").find("lock") != std::string::npos);
    CHECK_FALSE(stopped);
    CHECK(actions.refusal("Stop").has_value());
}
