#ifndef HAMMURABI_SIM_INVARIANT_CHECKER_H
#define HAMMURABI_SIM_INVARIANT_CHECKER_H

#include "sim/types.h"

#include <hammurabi/raft/types.h>

#include <map>
#include <stdexcept>

namespace hammurabi::sim {

struct invariant_violation : std::logic_error {
    using std::logic_error::logic_error;
};

class invariant_checker {
public:
    // Called by the cluster after each step, for each node currently in the leader role.
    void observe_leader(raft::node_id id, raft::term_t term, tick_t now);

private:
    struct leadership {
        raft::node_id id;
        tick_t since;
    };

    std::map<raft::term_t, leadership> leader_of_term_;
};

} // namespace hammurabi::sim

#endif // HAMMURABI_SIM_INVARIANT_CHECKER_H
