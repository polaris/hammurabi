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

using operation = std::variant<get, put, remove, cas>;

enum class status : std::uint8_t { ok = 1, not_found = 2, cas_failed = 3 };

struct result {
    status code = status::ok;
    std::optional<std::string> value{};
    bool operator==(const result&) const = default;
};

} // namespace hammurabi::kv

#endif // HAMMURABI_KV_COMMAND_H
