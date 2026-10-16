#include "hammurabi/raft/messages.h"
#include "hammurabi/raft/types.h"

#include <hammurabi/raft/node.h>

#include <cassert>
#include <utility>

namespace hammurabi::raft {

memory_storage::memory_storage(persistent_state initial) : state_{std::move(initial)} {}

persistent_state memory_storage::load() {
    return state_;
}

void memory_storage::save_hard_state(const hard_state& state) {
    state_.hard = state;
}

void memory_storage::append(std::span<const log_entry> entries) {
    state_.log.insert(state_.log.end(), entries.begin(), entries.end());
}

void memory_storage::truncate(log_index first_removed) {
    state_.log.resize(first_removed - 1);
}

node::node(config cfg, storage* store) : cfg_{std::move(cfg)}, storage_(store) {
    assert(cfg_.election_timeout_ticks > 0);
    assert(storage_ != nullptr);

    rng_.seed(cfg_.random_seed);
    const auto ps = storage_->load();
    hard_ = ps.hard;
    log_ = ps.log;
    reset_election_timer();
}

void node::tick() {
    if (std::holds_alternative<leader_state>(role_)) {
        if (++heartbeat_elapsed_ >= cfg_.heartbeat_interval_ticks) {
            heartbeat_elapsed_ = 0;
            broadcast_heartbeat();
        }
    } else if (++election_elapsed_ >= election_timeout_) {
        become_candidate();
    }
}

// void node::receive(const envelope& env) {

// }

// std::expected<log_index, not_leader> node::propose(std::vector<std::byte> command) {

// }

output node::take_output() {
    return std::exchange(out_, {});
}

role node::current_role() const {
    if (std::holds_alternative<leader_state>(role_)) {
        return role::leader;
    } else if (std::holds_alternative<follower_state>(role_)) {
        return role::follower;
    }
    return role::candidate;
}

term_t node::current_term() const {
    return hard_.term;
}

// std::optional<node_id> node::leader_hint() const;
// log_index node::commit_index() const;
// std::span<const log_entry> node::log() const;

void node::become_candidate() {
    reset_election_timer();
    role_ = candidate_state{.votes = {cfg_.self}};
    hard_.term += 1;
    hard_.voted_for = cfg_.self;
    storage_->save_hard_state(hard_);
    const auto lli = last_log_index();
    for (const auto node_id : cfg_.members) {
        if (node_id == cfg_.self) {
            continue;
        }
        const message msg = request_vote{.term = current_term(), .last_log_index = lli, .last_log_term = term_at(lli)};
        send(node_id, msg);
    }
    become_leader_if_won();
}

void node::become_leader_if_won() {
    if (const auto* cs = std::get_if<candidate_state>(&role_); cs != nullptr && cs->votes.size() >= majority()) {
        become_leader();
    }
}

void node::become_leader() {
    leader_state ls;
    for (const auto peer : cfg_.members) {
        if (peer == cfg_.self) {
            continue;
        }
        ls.next_index[peer] = last_log_index() + 1;
        ls.match_index[peer] = 0;
    }
    role_ = std::move(ls);
    heartbeat_elapsed_ = 0;
    broadcast_heartbeat();
}

void node::reset_election_timer() {
    election_elapsed_ = 0;
    const auto span = static_cast<decltype(rng_)::result_type>(cfg_.election_timeout_ticks);
    election_timeout_ = cfg_.election_timeout_ticks + static_cast<int>(rng_() % span);
}

void node::send(node_id to, message msg) {
    out_.messages.push_back(envelope{.from = cfg_.self, .to = to, .msg = std::move(msg)});
}

void node::broadcast_heartbeat() {
    for (const auto node_id : cfg_.members) {
        if (node_id == cfg_.self) {
            continue;
        }
        const auto prev_log_index = std::get<leader_state>(role_).next_index.at(node_id) - 1;
        const message msg = append_entries{
            .term = current_term(),
            .prev_log_index = prev_log_index,
            .prev_log_term = term_at(prev_log_index),
            .entries = {},
            .leader_commit = commit_index_,
        };
        send(node_id, msg);
    }
}

term_t node::term_at(log_index index) const {
    return (index == 0) ? 0 : log_[index - 1].term;
}

} // namespace hammurabi::raft
