#include "sim/cluster.h"

#include "sim/invariant_checker.h"
#include "sim/types.h"

#include <hammurabi/raft/format.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

namespace hammurabi::sim {

namespace {

std::string_view role_name(raft::role r) {
    switch (r) {
        case raft::role::follower:
            return "follower";
        case raft::role::candidate:
            return "candidate";
        case raft::role::leader:
            return "leader";
    }
    std::unreachable();
}

} // namespace

cluster::cluster(std::size_t size, std::uint64_t seed) : seeder_{seed}, net_{seeder_()} {
    const auto members =
        std::views::iota(raft::node_id{1}, static_cast<raft::node_id>(size) + 1) | std::ranges::to<std::vector>();

    for (const auto id : members) {
        storages_.insert(std::make_pair(id, std::make_unique<raft::memory_storage>()));
        nodes_.insert(std::make_pair(id, std::make_unique<raft::node>(
                                             raft::config{
                                                 .self = id,
                                                 .members = members,
                                                 .random_seed = seeder_(),
                                             },
                                             storages_[id].get())));
    }
}

void cluster::step() {
    now_ += 1;
    const auto& packets = net_.deliver(now_);
    for (const auto& packet : packets) {
        record(std::format("t={} deliver {}", now_, packet));
        node(packet.to).receive(packet);
    }
    for (const auto& [id, n] : nodes_) {
        n->tick();
        auto output = n->take_output();
        for (auto& packet : output.messages) {
            record(std::format("t={} send    {}", now_, packet));
            net_.send(std::move(packet), now_);
        }
    }
    for (const auto& [id, n] : nodes_) {
        if (n->current_role() == raft::role::leader) {
            try {
                invariant_checker_.observe_leader(id, n->current_term(), now_);
            } catch (const invariant_violation& e) {
                throw invariant_violation{std::format("{}\n{}", e.what(), history())};
            }
        }
    }
}

void cluster::run_for(tick_t ticks) {
    for (tick_t i = 0; i < ticks; ++i) {
        step();
    }
}

bool cluster::run_until(const std::function<bool()>& condition, tick_t limit) {
    for (tick_t i = 0; i < limit; ++i) {
        step();
        if (condition()) {
            return true;
        }
    }
    return false;
}

std::optional<raft::node_id> cluster::leader() const {
    std::optional<raft::node_id> result;
    raft::term_t best_term = 0;
    for (const auto& [id, n] : nodes_) {
        if (n->current_role() == raft::role::leader && (!result || n->current_term() > best_term)) {
            result = id;
            best_term = n->current_term();
        }
    }
    return result;
}

bool cluster::has_stable_leader() const {
    const auto l = leader();
    if (!l) {
        return false;
    }
    const auto term = nodes_.at(*l)->current_term();
    return std::ranges::all_of(nodes_, [&](const auto& entry) {
        const auto& [id, n] = entry;
        return n->current_term() == term && (id == *l || n->current_role() == raft::role::follower);
    });
}

raft::node& cluster::node(raft::node_id id) {
    return *nodes_.at(id);
}

network<raft::envelope>& cluster::net() {
    return net_;
}

tick_t cluster::now() const {
    return now_;
}

std::string cluster::history() const {
    std::string result;
    for (const auto& s : history_) {
        result += s;
        result += "\n";
    }
    return result;
}

std::string cluster::describe() const {
    std::string result;
    for (const auto& [id, n] : nodes_) {
        result += std::format("{}: {} t={}\n", id, role_name(n->current_role()), n->current_term());
    }
    return result;
}

void cluster::record(std::string line) {
    history_.push_back(std::move(line));
    if (history_.size() > history_limit) {
        history_.pop_front();
    }
}

} // namespace hammurabi::sim
