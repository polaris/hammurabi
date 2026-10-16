#ifndef HAMMURABI_RAFT_NODE_H
#define HAMMURABI_RAFT_NODE_H

#include <hammurabi/raft/messages.h>
#include <hammurabi/raft/types.h>

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <variant>
#include <vector>

namespace hammurabi::raft {

struct config {
    node_id self = 0;
    std::vector<node_id> members;    // all nodes, including self
    int election_timeout_ticks = 15; // actual timeout is random in [t, 2t)
    int heartbeat_interval_ticks = 5;
    std::size_t max_append_bytes = 1300; // fits in one 1400-byte datagram (step 4.4)
    std::uint64_t random_seed = 0;
};

struct hard_state {
    term_t term = 0;
    std::optional<node_id> voted_for{};
    bool operator==(const hard_state&) const = default;
};

struct persistent_state {
    hard_state hard{};
    std::vector<log_entry> log{};
};

class storage {
public:
    storage() = default;
    storage(const storage&) = default;
    storage(storage&&) = default;
    storage& operator=(const storage&) = default;
    storage& operator=(storage&&) = default;
    virtual ~storage() = default;

    [[nodiscard]] virtual persistent_state load() = 0;           // at startup
    virtual void save_hard_state(const hard_state& state) = 0;   // term + vote
    virtual void append(std::span<const log_entry> entries) = 0; // after the current last entry
    virtual void truncate(log_index first_removed) = 0;          // remove this entry and all after it
};

class memory_storage final : public storage {
public:
    memory_storage() = default;
    explicit memory_storage(persistent_state initial); // tests can start nodes with a prepared log

    [[nodiscard]] persistent_state load() override;           // at startup
    void save_hard_state(const hard_state& state) override;   // term + vote
    void append(std::span<const log_entry> entries) override; // after the current last entry
    void truncate(log_index first_removed) override;          // remove this entry and all after it

private:
    persistent_state state_;
};

enum class role : std::uint8_t { follower, candidate, leader };
struct not_leader {
    std::optional<node_id> leader_hint{};
};

struct output {
    std::vector<envelope> messages{};
    std::vector<std::pair<log_index, log_entry>> committed{};
};

class node {
public:
    node(config cfg, storage* store); // loads persistent state from store

    void tick();
    void receive(const envelope& env);
    [[nodiscard]] std::expected<log_index, not_leader> propose(std::vector<std::byte> command); // step 4.1
    [[nodiscard]] output take_output();

    [[nodiscard]] role current_role() const;
    [[nodiscard]] term_t current_term() const;
    [[nodiscard]] std::optional<node_id> leader_hint() const;
    [[nodiscard]] log_index commit_index() const;
    [[nodiscard]] std::span<const log_entry> log() const;

private:
    struct follower_state {
        std::optional<node_id> leader;
    };
    struct candidate_state {
        std::set<node_id> votes;
    };
    struct leader_state {
        std::map<node_id, log_index> next_index;
        std::map<node_id, log_index> match_index;
    };

    void become_follower(term_t term, std::optional<node_id> leader);
    void become_candidate();
    void become_leader_if_won();
    void become_leader();
    void reset_election_timer();        // picks a new random timeout
    void send(node_id to, message msg); // appends to out_.messages
    void broadcast_heartbeat();
    [[nodiscard]] std::size_t majority() const { return cfg_.members.size() / 2 + 1; }
    [[nodiscard]] log_index last_log_index() const { return log_.size(); }
    [[nodiscard]] term_t term_at(log_index index) const; // 0 for index 0

    config cfg_;
    storage* storage_;
    std::mt19937_64 rng_;
    hard_state hard_;            // persistent
    std::vector<log_entry> log_; // persistent; entry with index i lives at log_[i - 1]
    log_index commit_index_ = 0; // volatile
    log_index handed_out_ = 0;   // volatile; "lastApplied" from the paper (see step 4.3)
    std::variant<follower_state, candidate_state, leader_state> role_;
    int election_elapsed_ = 0;
    int election_timeout_ = 0;
    int heartbeat_elapsed_ = 0;
    output out_;
};

} // namespace hammurabi::raft

#endif // HAMMURABI_RAFT_NODE_H