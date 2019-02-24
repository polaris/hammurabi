#include "raft.h"

#include <boost/filesystem.hpp>
#include <boost/iostreams/device/mapped_file.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>

namespace hammurabi {

struct timeout {};

struct append_entries_request_received {
    explicit append_entries_request_received(proto::append_entries_request m) : message{std::move(m)} {}
    proto::append_entries_request message;
};

struct append_entries_response_received {
    explicit append_entries_response_received(proto::append_entries_response m) : message{std::move(m)} {}
    proto::append_entries_response message;
};

struct request_vote_request_received {
    explicit request_vote_request_received(proto::request_vote_request m) : message{std::move(m)} {}
    proto::request_vote_request message;
};

struct request_vote_response_received {
    explicit request_vote_response_received(proto::request_vote_response m) : message{std::move(m)} {}
    proto::request_vote_response message;
};

raft::raft(boost::asio::io_service& io_service, unsigned short port, endpoint_map_t peers)
: timer_{io_service, [this](){ current_state_->process_event(timeout{}); }}
, conn_{io_service, port}
, election_timeout_{150, 300}
, peers_{std::move(peers)}
, current_state_{new raft::follower{*this}}
, server_id_{port}
, current_term_{0}
, voted_for_{0}
, commit_index_{0}
, last_applied_{0} {
    load_persistent_state();

    if (log_.empty()) {
        log_.emplace_back(log_entry{0});
    }

    conn_.receive([this](uint8_t* input_data_, std::size_t bytes_received){
        const auto message_type = static_cast<rpc_type>(input_data_[0]);
        switch (message_type) {
            case rpc_type::append_entry_request: {
                proto::append_entries_request message;
                if (message.ParseFromArray(input_data_ + 1, static_cast<int>(bytes_received - 1))) {
                    current_state_->process_event(append_entries_request_received{message});
                }
                break;
            }
            case rpc_type::append_entry_response: {
                proto::append_entries_response message;
                if (message.ParseFromArray(input_data_ + 1, static_cast<int>(bytes_received - 1))) {
                    current_state_->process_event(append_entries_response_received{message});
                }
                break;
            }
            case rpc_type::request_vote_request: {
                proto::request_vote_request message;
                if (message.ParseFromArray(input_data_ + 1, static_cast<int>(bytes_received - 1))) {
                    current_state_->process_event(request_vote_request_received{message});
                }
                break;
            }
            case rpc_type::request_vote_response: {
                proto::request_vote_response message;
                if (message.ParseFromArray(input_data_ + 1, static_cast<int>(bytes_received - 1))) {
                    current_state_->process_event(request_vote_response_received{message});
                }
                break;
            }
        }
    });
}

template <typename T>
void raft::set_state() {
    current_state_.reset();
    current_state_.reset(new T{*this});
}

void raft::send_message(const boost::asio::ip::udp::endpoint &endpoint, const google::protobuf::Message &message, rpc_type type) {
    enum { max_length = 1024 };
    uint8_t buffer[max_length];
    bzero(buffer, max_length);
    buffer[0] = static_cast<uint8_t >(type);
    const std::size_t length = message.ByteSizeLong();
    message.SerializeToArray(buffer + 1, static_cast<int>(length));
    conn_.send(endpoint, buffer, length + 1);
}

void raft::send_request_votes() {
    for (const auto& peer : peers_) {
        proto::request_vote_request message;
        message.set_term(current_term_);
        message.set_candidate_id(server_id_);
        message.set_last_log_index(0);
        message.set_last_log_term(0);
        send_message(peer.second, message, rpc_type::request_vote_request);
    }
}

void raft::send_request_vote_response(const endpoint_t &endpoint, bool vote_granted) {
    proto::request_vote_response message;
    message.set_term(current_term_);
    message.set_vote_granted(vote_granted);
    send_message(endpoint, message, rpc_type::request_vote_response);
}

void raft::send_append_entries() {
    for (const auto& peer : peers_) {
        proto::append_entries_request message;
        message.set_term(current_term_);
        message.set_leader_id(server_id_);

        const uint32_t next_log_index = next_index_[peer.first];
        if (next_log_index <= log_.size()) {
            const uint32_t prev_log_index = next_log_index - 1;

            message.set_prev_log_index(prev_log_index);
            message.set_prev_log_term(log_[prev_log_index].term_received);

            proto::log_entry* const entry = message.add_entries();
            entry->set_term(log_[next_log_index].term_received);
            entry->set_index(next_log_index);
            entry->set_command("foo");
        } else {
            message.set_prev_log_index(0);
            message.set_prev_log_term(0);
        }

        message.set_leader_commit(commit_index_);
        send_message(peer.second, message, rpc_type::append_entry_request);
    }
}

void raft::send_append_entries_response(const endpoint_t &endpoint, bool success) {
    proto::append_entries_response message;
    message.set_term(current_term_);
    message.set_success(success);
    send_message(endpoint, message, rpc_type::append_entry_response);
}

// TODO: Move out of raft
void raft::load_persistent_state() {
    try {
        const std::string filename = persistent_state_filename();

        if (!boost::filesystem::exists(filename)) {
            return;
        }

        // TODO: Use memory-mapped file.
        std::ifstream ifs{filename, std::ios::binary | std::ios::ate};
        const long length = ifs.tellg();
        ifs.seekg(std::ifstream::beg);
        std::unique_ptr<uint8_t []> buffer{new uint8_t [length]};
        ifs.read(reinterpret_cast<char*>(buffer.get()), length);
        ifs.close();

        proto::persistent_state state;
        if (state.ParseFromArray(buffer.get(), static_cast<int>(length))) {
            current_term_ = state.current_term();
            voted_for_ = state.voted_for();
            // TODO: Load log entries
        }
    } catch (...) {
    }
}

// TODO: Move out of raft
void raft::store_persistent_state() {
    try {
        proto::persistent_state state;
        state.set_current_term(current_term_);
        state.set_voted_for(voted_for_);
        std::cout << current_term_ << ": " << voted_for_ << std::endl;
        for (const auto& entry : log_) {
            proto::log_entry* e = state.add_entries();
            e->set_term(entry.term_received);
            // TODO: set all fields
        }

        const std::size_t length = state.ByteSizeLong();
        std::unique_ptr<uint8_t []> buffer{new uint8_t [length]};
        state.SerializeToArray(buffer.get(), static_cast<int>(length));

        // TODO: Use memory-mapped file.
        std::ofstream ofs{persistent_state_filename(), std::ios::binary | std::ios::trunc};
        ofs.write(reinterpret_cast<const char*>(buffer.get()), length);
        ofs.close();
    } catch (...) {
    }
}

std::string raft::persistent_state_filename() const {
    std::stringstream ss;
    ss << server_id_ << ".state";
    return ss.str();
}

raft::follower::follower(raft& server)
: state(server) {
    server_.timer_.reset(boost::posix_time::milliseconds{server_.election_timeout_()});
    std::cout << "follower" << std::endl;
}

void raft::follower::process_event(timeout const&) { server_.set_state<candidate>(); }

void raft::follower::process_event(append_entries_request_received const &event) {
    if (event.message.term() > server_.current_term_) {
        server_.current_term_ = event.message.term();
        server_.voted_for_ = 0;
        server_.store_persistent_state();
    }
    if (event.message.term() == server_.current_term_) {

        if (event.message.leader_commit() > server_.commit_index_) {
            server_.commit_index_ = std::min(static_cast<uint32_t>(event.message.leader_commit()),
                                             static_cast<uint32_t>(server_.log_.size()));
            while (server_.commit_index_ > server_.last_applied_) {
                server_.last_applied_ += 1;
            }
        }

        server_.timer_.reset(boost::posix_time::milliseconds{server_.election_timeout_()});
        server_.send_append_entries_response(server_.peers_[event.message.leader_id()], true);
    } else {
        server_.send_append_entries_response(server_.peers_[event.message.leader_id()], false);
    }
}

void raft::follower::process_event(append_entries_response_received const &event) {
    if (event.message.term() > server_.current_term_) {
        server_.current_term_ = event.message.term();
        server_.voted_for_ = 0;
        server_.store_persistent_state();
    }
}

void raft::follower::process_event(request_vote_request_received const& event) {
    if (event.message.term() > server_.current_term_ || ((event.message.term() == server_.current_term_) && (server_.voted_for_ == 0))) {
        server_.current_term_ = event.message.term();
        server_.voted_for_ = event.message.candidate_id();
        server_.store_persistent_state();
        server_.send_request_vote_response(server_.peers_[event.message.candidate_id()], true);
    } else {
        server_.send_request_vote_response(server_.peers_[event.message.candidate_id()], false);
    }
}

void raft::follower::process_event(request_vote_response_received const &event) {
    if (event.message.term() > server_.current_term_) {
        server_.current_term_ = event.message.term();
        server_.voted_for_ = 0;
        server_.store_persistent_state();
    }
}

raft::candidate::candidate(raft& server)
: state(server)
, votes_{0} {
    start_new_election();
    std::cout << "candidate" << std::endl;
}

void raft::candidate::process_event(timeout const&) { start_new_election(); }

void raft::candidate::process_event(append_entries_request_received const &event) {
    if (event.message.term() >= server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    } else {
        server_.send_append_entries_response(server_.peers_[event.message.leader_id()], false);
    }
}

void raft::candidate::process_event(append_entries_response_received const &event) {
    if (event.message.term() > server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    }
}

void raft::candidate::process_event(request_vote_request_received const& event) {
    if (event.message.term() > server_.current_term_ || ((event.message.term() == server_.current_term_) && (server_.voted_for_ == 0))) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    } else {
        server_.send_request_vote_response(server_.peers_[event.message.candidate_id()], false);
    }
}

