#include "protocol/return_stream.hpp"

#include <vector>

namespace azl {

bool isReturnPacket(const Message& message)
{
    const auto payload = message.find(kReturnPayload);
    return payload != message.end() && payload->is_binary();
}

bool ReturnListeners::add(ClientId client)
{
    std::lock_guard lock(mutex_);
    return clients_.insert(client).second && clients_.size() == 1;
}

bool ReturnListeners::remove(ClientId client)
{
    std::lock_guard lock(mutex_);
    clients_.erase(client);
    return clients_.empty();
}

bool ReturnListeners::empty() const
{
    std::lock_guard lock(mutex_);
    return clients_.empty();
}

size_t ReturnListeners::size() const
{
    std::lock_guard lock(mutex_);
    return clients_.size();
}

bool ReturnListeners::send(DocumentServer& document, const Message& message)
{
    std::set<ClientId> clients;
    {
        std::lock_guard lock(mutex_);
        clients = clients_;
    }
    const uint64_t size = encode(message, Encoding::MessagePack).size();
    std::vector<ClientId> gone;
    for (ClientId client : clients) {
        if (document.send(client, message))
            bytes_.fetch_add(size, std::memory_order_relaxed);
        else
            gone.push_back(client);
    }
    std::lock_guard lock(mutex_);
    for (ClientId client : gone)
        clients_.erase(client);
    return !clients_.empty();
}

} // namespace azl
