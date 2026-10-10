#ifndef HAMMURABI_RAFT_MESSAGES_H
#define HAMMURABI_RAFT_MESSAGES_H

#include <hammurabi/raft/types.h>

#include <concepts>
#include <variant>
#include <vector>

namespace hammurabi::raft {

struct request_vote {
    term_t term = 0;
    log_index last_log_index = 0;
    term_t last_log_term = 0;
    bool operator==(const request_vote&) const = default;
};

struct request_vote_reply {
    term_t term = 0;
    bool granted = false;
    bool operator==(const request_vote_reply&) const = default;
};

struct append_entries {
    term_t term = 0;
    log_index prev_log_index = 0;
    term_t prev_log_term = 0;
    std::vector<log_entry> entries{};
    log_index leader_commit = 0;
    bool operator==(const append_entries&) const = default;
};

struct append_entries_reply {
    term_t term = 0;
    bool success = false;
    log_index match_index = 0;    // on success: last index known to match the leader
    log_index last_log_index = 0; // on failure: helps the leader skip back faster
    bool operator==(const append_entries_reply&) const = default;
};

using message = std::variant<request_vote, request_vote_reply, append_entries, append_entries_reply>;

struct envelope {
    node_id from = 0;
    node_id to = 0;
    message msg{};
    bool operator==(const envelope&) const = default;
};

static_assert(std::equality_comparable<envelope>);

} // namespace hammurabi::raft

#endif // HAMMURABI_RAFT_MESSAGES_H
