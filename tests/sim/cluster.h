#ifndef HAMMURABI_SIM_CLUSTER_H
#define HAMMURABI_SIM_CLUSTER_H

#include "sim/invariant_checker.h"
#include "sim/network.h"

#include <hammurabi/raft/node.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>

namespace hammurabi::sim {

class cluster {
public:
    cluster(std::size_t size, std::uint64_t seed);
    // Neither copyable nor movable: nodes hold raw pointers into storages_, and tests never need to move a cluster.
    cluster(const cluster&) = delete;
    cluster& operator=(const cluster&) = delete;
    cluster(cluster&&) = delete;
    cluster& operator=(cluster&&) = delete;
    ~cluster() = default;

    void step();
    void run_for(tick_t ticks);
    [[nodiscard]] bool run_until(const std::function<bool()>& condition, tick_t limit);
    [[nodiscard]] std::optional<raft::node_id> leader() const;
    // Exactly one leader, everyone else a follower, all in the same term: rules out a stale leader.
    [[nodiscard]] bool has_stable_leader() const;
    raft::node& node(raft::node_id id);
    network<raft::envelope>& net();
    [[nodiscard]] tick_t now() const;
    [[nodiscard]] std::string history() const;
    [[nodiscard]] std::string describe() const; // role and term of every node, e.g. "1: follower t=7"

private:
    static constexpr std::size_t history_limit = 200;

    tick_t now_ = 0;
    std::mt19937_64 seeder_;
    network<raft::envelope> net_;
    // Declared before nodes_ so it is destroyed after them: each node holds a raw pointer to its storage.
    std::map<raft::node_id, std::unique_ptr<raft::memory_storage>> storages_;
    std::map<raft::node_id, std::unique_ptr<raft::node>> nodes_;
    std::deque<std::string> history_;
    invariant_checker invariant_checker_;

    void record(std::string line);
};

} // namespace hammurabi::sim

#endif // HAMMURABI_SIM_CLUSTER_H
