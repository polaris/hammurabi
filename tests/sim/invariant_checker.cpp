#include "sim/invariant_checker.h"

#include <format>

namespace hammurabi::sim {

void invariant_checker::observe_leader(raft::node_id id, raft::term_t term, tick_t now) {
    const auto [it, inserted] = leader_of_term_.try_emplace(term, leadership{.id = id, .since = now});
    if (!inserted && it->second.id != id) {
        throw invariant_violation(std::format("Election Safety violated: node {} is leader in term {} at t={}, "
                                              "but node {} has been leader in term {} since t={}",
                                              id, term, now, it->second.id, term, it->second.since));
    }
}

} // namespace hammurabi::sim
