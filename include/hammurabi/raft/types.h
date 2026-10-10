#ifndef HAMMURABI_RAFT_TYPES_H
#define HAMMURABI_RAFT_TYPES_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hammurabi::raft {

using node_id = std::uint32_t;
using term_t = std::uint64_t;
using log_index = std::uint64_t; // first entry has index 1; index 0 means "before the first entry"

enum class entry_kind : std::uint8_t { noop = 1, command = 2 };

struct log_entry {
    term_t term = 0;
    entry_kind kind = entry_kind::command;
    std::vector<std::byte> data{}; // the encoded kv command; empty for noop
    bool operator==(const log_entry&) const = default;
};

} // namespace hammurabi::raft

#endif // HAMMURABI_RAFT_TYPES_H
