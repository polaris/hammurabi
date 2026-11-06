#include "sim/invariant_checker.h"

#include <hammurabi/raft/format.h>

#include <algorithm>
#include <format>

namespace hammurabi::sim {

void invariant_checker::observe_leader(raft::node_id id, raft::term_t term, std::span<const raft::log_entry> log,
                                       tick_t now) {
    const auto [it, inserted] = leader_of_term_.try_emplace(term, leadership{.id = id, .since = now});
    if (!inserted && it->second.id != id) {
        throw invariant_violation(std::format("Election Safety violated: node {} is leader in term {} at t={}, "
                                              "but node {} has been leader in term {} since t={}",
                                              id, term, now, it->second.id, term, it->second.since));
    }
    if (inserted) {
        check_leader_completeness(id, term, log);
    }
}

void invariant_checker::observe_committed(raft::node_id id,
                                          const std::vector<std::pair<raft::log_index, raft::log_entry>>& committed) {
    for (const auto& [index, entry] : committed) {
        const auto [it, inserted] = committed_.try_emplace(index, handout{.by = id, .entry = entry});
        if (!inserted && it->second.entry != entry) {
            throw invariant_violation(std::format("State Machine Safety violated: node {} hands out {} at index {}, "
                                                  "but node {} handed out {} there",
                                                  id, entry, index, it->second.by, it->second.entry));
        }
    }
}

void invariant_checker::check_log_matching(raft::node_id a, std::span<const raft::log_entry> a_log, raft::node_id b,
                                           std::span<const raft::log_entry> b_log) {
    // The highest index at which both logs hold an entry of the same term. The property is inductive, so comparing
    // everything up to this one index covers every lower matching pair as well.
    auto anchor = static_cast<raft::log_index>(std::min(a_log.size(), b_log.size()));
    while (anchor > 0 && a_log[anchor - 1].term != b_log[anchor - 1].term) {
        --anchor;
    }
    for (raft::log_index i = 1; i <= anchor; ++i) {
        if (a_log[i - 1] != b_log[i - 1]) {
            throw invariant_violation(std::format("Log Matching violated: nodes {} and {} both have term {} at index "
                                                  "{}, but differ at index {}: {} vs. {}",
                                                  a, b, a_log[anchor - 1].term, anchor, i, a_log[i - 1], b_log[i - 1]));
        }
    }
}

void invariant_checker::check_leader_completeness(raft::node_id id, raft::term_t term,
                                                  std::span<const raft::log_entry> log) const {
    for (const auto& [index, h] : committed_) {
        if (index > log.size()) {
            throw invariant_violation(std::format("Leader Completeness violated: node {} became leader in term {} "
                                                  "with {} entries, but node {} handed out index {} as committed",
                                                  id, term, log.size(), h.by, index));
        }
        if (log[index - 1] != h.entry) {
            throw invariant_violation(std::format("Leader Completeness violated: node {} became leader in term {} "
                                                  "with {} at index {}, but node {} handed out {} there",
                                                  id, term, log[index - 1], index, h.by, h.entry));
        }
    }
}

} // namespace hammurabi::sim
