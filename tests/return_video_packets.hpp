#pragma once

#include "output/h264_stream.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

// Return video packets saved as test fixtures. Each packet: key (1 byte), config and data (each a little-endian 32-bit
// size, then the bytes).
namespace {

void writeBytes(std::ofstream& out, const std::vector<uint8_t>& bytes)
{
    const auto size = uint32_t(bytes.size());
    out.write(reinterpret_cast<const char*>(&size), 4);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

std::vector<uint8_t> readBytes(std::ifstream& in)
{
    uint32_t size = 0;
    in.read(reinterpret_cast<char*>(&size), 4);
    std::vector<uint8_t> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
    return bytes;
}

[[maybe_unused]] void writePacket(std::ofstream& out, const azl::return_video::Packet& packet)
{
    out.put(packet.key ? 1 : 0);
    writeBytes(out, packet.config);
    writeBytes(out, packet.data);
}

[[maybe_unused]] std::vector<azl::return_video::Packet> readPackets(const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    std::vector<azl::return_video::Packet> packets;
    for (char key; in.get(key);) {
        azl::return_video::Packet packet;
        packet.key = key != 0;
        packet.config = readBytes(in);
        packet.data = readBytes(in);
        if (in)
            packets.push_back(std::move(packet));
    }
    return packets;
}

} // namespace
