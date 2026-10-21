#include "audio/return_audio.hpp"

#include "audio/opus_codec.hpp"

#include <chrono>
#include <cstdio>
#include <memory>

namespace azl {

namespace {

constexpr auto kPoll = std::chrono::milliseconds(10);

Message error(const std::string& text)
{
    return {{"type", "error"}, {"message", text}};
}

} // namespace

ReturnAudio::ReturnAudio(DocumentServer& document, AudioAnalysis& analysis) : document_(document), analysis_(analysis)
{
    document_.handle("listen", [this](const Message& message, const DocumentServer::Sink& reply) { handle(message, reply); });
    thread_ = std::thread([this] { run(); });
}

ReturnAudio::~ReturnAudio()
{
    document_.handle("listen", nullptr);
    {
        std::lock_guard lock(mutex_);
        running_ = false;
        analysis_.setTap(this, nullptr);
    }
    wake_.notify_all();
    thread_.join();
}

void ReturnAudio::handle(const Message& message, const DocumentServer::Sink& reply)
{
    const std::string op = message.value("op", "");
    const ClientId client = message.value("client", ClientId(0));
    if (op == "start") {
        try {
            OpusStreamEncoder probe(kOpusRate, kBitrate);
        } catch (const std::exception& e) {
            return reply(error(std::string("cannot listen: ") + e.what()));
        }
        listeners_.add(client);
        retap();
        reply({{"type", "listen"},
               {"op", "start"},
               {"codec", "opus"},
               {"sampleRate", kOpusRate},
               {"channels", 1},
               {"bitrate", kBitrate},
               {"frameMs", 20},
               {"listeners", listeners_.size()}});
    } else if (op == "stop") {
        listeners_.remove(client);
        retap();
        reply({{"type", "listen"}, {"op", "stop"}, {"listeners", listeners_.size()}});
    } else if (op == "status") {
        reply({{"type", "listen"}, {"op", "status"}, {"listeners", listeners_.size()}, {"bytes", bytes()}});
    } else {
        reply(error("unknown listen op \"" + op + "\""));
    }
}

void ReturnAudio::retap()
{
    {
        std::lock_guard lock(mutex_);
        if (!running_)
            return;
        if (listeners_.empty())
            analysis_.setTap(this, nullptr);
        else
            analysis_.setTap(this, [this](const float* samples, size_t count, double, double sampleRate) {
                heard(samples, count, sampleRate);
            });
    }
    wake_.notify_all();
}

void ReturnAudio::heard(const float* samples, size_t count, double sampleRate)
{
    if (chunks_->writable() == 0 || samples_->writable() < count) {
        dropped_.fetch_add(count, std::memory_order_relaxed);
        return;
    }
    samples_->write(samples, count);
    const Chunk chunk{uint32_t(count), sampleRate};
    chunks_->write(&chunk, 1);
}

void ReturnAudio::run()
{
    std::unique_ptr<OpusStreamEncoder> encoder;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<float> buffer(1 << 16);
    for (;;) {
        {
            std::unique_lock lock(mutex_);
            if (listeners_.empty())
                wake_.wait(lock, [this] { return !running_ || !listeners_.empty(); });
            else
                wake_.wait_for(lock, kPoll, [this] { return !running_; });
            if (!running_)
                return;
        }
        Chunk chunk;
        while (chunks_->read(&chunk, 1)) {
            samples_->read(buffer.data(), chunk.count);
            if (listeners_.empty())
                continue;
            if (!encoder || encoder->inputRate() != chunk.sampleRate) {
                try {
                    encoder = std::make_unique<OpusStreamEncoder>(chunk.sampleRate, kBitrate);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "return audio: %s at %.0f Hz\n", e.what(), chunk.sampleRate);
                    encoder.reset();
                    dropped_.fetch_add(chunk.count, std::memory_order_relaxed);
                    continue;
                }
            }
            for (std::vector<uint8_t>& packet : encoder->encode(buffer.data(), chunk.count)) {
                packets.push_back(std::move(packet));
                if (packets.size() == kPacketsPerMessage) {
                    send(packets);
                    packets.clear();
                }
            }
        }
        if (listeners_.empty()) {
            encoder.reset();
            packets.clear();
        }
    }
}

void ReturnAudio::send(const std::vector<std::vector<uint8_t>>& packets)
{
    std::vector<uint8_t> payload;
    std::vector<size_t> sizes;
    for (const std::vector<uint8_t>& packet : packets) {
        payload.insert(payload.end(), packet.begin(), packet.end());
        sizes.push_back(packet.size());
    }
    const Message message{{"type", "listen"},
                          {"op", "audio"},
                          {"seq", seq_++},
                          {"sizes", sizes},
                          {kReturnPayload, Message::binary(std::move(payload))}};
    if (!listeners_.send(document_, message))
        retap();
}

} // namespace azl
