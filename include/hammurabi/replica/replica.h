#ifndef HAMMURABI_REPLICA_H
#define HAMMURABI_REPLICA_H

#include <hammurabi/kv/protocol.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace hammurabi {

using request_id = std::uint64_t; // chosen by the host; identifies the waiting client

// Key and value bytes per request, so every command fits into one UDP datagram (section 5.6).
inline constexpr std::size_t max_payload_bytes = 1024;

struct replica_output {
    std::vector<raft::envelope> messages;
    std::vector<std::pair<request_id, kv::response>> responses;
};

class replica {
public:
    replica(raft::config cfg, raft::storage* store, std::map<raft::node_id, std::string> client_addresses);
    void tick();
    void receive(const raft::envelope& env);
    void submit(request_id id, const kv::request& req);
    [[nodiscard]] replica_output take_output();
    [[nodiscard]] const raft::node& node() const;
    [[nodiscard]] const kv::state_machine& machine() const;

private:
    struct pending {
        raft::term_t term;
        request_id id;
    };
    // node_ must stay declared before client_addresses_: the constructor checks the addresses while
    // building node_, and only then moves them into client_addresses_.
    raft::node node_;
    kv::state_machine machine_;
    std::map<raft::log_index, pending> pending_;
    std::map<raft::node_id, std::string> client_addresses_;
    replica_output out_;

    kv::response to_response(const std::variant<raft::not_leader, raft::command_too_large>& error) const;
};

} // namespace hammurabi

#endif // HAMMURABI_REPLICA_H