void raft::candidate::process_event(request_vote_response_received const& event) {
    if (event.message.term() > server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    } else if (event.message.vote_granted()) {
        votes_ += 1;
        if (has_majority()) {
            server_.set_state<leader>();
        }
    }
}

void raft::candidate::start_new_election() {
    server_.current_term_ += 1;
    server_.voted_for_ = server_.server_id_;
    server_.store_persistent_state();
    votes_ = 1;
    server_.send_request_votes();
    server_.timer_.reset(boost::posix_time::milliseconds{server_.election_timeout_()});
}

bool raft::candidate::has_majority() const {
    return votes_ >= floor(server_.peers_.size() / 2.0) + 1;
}

raft::leader::leader(raft& server)
: state(server) {
    for (auto const& peer : server_.peers_) {
        server_.next_index_[peer.first] = static_cast<hammurabi::log_index_t>(server_.log_.size() + 1);
        server_.match_index_[peer.first] = 0;
    }
    server_.timer_.reset(boost::posix_time::milliseconds{0});
    std::cout << "leader" << std::endl;
}

void raft::leader::process_event(timeout const&) {
    server_.send_append_entries();
    server_.timer_.reset(boost::posix_time::milliseconds{100});
}

void raft::leader::process_event(append_entries_request_received const &event) {
    if (event.message.term() >= server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    } else {
        server_.send_append_entries_response(server_.peers_[event.message.leader_id()], false);
    }
}

void raft::leader::process_event(append_entries_response_received const &event) {
    if (event.message.term() > server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    } else {
        if (event.message.success()) {
            // TODO: update next_match for this server.
            // TODO: update next_index for this server.
        } else {
            // TODO: decrease next_index for this server.
        }
    }
}

void raft::leader::process_event(request_vote_request_received const& event) {
    if (event.message.term() > server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    } else {
        server_.send_request_vote_response(server_.peers_[event.message.candidate_id()], false);
    }
}

void raft::leader::process_event(request_vote_response_received const &event) {
    if (event.message.term() > server_.current_term_) {
        server_.set_state<follower>();
        server_.current_state_->process_event(event);
    }
}

}   // namespace hammurabi
