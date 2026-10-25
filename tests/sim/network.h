#ifndef HAMMURABI_SIM_NETWORK_H
#define HAMMURABI_SIM_NETWORK_H

#include "sim/types.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace hammurabi::sim {

template<typename Packet>
class network {
public:
    explicit network(std::uint64_t seed);
    void send(Packet packet, tick_t now);
    [[nodiscard]] std::vector<Packet> deliver(tick_t now);

private:
    struct in_flight {
        tick_t due;
        std::uint64_t seq;
        Packet packet;
    };
    std::vector<in_flight> queue_{};
    std::mt19937_64 rng_;
    std::uint64_t next_seq_ = 0;
};

template<typename Packet>
network<Packet>::network(std::uint64_t seed) : rng_(seed) {}

template<typename Packet>
void network<Packet>::send(Packet packet, tick_t now) {
    queue_.push_back(in_flight{
        .due = now + 1,
        .seq = next_seq_++,
        .packet = std::move(packet),
    });
}

template<typename Packet>
std::vector<Packet> network<Packet>::deliver(tick_t now) {
    auto due = std::ranges::partition(queue_, [now](const in_flight& item) { return item.due > now; });
    std::ranges::sort(due, {}, [](const in_flight& item) { return std::pair{item.due, item.seq}; });
    std::vector<Packet> result;
    result.reserve(due.size());
    for (auto& item : due) {
        result.push_back(std::move(item.packet));
    }
    queue_.erase(due.begin(), due.end());
    return result;
}

} // namespace hammurabi::sim

#endif // HAMMURABI_SIM_NETWORK_H
