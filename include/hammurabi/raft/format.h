#ifndef HAMMURABI_RAFT_FORMAT_H
#define HAMMURABI_RAFT_FORMAT_H

#include <hammurabi/raft/messages.h>

#include <format>
#include <variant>

namespace hammurabi::raft::detail {

struct no_spec_formatter {
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }
};

} // namespace hammurabi::raft::detail

template<>
struct std::formatter<hammurabi::raft::request_vote> : hammurabi::raft::detail::no_spec_formatter {
    auto format(const hammurabi::raft::request_vote& m, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "RequestVote{{term={}, last={}/{}}}", m.term, m.last_log_index,
                              m.last_log_term);
    }
};

template<>
struct std::formatter<hammurabi::raft::request_vote_reply> : hammurabi::raft::detail::no_spec_formatter {
    auto format(const hammurabi::raft::request_vote_reply& m, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "RequestVoteReply{{term={}, {}}}", m.term, m.granted ? "granted" : "denied");
    }
};

template<>
struct std::formatter<hammurabi::raft::append_entries> : hammurabi::raft::detail::no_spec_formatter {
    auto format(const hammurabi::raft::append_entries& m, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "AppendEntries{{term={}, prev={}/{}, entries={}, commit={}}}", m.term,
                              m.prev_log_index, m.prev_log_term, m.entries.size(), m.leader_commit);
    }
};

template<>
struct std::formatter<hammurabi::raft::append_entries_reply> : hammurabi::raft::detail::no_spec_formatter {
    auto format(const hammurabi::raft::append_entries_reply& m, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "AppendEntriesReply{{term={}, {}, match={}, last={}}}", m.term,
                              m.success ? "ok" : "rejected", m.match_index, m.last_log_index);
    }
};

template<>
struct std::formatter<hammurabi::raft::message> : hammurabi::raft::detail::no_spec_formatter {
    auto format(const hammurabi::raft::message& m, std::format_context& ctx) const {
        return std::visit([&ctx](const auto& msg) { return std::format_to(ctx.out(), "{}", msg); }, m);
    }
};

template<>
struct std::formatter<hammurabi::raft::envelope> : hammurabi::raft::detail::no_spec_formatter {
    auto format(const hammurabi::raft::envelope& env, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}→{} {}", env.from, env.to, env.msg);
    }
};

#endif // HAMMURABI_RAFT_FORMAT_H
