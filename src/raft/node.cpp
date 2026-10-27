#include "hammurabi/raft/messages.h"
#include "hammurabi/raft/types.h"

#include <hammurabi/raft/node.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <expected>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

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
            broadcast_append_entries();
        }
    } else if (++election_elapsed_ >= election_timeout_) {
        become_candidate();
    }
}

void node::receive(const envelope& env) {
    std::visit(
        [&](const auto& m) {
            if (m.term > hard_.term) {
                adopt_term(m.term); // Figure 2, "Rules for All Servers"
            }
            if (m.term < hard_.term) {
                reject_stale(env.from, m); // Figure 2: "Reply false if term < currentTerm"
                return;
            }
            handle(env.from, m); // from here on: m.term == hard_.term
        },
        env.msg);
}

void node::reject_stale(node_id from, const request_vote& /*m*/) {
    send(from, request_vote_reply{.term = hard_.term, .granted = false});
}

void node::reject_stale(node_id /*from*/, const request_vote_reply& /*m*/) {} // a reply from an old term: drop

void node::reject_stale(node_id from, const append_entries& /*m*/) {
    send(from, append_entries_reply{
                   .term = hard_.term,
                   .success = false,
                   .match_index = 0,
                   .last_log_index = last_log_index(),
               });
}

void node::reject_stale(node_id /*from*/, const append_entries_reply& /*m*/) {} // a reply from an old term: drop

void node::handle(node_id from, const request_vote& m) {
    // §5.4.1: only vote for a candidate whose log is at least as up to date as ours.
    // "Up to date" compares the last term first, then the last index.
    const bool log_ok =
        std::pair{m.last_log_term, m.last_log_index} >= std::pair{term_at(last_log_index()), last_log_index()};
    const bool can_vote = !hard_.voted_for || *hard_.voted_for == from; // "== from": a duplicate request
    const bool grant = can_vote && log_ok;
    if (grant) {
        hard_.voted_for = from;
        storage_->save_hard_state(hard_); // persist BEFORE the reply leaves (section 5.7)
        reset_election_timer();           // don't start a competing election right after voting
    }
    send(from, request_vote_reply{.term = hard_.term, .granted = grant});
}

void node::handle(node_id from, const request_vote_reply& m) {
    if (auto* cs = std::get_if<candidate_state>(&role_); cs != nullptr && m.granted) {
        cs->votes.insert(from);
        become_leader_if_won();
    }
}

void node::handle(node_id from, const append_entries& m) {
    assert(!std::holds_alternative<leader_state>(role_)); // Election Safety: two leaders in one term
    become_follower(from);                                // §5.2: also ends a candidacy in this term

    // 1. Consistency check: do we have the entry the new entries follow?
    if (m.prev_log_index > last_log_index() || term_at(m.prev_log_index) != m.prev_log_term) {
        send(from, append_entries_reply{
                       .term = hard_.term,
                       .success = false,
                       .match_index = 0,
                       .last_log_index = last_log_index(),
                   });
        return;
    }

    // 2. Skip entries we already have. Stop at the first new or conflicting one.
    auto first_new = m.entries.begin();
    log_index index = m.prev_log_index + 1;
    while (first_new != m.entries.end() && index <= last_log_index() && term_at(index) == first_new->term) {
        ++first_new;
        ++index;
    }
    if (first_new != m.entries.end()) {
        if (index <= last_log_index()) {
            truncate_log_from(index); // conflict: drop it and everything after (persisted)
        }
        append_to_log(std::span{first_new, m.entries.end()}); // persisted, then added to log_
    }

    // 3. Commit what the leader has committed, but only as far as we have verified (step 4.3).
    const log_index last_verified = m.prev_log_index + m.entries.size();
    advance_commit_index(std::min(m.leader_commit, last_verified));

    send(from, append_entries_reply{
                   .term = hard_.term,
                   .success = true,
                   .match_index = last_verified,
                   .last_log_index = last_log_index(),
               });
}

void node::truncate_log_from(log_index index) {
    assert(index >= 1);
    storage_->truncate(index);
    log_.resize(index - 1);
}

void node::append_to_log(std::span<const log_entry> entries) {
    storage_->append(entries);
    log_.insert(log_.end(), entries.begin(), entries.end());
}

void node::advance_commit_index(log_index index) {
    if (index > commit_index_) {
        commit_index_ = index;
    }
}

void node::handle(node_id /*from*/, const append_entries_reply& /*m*/) {} // step 4.2

std::expected<log_index, not_leader> node::propose(std::vector<std::byte> command) {
    if (current_role() != role::leader) {
        return std::unexpected{not_leader{.leader_hint = leader_hint()}};
    }
    const log_entry e{.term = current_term(), .kind = entry_kind::command, .data = std::move(command)};
    append_to_log(std::span{&e, 1});
    broadcast_append_entries();
    return last_log_index();
}

void node::send_append_entries(node_id peer) {
    const auto next_log_index = std::get<leader_state>(role_).next_index.at(peer);
    assert(next_log_index <= last_log_index() + 1);
    const auto prev_log_index = next_log_index - 1;
    const auto entries = log().subspan(next_log_index - 1);
    message msg = append_entries{
        .term = current_term(),
        .prev_log_index = prev_log_index,
        .prev_log_term = term_at(prev_log_index),
        .entries = std::vector<log_entry>(entries.begin(), entries.end()),
        .leader_commit = commit_index_,
    };
    send(peer, std::move(msg));
}

void node::broadcast_append_entries() {
    for (const auto peer : cfg_.members) {
        if (peer == cfg_.self) {
            continue;
        }
        send_append_entries(peer);
    }
}

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

log_index node::commit_index() const {
    return commit_index_;
}

std::span<const log_entry> node::log() const {
    return log_;
}

void node::adopt_term(term_t term) {
    assert(term > hard_.term);
    hard_.term = term;
    hard_.voted_for = std::nullopt;
    storage_->save_hard_state(hard_);
    become_follower(std::nullopt); // the leader of the new term is not known yet
}

void node::become_follower(std::optional<node_id> leader) {
    reset_election_timer();
    role_ = follower_state{.leader = leader};
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
    broadcast_append_entries();
}

void node::reset_election_timer() {
    election_elapsed_ = 0;
    const auto span = static_cast<decltype(rng_)::result_type>(cfg_.election_timeout_ticks);
    election_timeout_ = cfg_.election_timeout_ticks + static_cast<int>(rng_() % span);
}

void node::send(node_id to, message msg) {
    out_.messages.push_back(envelope{.from = cfg_.self, .to = to, .msg = std::move(msg)});
}

term_t node::term_at(log_index index) const {
    return (index == 0) ? 0 : log_[index - 1].term;
}

} // namespace hammurabi::raft
