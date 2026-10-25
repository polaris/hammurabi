#ifndef HAMMURABI_SIM_NETWORK_H
#define HAMMURABI_SIM_NETWORK_H

#include "sim/random.h"
#include "sim/types.h"

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace hammurabi::sim {

template<typename P>
concept routable = std::copyable<P> && std::same_as<decltype(P::from), decltype(P::to)> &&
                   std::totally_ordered<decltype(P::from)>; // std::set<address> needs operator<

template<routable Packet>
class network {
public:
    using address = decltype(Packet::from);

    explicit network(std::uint64_t seed);
    void send(Packet packet, tick_t now); // all faults are decided here, not on delivery
    [[nodiscard]] std::vector<Packet> deliver(tick_t now);

    void set_loss(int per_mille);
    void set_duplication(int per_mille);
    void set_delay(tick_t min, tick_t max);
    // Packets only flow inside a group; nodes not listed form one more group.
    // Replaces any previous partition or isolation.
    void partition(const std::vector<std::set<address>>& groups);
    void isolate(address id); // cuts id off from everyone, on top of the current partition
    void heal();

private:
    static constexpr tick_t absolute_min_delay = 1;

    struct in_flight {
        tick_t due;
        std::uint64_t seq;
        Packet packet;
    };
    std::vector<in_flight> queue_{};
    std::mt19937_64 rng_;
    std::uint64_t next_seq_ = 0;

    int loss_per_mille_ = 0;
    int duplication_per_mille_ = 0;
    tick_t min_delay_ = absolute_min_delay;
    tick_t max_delay_ = absolute_min_delay;
    std::map<address, std::size_t> group_of_;
    std::size_t next_group_ = 0;

    [[nodiscard]] bool reachable(address from, address to) const;
    [[nodiscard]] tick_t random_delay() { return random_between(rng_, min_delay_, max_delay_); }
};

template<routable Packet>
void network<Packet>::set_loss(int per_mille) {
    assert(min_per_mille <= per_mille && per_mille <= max_per_mille);
    loss_per_mille_ = per_mille;
}

template<routable Packet>
void network<Packet>::set_duplication(int per_mille) {
    assert(min_per_mille <= per_mille && per_mille <= max_per_mille);
    duplication_per_mille_ = per_mille;
}

template<routable Packet>
void network<Packet>::set_delay(tick_t min, tick_t max) {
    assert(absolute_min_delay <= min && min <= max);
    min_delay_ = min;
    max_delay_ = max;
}

template<routable Packet>
void network<Packet>::partition(const std::vector<std::set<address>>& groups) {
    heal();
    for (std::size_t i = 0; i < groups.size(); ++i) {
        for (const address addr : groups[i]) {
            [[maybe_unused]] auto [it, inserted] = group_of_.try_emplace(addr, i);
            assert(inserted);
        }
    }
    next_group_ = groups.size();
}

template<routable Packet>
void network<Packet>::isolate(address id) {
    group_of_[id] = next_group_++;
}

template<routable Packet>
void network<Packet>::heal() {
    group_of_.clear();
    next_group_ = 0;
}

template<routable Packet>
bool network<Packet>::reachable(address from, address to) const {
    auto group = [this](address a) -> std::optional<std::size_t> {
        auto it = group_of_.find(a);
        return it == group_of_.end() ? std::nullopt : std::optional{it->second};
    };
    return group(from) == group(to); // nullopt == nullopt -> both implicitly in the same group -> reachable
}

template<routable Packet>
network<Packet>::network(std::uint64_t seed) : rng_(seed) {}

template<routable Packet>
void network<Packet>::send(Packet packet, tick_t now) {
    if (!reachable(packet.from, packet.to) || chance(rng_, loss_per_mille_)) {
        return;
    }
    queue_.push_back(in_flight{
        .due = now + random_delay(),
        .seq = next_seq_++,
        .packet = packet,
    });
    if (chance(rng_, duplication_per_mille_)) {
        queue_.push_back(in_flight{
            .due = now + random_delay(),
            .seq = next_seq_++,
            .packet = std::move(packet),
        });
    }
}

template<routable Packet>
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
