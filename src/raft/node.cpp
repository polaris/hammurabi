#include "hammurabi/raft/messages.h"
#include "hammurabi/raft/types.h"

#include <hammurabi/raft/node.h>

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

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

void node::receive(const envelope& env) {
    const term_t msg_term = std::visit([](const auto& m) { return m.term; }, env.msg);
    if (msg_term > hard_.term) {
        become_follower(msg_term, std::nullopt);
    }
    std::visit([&](const auto& m) { handle(env.from, m); }, env.msg);
}

void node::handle(node_id from, const request_vote& m) {
    // §5.4.1: only vote for a candidate whose log is at least as up to date as ours.
    // "Up to date" compares the last term first, then the last index.
    const bool log_ok =
        std::pair{m.last_log_term, m.last_log_index} >= std::pair{term_at(last_log_index()), last_log_index()};
    const bool can_vote = !hard_.voted_for || *hard_.voted_for == from; // "== from": a duplicate request
    const bool grant = m.term == hard_.term && can_vote && log_ok;
    if (grant) {
        hard_.voted_for = from;
        storage_->save_hard_state(hard_); // persist BEFORE the reply leaves (section 5.7)
        reset_election_timer();           // don't start a competing election right after voting
    }
    send(from, request_vote_reply{.term = hard_.term, .granted = grant});
}

void node::handle(node_id from, const request_vote_reply& m) {
    if (auto* cs = std::get_if<candidate_state>(&role_); cs != nullptr && m.term == hard_.term && m.granted) {
        cs->votes.insert(from);
        become_leader_if_won();
    }
}

void node::handle(node_id from, const append_entries& m) {
    assert(!(std::holds_alternative<leader_state>(role_) && m.term == current_term()));
    const bool success = m.term >= current_term();
    if (success) {
        become_follower(m.term, from);
    }
    send(from, append_entries_reply{.term = current_term(), .success = success});
}

void node::handle(node_id from, const append_entries_reply& m) {
    (void)from;
    (void)m;
}

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

std::optional<node_id> node::leader_hint() const {
    if (auto* fs = std::get_if<follower_state>(&role_); fs != nullptr) {
        return fs->leader;
    }
    return std::nullopt;
}

// log_index node::commit_index() const;
// std::span<const log_entry> node::log() const;

void node::become_follower(term_t term, std::optional<node_id> leader) {
    reset_election_timer();
    role_ = follower_state{.leader = leader};
    if (term > current_term()) {
        hard_.voted_for = std::nullopt;
        hard_.term = term;
        storage_->save_hard_state(hard_);
    }
}

void node::become_candidate() {
    reset_election_timer();
    role_ = candidate_state{.votes = {cfg_.self}};
    hard_.term += 1;
    hard_.voted_for = cfg_.self;
    storage_->save_hard_state(hard_);
    const auto lli = last_log_index();
    for (const auto peer : cfg_.members) {
        if (peer == cfg_.self) {
            continue;
        }
        const message msg = request_vote{.term = current_term(), .last_log_index = lli, .last_log_term = term_at(lli)};
        send(peer, msg);
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
    for (const auto peer : cfg_.members) {
        if (peer == cfg_.self) {
            continue;
        }
        const auto prev_log_index = std::get<leader_state>(role_).next_index.at(peer) - 1;
        const message msg = append_entries{
            .term = current_term(),
            .prev_log_index = prev_log_index,
            .prev_log_term = term_at(prev_log_index),
            .entries = {},
            .leader_commit = commit_index_,
        };
        send(peer, msg);
    }
}

term_t node::term_at(log_index index) const {
    return (index == 0) ? 0 : log_[index - 1].term;
}

} // namespace hammurabi::raft
