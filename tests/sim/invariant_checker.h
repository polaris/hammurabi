#ifndef HAMMURABI_SIM_INVARIANT_CHECKER_H
#define HAMMURABI_SIM_INVARIANT_CHECKER_H

#include "sim/types.h"

#include <hammurabi/raft/types.h>

#include <map>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace hammurabi::sim {

struct invariant_violation : std::logic_error {
    using std::logic_error::logic_error;
};

class invariant_checker {
public:
    // Called by the cluster after each step, for each node currently in the leader role. Checks Election Safety,
    // and Leader Completeness the first time a term's leader is seen.
    void observe_leader(raft::node_id id, raft::term_t term, std::span<const raft::log_entry> log, tick_t now);

    // Called with the entries a node hands out as committed. Checks State Machine Safety.
    void observe_committed(raft::node_id id, const std::vector<std::pair<raft::log_index, raft::log_entry>>& committed);

    // Checks Log Matching between the logs of nodes a and b.
    static void check_log_matching(raft::node_id a, std::span<const raft::log_entry> a_log, raft::node_id b,
                                   std::span<const raft::log_entry> b_log);

private:
    struct leadership {
        raft::node_id id;
        tick_t since;
    };

    void check_leader_completeness(raft::node_id id, raft::term_t term, std::span<const raft::log_entry> log) const;

    std::map<raft::term_t, leadership> leader_of_term_;

    struct handout {
        raft::node_id by{};
        raft::log_entry entry;
    };

    std::map<raft::log_index, handout> committed_;
};

} // namespace hammurabi::sim

#endif // HAMMURABI_SIM_INVARIANT_CHECKER_H
