#ifndef HAMMURABI_SERVER_H
#define HAMMURABI_SERVER_H

#include "detail/deadline_timer.h"

#include "connector.h"
#include "detail/rng.h"
#include "types.h"

#include "../../proto/raft.pb.h"

#include <boost/asio.hpp>

#include <memory>
#include <unordered_map>
#include <vector>

namespace hammurabi {

struct log_entry {
    explicit log_entry(term_t term) : term_received{term} {}
    term_t term_received;
};

enum class rpc_type : uint8_t {
    append_entry_request = 0x00,
    append_entry_response = 0x01,
    request_vote_request = 0x02,
    request_vote_response = 0x03
};

struct timeout;
struct append_entries_request_received;
struct append_entries_response_received;
struct request_vote_request_received;
struct request_vote_response_received;

class raft {
public:
    raft(boost::asio::io_service& io_service, unsigned short port, endpoint_map_t peers);

    virtual ~raft() = default;

private:
    template <typename T>
    void set_state();
    void send_message(const boost::asio::ip::udp::endpoint &endpoint, const google::protobuf::Message &message, rpc_type type);
    void send_request_votes();
    void send_request_vote_response(const endpoint_t &endpoint, bool vote_granted);
    void send_append_entries();
    void send_append_entries_response(const endpoint_t &endpoint, bool success);
    std::string persistent_state_filename() const;

    struct state {
        explicit state(raft& server) : server_{server} {}
        virtual ~state() = default;
        virtual void process_event(timeout const&) = 0;
        virtual void process_event(append_entries_request_received const& event) = 0;
        virtual void process_event(append_entries_response_received const& event) = 0;
        virtual void process_event(request_vote_request_received const& event) = 0;
        virtual void process_event(request_vote_response_received const& event) = 0;
    protected:
        raft& server_;
    };

    struct follower : public state {
        explicit follower(raft& server);
        ~follower() override = default;
        void process_event(timeout const&) override;
        void process_event(append_entries_request_received const& event) override;
        void process_event(append_entries_response_received const& event) override;
        void process_event(request_vote_request_received const& event) override;
        void process_event(request_vote_response_received const& event) override;
    };

    struct candidate : public state {
        explicit candidate(raft& server);
        ~candidate() override = default;
        void process_event(timeout const&) override;
        void process_event(append_entries_request_received const& event) override;
        void process_event(append_entries_response_received const& event) override;
        void process_event(request_vote_request_received const& event) override;
        void process_event(request_vote_response_received const& event) override;
    private:
        void start_new_election();
        bool has_majority() const;
        unsigned int votes_;
    };

    struct leader : public state {
        explicit leader(raft& server);
        ~leader() override = default;
        void process_event(timeout const&) override;
        void process_event(append_entries_request_received const& event) override;
        void process_event(append_entries_response_received const& event) override;
        void process_event(request_vote_request_received const& event) override;
        void process_event(request_vote_response_received const& event) override;
    };

    void load_persistent_state();
    void store_persistent_state();

    detail::deadline_timer timer_;
    connector conn_;
    detail::rng<unsigned int> election_timeout_;
    endpoint_map_t peers_;
    std::unique_ptr<state> current_state_;
    server_id_t server_id_;

    // Persistent state_ on all servers
    term_t current_term_;           // latest term server has seen
    server_id_t voted_for_;         // candidate's server id that received vote in current term; or 0 if none
    std::vector<log_entry> log_;    // log entries; first index is 1

    // Volatile state_ on all servers
    log_index_t commit_index_;      // index of highest log entry known to be committed
    log_index_t last_applied_;      // index of highest log entry applied to state machine

    // Volatile state_ on leaders
    std::unordered_map<server_id_t, log_index_t> next_index_;   // for each server, index of the next log entry to send
                                                                // to that server (initialized to leader last log
                                                                // index + 1)
    std::unordered_map<server_id_t, log_index_t> match_index_;  // for each server, index of highest log entry known to
                                                                // be replicated on server (initialized to 0, increases
                                                                // monotonically)
};

}

#endif // HAMMURABI_SERVER_H
