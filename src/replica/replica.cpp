#include <hammurabi/kv/command.h>
#include <hammurabi/kv/protocol.h>
#include <hammurabi/kv/state_machine.h>
#include <hammurabi/overloaded.h>
#include <hammurabi/raft/node.h>
#include <hammurabi/raft/types.h>
#include <hammurabi/replica/replica.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace hammurabi {

namespace {

std::size_t payload_bytes(const kv::operation& op) {
    return std::visit(
        overloaded{
            [](const kv::get& o) { return o.key.size(); },
            [](const kv::put& o) { return o.key.size() + o.value.size(); },
            [](const kv::remove& o) { return o.key.size(); },
            [](const kv::cas& o) { return o.key.size() + (o.expected ? o.expected->size() : 0) + o.desired.size(); },
            [](const kv::register_client&) { return std::size_t{0}; },
        },
        op);
}

// The session rules of section 5.8. The decoder only checks shape, so these are checked here,
// before anything reaches the log.
bool follows_session_rules(const kv::request& req) {
    const bool sessionless =
        std::holds_alternative<kv::get>(req.op) || std::holds_alternative<kv::register_client>(req.op);
    if (req.client_id == 0) {
        return sessionless && req.sequence == 0;
    }
    // A real client never registers again, and numbers its requests from 1. Sequence 0 right after
    // registering would equal last_sequence and be answered as a "retry".
    return !std::holds_alternative<kv::register_client>(req.op) && req.sequence >= 1;
}

kv::response as_response(const std::expected<kv::reply, kv::rejection>& answer) {
    if (!answer) {
        return answer.error();
    }
    return std::visit([](const auto& r) -> kv::response { return r; }, *answer);
}

raft::config verify_client_addresses(raft::config cfg, const std::map<raft::node_id, std::string>& client_addresses) {
    auto ids = cfg.members;
    std::ranges::sort(ids);
    const bool same = std::ranges::equal(ids, client_addresses | std::views::keys);
    if (!same) {
        throw std::runtime_error(
            "Map of client addresses is inconsistent with the list of node IDs in the node configuration.");
    }
    return cfg;
}

} // namespace

replica::replica(raft::config cfg, raft::storage* store, std::map<raft::node_id, std::string> client_addresses)
: node_{verify_client_addresses(std::move(cfg), client_addresses), store}
, client_addresses_{std::move(client_addresses)} {}

void replica::tick() {
    node_.tick();
}

void replica::receive(const raft::envelope& env) {
    node_.receive(env);
}

void replica::submit(request_id id, const kv::request& req) {
    if (!follows_session_rules(req)) {
        out_.responses.emplace_back(id, kv::rejection::malformed);
        return;
    }
    if (payload_bytes(req.op) > max_payload_bytes) { // 1 KiB, section 5.6
        out_.responses.emplace_back(id, kv::rejection::too_large);
        return;
    }
    // The log format, not the client format: same fields, but no version byte.
    const auto index =
        node_.propose(kv::encode(kv::command{.client_id = req.client_id, .sequence = req.sequence, .op = req.op}));
    if (!index) {
        out_.responses.emplace_back(id, to_response(index.error()));
        return;
    }
    [[maybe_unused]] const auto [_, inserted] =
        pending_.try_emplace(*index, pending{.term = node_.current_term(), .id = id});
    assert(inserted);
}

replica_output replica::take_output() {
    auto raft_out = node_.take_output();
    out_.messages.insert(out_.messages.end(), std::make_move_iterator(raft_out.messages.begin()),
                         std::make_move_iterator(raft_out.messages.end()));

    const auto redirect = [this] { return to_response(raft::not_leader{.leader_hint = node_.leader_hint()}); };

    // Apply first, then flush: an entry can commit in the same batch in which this node steps down,
    // and its client deserves the real result.
    for (const auto& [index, entry] : raft_out.committed) {
        std::optional<kv::response> result; // stays empty for a no-op
        if (entry.kind == raft::entry_kind::command) {
            const auto cmd = kv::decode_command(entry.data);
            if (!cmd) {
                // Only submit() writes commands, so this is corruption (e.g. a damaged log file), not bad input.
                throw std::runtime_error(std::format("log entry {} is not a valid command", index));
            }
            result = as_response(machine_.apply(index, *cmd));
        }

        // Look up no-ops too: a new leader's no-op may have overwritten the entry a client waits for.
        const auto waiting = pending_.find(index);
        if (waiting == pending_.end()) {
            continue;
        }
        if (waiting->second.term == entry.term) {
            // Same term: this is the entry submit() proposed, since one leader per term writes each index only once.
            // submit() proposes commands only, so the entry was applied above and has a result.
            assert(result.has_value());
            out_.responses.emplace_back(waiting->second.id, *result);
        } else {
            out_.responses.emplace_back(waiting->second.id, redirect()); // overwritten by another leader
        }
        pending_.erase(waiting);
    }

    // No longer leader: whether a pending entry commits is now up to another leader, and its index
    // may never be reached. Send the client there now (a retry is safe with sessions, step 5.2).
    if (node_.current_role() != raft::role::leader) {
        for (const auto& [index, waiting] : pending_) {
            out_.responses.emplace_back(waiting.id, redirect());
        }
        pending_.clear();
    }

    return std::exchange(out_, {});
}

const raft::node& replica::node() const {
    return node_;
}

const kv::state_machine& replica::machine() const {
    return machine_;
}

kv::response replica::to_response(const std::variant<raft::not_leader, raft::command_too_large>& error) const {
    return std::visit(overloaded{
                          [this](const raft::not_leader& e) -> kv::response {
                              return kv::not_leader{.leader_address =
                                                        e.leader_hint.transform([this](raft::node_id leader) {
                                                            return client_addresses_.at(leader);
                                                        })};
                          },
                          [](const raft::command_too_large&) -> kv::response { return kv::rejection::too_large; },
                      },
                      error);
}

} // namespace hammurabi