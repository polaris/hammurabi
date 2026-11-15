#ifndef HAMMURABI_KV_COMMAND_H
#define HAMMURABI_KV_COMMAND_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace hammurabi::kv {

struct get {
    std::string key;
    bool operator==(const get&) const = default;
};

struct put {
    std::string key;
    std::string value;
    bool operator==(const put&) const = default;
};

struct remove {
    std::string key;
    bool operator==(const remove&) const = default;
};

struct cas {
    std::string key;
    std::optional<std::string> expected;
    std::string desired;
    bool operator==(const cas&) const = default;
};

// Opens a client session; its answer is the client id (section 5.8).
struct register_client {
    bool operator==(const register_client&) const = default;
};

using operation = std::variant<get, put, remove, cas, register_client>;

// What the log stores: an operation plus the session fields that make retries safe.
// client_id 0 means "no session": allowed for get and register_client only.
struct command {
    std::uint64_t client_id = 0;
    std::uint64_t sequence = 0;
    operation op;
    bool operator==(const command&) const = default;
};

enum class status : std::uint8_t { ok = 1, not_found = 2, cas_failed = 3 };

struct result {
    status code = status::ok;
    std::optional<std::string> value{};
    bool operator==(const result&) const = default;
};

// The answer to register_client: the log index of its entry, the same on every node.
struct registered {
    std::uint64_t client_id = 0;
    bool operator==(const registered&) const = default;
};

// What an executed command answers.
using reply = std::variant<result, registered>;

// Why a request was refused without being executed.
enum class rejection : std::uint8_t { too_large = 1, malformed = 2, session_expired = 3 };

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_COMMAND_H
